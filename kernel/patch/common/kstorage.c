#include <kstorage.h>

#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/spinlock.h>
#include <compiler.h>
#include <stdbool.h>
#include <symbol.h>
#include <uapi/asm-generic/errno.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <kputils.h>

#define KSTRORAGE_MAX_GROUP_NUM 4

// static atomic64_t used_max_group = ATOMIC_INIT(0);
static int used_max_group = -1;
static struct xarray kstorage_xa[KSTRORAGE_MAX_GROUP_NUM];
static atomic_t group_sizes[KSTRORAGE_MAX_GROUP_NUM];
static spinlock_t used_max_group_lock;

static void reclaim_callback(struct rcu_head *rcu)
{
    struct kstorage *ks = container_of(rcu, struct kstorage, rcu);
    kvfree(ks);
}

int try_alloc_kstroage_group()
{
    spin_lock(&used_max_group_lock);
    used_max_group++;
    if (used_max_group < 0 || used_max_group >= KSTRORAGE_MAX_GROUP_NUM) return -1;
    spin_unlock(&used_max_group_lock);
    return used_max_group;
}

int kstorage_group_size(int gid)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;
    return atomic_read(&group_sizes[gid]);
}

int write_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    struct kstorage *old;
    struct kstorage *new = (struct kstorage *)vmalloc(sizeof(struct kstorage) + len);
    if (!new) return -ENOMEM;
    new->gid = gid;
    new->did = did;
    new->dlen = 0;
    if (data_is_user) {
        void *drc = memdup_user(data + offset, len);
        if (IS_ERR(drc)) {
            kvfree(new);
            return PTR_ERR(drc);
        }
        memcpy(new->data, drc, len);
        kvfree(drc);
    } else {
        memcpy(new->data, data + offset, len);
    }
    new->dlen = len;

    xa_lock(&kstorage_xa[gid]);
    old = __xa_store(&kstorage_xa[gid], did, new, GFP_KERNEL);
    int xerr = xa_err(old);
    if (xerr) {
        xa_unlock(&kstorage_xa[gid]);
        kvfree(new);
        return xerr;
    }
    if (!old)
        atomic_inc(&group_sizes[gid]);
    xa_unlock(&kstorage_xa[gid]);

    if (old) {
        bool async = true;
        if (async) {
            call_rcu(&old->rcu, reclaim_callback);
        } else {
            synchronize_rcu();
            kvfree(old);
        }
    }
    return 0;
}
KP_EXPORT_SYMBOL(write_kstorage);

const struct kstorage *get_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return ERR_PTR(-ENOENT);

    struct kstorage *pos = xa_load(&kstorage_xa[gid], did);
    if (!pos) return ERR_PTR(-ENOENT);
    return pos;
}
KP_EXPORT_SYMBOL(get_kstorage);

int on_each_kstorage_elem(int gid, on_kstorage_cb cb, void *udata)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    int rc = 0;
    unsigned long index = 0;
    struct kstorage *pos;

    rcu_read_lock();

    xa_for_each(&kstorage_xa[gid], index, pos)
    {
        rc = cb(pos, udata);
        if (rc) break;
    }

    rcu_read_unlock();

    return rc;
}
KP_EXPORT_SYMBOL(on_each_kstorage_elem);

int read_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    int rc = 0;
    rcu_read_lock();

    const struct kstorage *pos = get_kstorage(gid, did);

    if (IS_ERR(pos)) {
        rcu_read_unlock();
        return PTR_ERR(pos);
    }

    int min_len = pos->dlen - offset > len ? len : pos->dlen - offset;

    if (data_is_user) {
        int cplen = compat_copy_to_user(data, pos->data + offset, min_len);
        if (cplen <= 0) {
            logkfe("compat_copy_to_user error: %d", cplen);
            rc = cplen;
        }
    } else {
        memcpy(data, pos->data + offset, min_len);
    }

    rcu_read_unlock();
    return rc;
}
KP_EXPORT_SYMBOL(read_kstorage);

int list_kstorage_ids(int gid, long *ids, int idslen, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    int cnt = 0;
    unsigned long index = 0;
    struct kstorage *pos;

    rcu_read_lock();

    xa_for_each(&kstorage_xa[gid], index, pos)
    {
        if (cnt >= idslen) break;

        if (data_is_user) {
            int cplen = compat_copy_to_user(ids + cnt, &pos->did, sizeof(pos->did));
            if (cplen <= 0) {
                logkfe("compat_copy_to_user error: %d", cplen);
                cnt = cplen;
            }
        } else {
            memcpy(ids + cnt, &pos->did, sizeof(pos->did));
        }
        cnt++;
    }

    rcu_read_unlock();

    return cnt;
}
KP_EXPORT_SYMBOL(list_kstorage_ids);

int remove_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    struct kstorage *pos;

    xa_lock(&kstorage_xa[gid]);
    pos = __xa_erase(&kstorage_xa[gid], did);
    if (pos)
        atomic_dec(&group_sizes[gid]);
    xa_unlock(&kstorage_xa[gid]);

    if (!pos)
        return -ENOENT;

    bool async = true;
    if (async) {
        call_rcu(&pos->rcu, reclaim_callback);
    } else {
        synchronize_rcu();
        kvfree(pos);
    }
    return 0;
}
KP_EXPORT_SYMBOL(remove_kstorage);

int kstorage_init()
{
    for (int i = 0; i < KSTRORAGE_MAX_GROUP_NUM; i++) {
        xa_init(&kstorage_xa[i]);
        atomic_set(&group_sizes[i], 0);
    }
    spin_lock_init(&used_max_group_lock);

    return 0;
}
