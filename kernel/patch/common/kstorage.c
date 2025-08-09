/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 bmax121. All Rights Reserved.
 *
 * RCU-protected kstorage with sorted-array + binary search (COW writers, lockless readers).
 */

#include <kstorage.h>
#include <kputils.h>

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define KSTRORAGE_MAX_GROUP_NUM 4

/* 元素：发布后只读；通过 RCU 回收 */
struct kstorage {
    int   gid;
    long  did;
    int   dlen;
    struct rcu_head rcu;   /* 用于元素回收 */
    char  data[];          /* 柔性数组 */
};

/* 组的不可变快照（有序指针数组） */
struct ks_group_snapshot {
    int size;
    struct kstorage **items;   /* 长度为 size */
    struct rcu_head rcu;       /* 用于快照回收 */
};

/* 每个组：writer 用自旋锁，reader 走 RCU */
struct ks_group {
    spinlock_t lock;                /* 串行化 writer */
    struct ks_group_snapshot __rcu *snap; /* RCU 指向当前快照 */
};

static int used_max_group = -1;
static struct ks_group groups[KSTRORAGE_MAX_GROUP_NUM];

/* ---------- utils ---------- */

static int ks_bsearch(struct ks_group_snapshot *s, long did)
{
    int lo = 0, hi = s->size - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        long mdid = s->items[mid]->did;
        if (mdid == did) return mid;
        if (mdid < did)  lo = mid + 1;
        else             hi = mid - 1;
    }
    /* 未命中，返回插入点的负编码 */
    return -(lo + 1);
}

static void ks_free_snapshot_rcu(struct rcu_head *rcu)
{
    struct ks_group_snapshot *s = container_of(rcu, struct ks_group_snapshot, rcu);
    kvfree(s->items);
    kfree(s);
}

static void ks_free_kstorage_rcu(struct rcu_head *rcu)
{
    struct kstorage *ks = container_of(rcu, struct kstorage, rcu);
    kvfree(ks);
}

/* 复制当前快照为“可写副本”，容量 = new_size */
static struct ks_group_snapshot *ks_clone_snapshot(struct ks_group_snapshot *old, int new_size)
{
    struct ks_group_snapshot *ns = kzalloc(sizeof(*ns), GFP_KERNEL);
    if (!ns) return NULL;

    ns->size = new_size;
    if (new_size > 0) {
        size_t bytes = sizeof(struct kstorage *) * new_size;
        ns->items = kvmalloc(bytes, GFP_KERNEL);
        if (!ns->items) {
            kfree(ns);
            return NULL;
        }
        if (old && old->size > 0) {
            /* 先拷到相同位置，调用者可再做插入/删除/替换调整 */
            memcpy(ns->items, old->items, sizeof(struct kstorage *) *
                                         min(old->size, new_size));
        }
    }
    return ns;
}

/* 创建（并填充数据）一个新的 kstorage */
static struct kstorage *ks_make_item(int gid, long did, const void *data, int offset, int len, bool from_user)
{
    if (len < 0) return ERR_PTR(-EINVAL);

    struct kstorage *item = kvmalloc(sizeof(struct kstorage) + len, GFP_KERNEL);
    if (!item) return ERR_PTR(-ENOMEM);

    item->gid  = gid;
    item->did  = did;
    item->dlen = len;

    if (len > 0) {
        if (from_user) {
            if (copy_from_user(item->data, (const char __user *)data + offset, len)) {
                kvfree(item);
                return ERR_PTR(-EFAULT);
            }
        } else {
            memcpy(item->data, (const char *)data + offset, len);
        }
    }
    return item;
}

/* ---------- API ---------- */

int try_alloc_kstroage_group(void)
{
    int id = ++used_max_group;
    if (id < 0 || id >= KSTRORAGE_MAX_GROUP_NUM)
        return -1;

    spin_lock_init(&groups[id].lock);

    /* 建一个空快照，便于读路径简化分支 */
    struct ks_group_snapshot *empty = kzalloc(sizeof(*empty), GFP_KERNEL);
    if (!empty) {
        used_max_group--;
        return -ENOMEM;
    }
    empty->size = 0;
    empty->items = NULL;
    rcu_assign_pointer(groups[id].snap, empty);
    return id;
}
KP_EXPORT_SYMBOL(try_alloc_kstroage_group);

int kstorage_group_size(int gid)
{
    int sz = -ENOENT;
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    rcu_read_lock();
    struct ks_group_snapshot *s = rcu_dereference(groups[gid].snap);
    sz = s ? s->size : 0;
    rcu_read_unlock();
    return sz;
}
KP_EXPORT_SYMBOL(kstorage_group_size);

int write_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    struct kstorage *new_item = ks_make_item(gid, did, data, offset, len, data_is_user);
    if (IS_ERR(new_item)) return PTR_ERR(new_item);

    spin_lock(&groups[gid].lock);

    /* 取当前快照（在锁下，用 *_protected 保证与 RCU 匹配） */
    struct ks_group_snapshot *old = rcu_dereference_protected(groups[gid].snap, lockdep_is_held(&groups[gid].lock));
    int idx = old ? ks_bsearch(old, did) : -1;

    struct ks_group_snapshot *ns = NULL;
    struct kstorage *old_item = NULL;

    if (idx >= 0) {
        /* 替换 */
        ns = ks_clone_snapshot(old, old->size);
        if (!ns) {
            spin_unlock(&groups[gid].lock);
            kvfree(new_item);
            return -ENOMEM;
        }
        old_item = ns->items[idx];
        ns->items[idx] = new_item;
    } else {
        /* 插入 */
        int ins = -idx - 1;
        ns = ks_clone_snapshot(old, old->size + 1);
        if (!ns) {
            spin_unlock(&groups[gid].lock);
            kvfree(new_item);
            return -ENOMEM;
        }
        /* 右移腾位置 */
        if (old->size - ins > 0) {
            memmove(&ns->items[ins + 1], &ns->items[ins],
                    sizeof(struct kstorage *) * (old->size - ins));
        }
        ns->items[ins] = new_item;
    }

    rcu_assign_pointer(groups[gid].snap, ns);
    spin_unlock(&groups[gid].lock);

    /* 延迟回收旧快照 & 旧元素（若替换） */
    if (old) call_rcu(&old->rcu, ks_free_snapshot_rcu);
    if (old_item) call_rcu(&old_item->rcu, ks_free_kstorage_rcu);

    return 0;
}
KP_EXPORT_SYMBOL(write_kstorage);

const struct kstorage *get_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return NULL;

    rcu_read_lock();
    struct ks_group_snapshot *s = rcu_dereference(groups[gid].snap);
    if (!s) { rcu_read_unlock(); return NULL; }

    int idx = ks_bsearch(s, did);
    const struct kstorage *ret = (idx >= 0) ? s->items[idx] : NULL;
    /* 注意：返回的指针仅在调用方的 RCU 读段内安全 */
    rcu_read_unlock();
    return ret;
}
KP_EXPORT_SYMBOL(get_kstorage);

int on_each_kstorage_elem(int gid, on_kstorage_cb cb, void *udata)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    int rc = 0;
    rcu_read_lock();
    struct ks_group_snapshot *s = rcu_dereference(groups[gid].snap);
    if (s) {
        for (int i = 0; i < s->size; i++) {
            rc = cb(s->items[i], udata);
            if (rc) break;
        }
    }
    rcu_read_unlock();
    return rc;
}
KP_EXPORT_SYMBOL(on_each_kstorage_elem);

int read_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    int rc = 0;

    rcu_read_lock();
    struct ks_group_snapshot *s = rcu_dereference(groups[gid].snap);
    if (!s) { rcu_read_unlock(); return -ENOENT; }

    int idx = ks_bsearch(s, did);
    if (idx < 0) { rcu_read_unlock(); return -ENOENT; }

    const struct kstorage *ks = s->items[idx];

    if (offset < 0 || offset >= ks->dlen) {
        rcu_read_unlock();
        return -EINVAL;
    }
    int n = ks->dlen - offset;
    if (n > len) n = len;

    if (data_is_user) {
        if (copy_to_user((void __user *)data, ks->data + offset, n))
            rc = -EFAULT;
    } else {
        memcpy(data, ks->data + offset, n);
    }
    rcu_read_unlock();
    return rc;
}
KP_EXPORT_SYMBOL(read_kstorage);

int list_kstorage_ids(int gid, long *ids, int idslen, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    int cnt = 0;
    rcu_read_lock();
    struct ks_group_snapshot *s = rcu_dereference(groups[gid].snap);
    if (s) {
        cnt = min(idslen, s->size);
        if (data_is_user) {
            /* 按 did 升序输出 */
            for (int i = 0; i < cnt; i++) {
                long did = s->items[i]->did;
                if (copy_to_user(&ids[i], &did, sizeof(long))) {
                    cnt = -EFAULT;
                    break;
                }
            }
        } else {
            for (int i = 0; i < cnt; i++) {
                ids[i] = s->items[i]->did;
            }
        }
    }
    rcu_read_unlock();
    return cnt;
}
KP_EXPORT_SYMBOL(list_kstorage_ids);

int remove_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM) return -ENOENT;

    spin_lock(&groups[gid].lock);

    struct ks_group_snapshot *old = rcu_dereference_protected(groups[gid].snap, lockdep_is_held(&groups[gid].lock));
    int idx = old ? ks_bsearch(old, did) : -1;
    if (idx < 0) {
        spin_unlock(&groups[gid].lock);
        return -ENOENT;
    }

    struct ks_group_snapshot *ns = ks_clone_snapshot(old, old->size - 1);
    if (!ns) {
        spin_unlock(&groups[gid].lock);
        return -ENOMEM;
    }

    struct kstorage *victim = old->items[idx];

    /* 左段保持，右段左移 */
    if (idx > 0)
        memcpy(&ns->items[0], &old->items[0], sizeof(struct kstorage *) * idx);
    if (old->size - idx - 1 > 0)
        memcpy(&ns->items[idx], &old->items[idx + 1],
               sizeof(struct kstorage *) * (old->size - idx - 1));

    rcu_assign_pointer(groups[gid].snap, ns);
    spin_unlock(&groups[gid].lock);

    if (old) call_rcu(&old->rcu, ks_free_snapshot_rcu);
    if (victim) call_rcu(&victim->rcu, ks_free_kstorage_rcu);

    return 0;
}
KP_EXPORT_SYMBOL(remove_kstorage);

int kstorage_init(void)
{
    for (int i = 0; i < KSTRORAGE_MAX_GROUP_NUM; i++) {
        spin_lock_init(&groups[i].lock);
        RCU_INIT_POINTER(groups[i].snap, NULL);
    }
    used_max_group = -1;
    return 0;
}
KP_EXPORT_SYMBOL(kstorage_init);
