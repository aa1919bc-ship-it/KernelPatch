/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 bmax121. All Rights Reserved.
 */

#include <kstorage.h>
#include <kputils.h>

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define KSTRORAGE_MAX_GROUP_NUM 4

struct kstorage_group {
    struct kstorage **items;
    int size;
    int capacity;
};

static int used_max_group = -1;
static struct kstorage_group groups[KSTRORAGE_MAX_GROUP_NUM];

static int last_gid = -1;
static long last_did = 0;
static struct kstorage *last_item = NULL;

static int ks_binary_search(struct kstorage_group *g, long did)
{
    int low = 0;
    int high = g->size - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        long mdid = g->items[mid]->did;
        if (mdid == did)
            return mid;
        if (mdid < did)
            low = mid + 1;
        else
            high = mid - 1;
    }
    return -(low + 1);
}

int try_alloc_kstroage_group()
{
    used_max_group++;
    if (used_max_group < 0 || used_max_group >= KSTRORAGE_MAX_GROUP_NUM)
        return -1;
    return used_max_group;
}

int kstorage_group_size(int gid)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return -ENOENT;
    return groups[gid].size;
}

int write_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    (void)data_is_user;
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return -ENOENT;

    struct kstorage_group *g = &groups[gid];
    struct kstorage *new = malloc(sizeof(struct kstorage) + len);
    if (!new)
        return -ENOMEM;

    new->gid = gid;
    new->did = did;
    new->dlen = len;
    memcpy(new->data, ((char *)data) + offset, len);

    int idx = ks_binary_search(g, did);
    if (idx >= 0) {
        struct kstorage *old = g->items[idx];
        g->items[idx] = new;
        if (last_gid == gid && last_did == did)
            last_item = new;
        free(old);
    } else {
        idx = -idx - 1;
        if (g->size == g->capacity) {
            int newcap = g->capacity ? g->capacity * 2 : 4;
            struct kstorage **tmp = realloc(g->items, newcap * sizeof(*tmp));
            if (!tmp) {
                free(new);
                return -ENOMEM;
            }
            g->items = tmp;
            g->capacity = newcap;
        }
        memmove(&g->items[idx + 1], &g->items[idx],
                (g->size - idx) * sizeof(g->items[0]));
        g->items[idx] = new;
        g->size++;
    }
    return 0;
}
KP_EXPORT_SYMBOL(write_kstorage);

const struct kstorage *get_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return NULL;
    if (gid == last_gid && did == last_did)
        return last_item;

    struct kstorage_group *g = &groups[gid];
    int idx = ks_binary_search(g, did);
    if (idx < 0)
        return NULL;

    last_gid = gid;
    last_did = did;
    last_item = g->items[idx];
    return last_item;
}
KP_EXPORT_SYMBOL(get_kstorage);

int on_each_kstorage_elem(int gid, on_kstorage_cb cb, void *udata)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return -ENOENT;

    struct kstorage_group *g = &groups[gid];
    for (int i = 0; i < g->size; i++) {
        int rc = cb(g->items[i], udata);
        if (rc)
            return rc;
    }
    return 0;
}
KP_EXPORT_SYMBOL(on_each_kstorage_elem);

int read_kstorage(int gid, long did, void *data, int offset, int len, bool data_is_user)
{
    int rc = 0;
    const struct kstorage *pos = get_kstorage(gid, did);
    if (!pos)
        return -ENOENT;

    if (offset >= pos->dlen)
        return -EINVAL;

    int min_len = pos->dlen - offset;
    if (min_len > len)
        min_len = len;

    if (data_is_user) {
        int cplen = compat_copy_to_user(data, pos->data + offset, min_len);
        if (cplen <= 0)
            rc = cplen;
    } else {
        memcpy(data, pos->data + offset, min_len);
    }
    return rc;
}
KP_EXPORT_SYMBOL(read_kstorage);

int list_kstorage_ids(int gid, long *ids, int idslen, bool data_is_user)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return -ENOENT;

    struct kstorage_group *g = &groups[gid];
    int cnt = 0;
    for (; cnt < g->size && cnt < idslen; cnt++) {
        if (data_is_user) {
            int cplen = compat_copy_to_user(ids + cnt,
                                            &g->items[cnt]->did,
                                            sizeof(long));
            if (cplen <= 0) {
                cnt = cplen;
                break;
            }
        } else {
            ids[cnt] = g->items[cnt]->did;
        }
    }
    return cnt;
}
KP_EXPORT_SYMBOL(list_kstorage_ids);

int remove_kstorage(int gid, long did)
{
    if (gid < 0 || gid >= KSTRORAGE_MAX_GROUP_NUM)
        return -ENOENT;

    struct kstorage_group *g = &groups[gid];
    int idx = ks_binary_search(g, did);
    if (idx < 0)
        return -ENOENT;

    struct kstorage *old = g->items[idx];
    memmove(&g->items[idx], &g->items[idx + 1],
            (g->size - idx - 1) * sizeof(g->items[0]));
    g->size--;

    if (last_gid == gid && last_did == did) {
        last_gid = -1;
        last_item = NULL;
    }

    free(old);
    return 0;
}
KP_EXPORT_SYMBOL(remove_kstorage);

int kstorage_init()
{
    for (int i = 0; i < KSTRORAGE_MAX_GROUP_NUM; i++) {
        groups[i].items = NULL;
        groups[i].size = 0;
        groups[i].capacity = 0;
    }
    used_max_group = -1;
    last_gid = -1;
    last_item = NULL;
    return 0;
}

