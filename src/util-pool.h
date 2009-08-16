/* Copyright (c) 2008 by Victor Julien <victor@inliniac.net> */

#ifndef __UTIL_POOL_H__
#define __UTIL_POOL_H__

/* pool bucket structure */
typedef struct PoolBucket_ {
    void *data;
    struct PoolBucket_ *next;
} PoolBucket;

/* pool structure */
typedef struct Pool_ {
    uint32_t max_buckets;
    uint32_t allocated;

    PoolBucket *alloc_list;
    uint32_t alloc_list_size;

    PoolBucket *empty_list;
    uint32_t empty_list_size;

    void *(*Alloc)(void *);
    void *AllocData;
    void (*Free)(void *);
} Pool;

/* prototypes */
Pool* PoolInit(uint32_t, uint32_t, void *(*Alloc)(void *), void *, void (*Free)(void *));
void PoolFree(Pool *);
void PoolPrint(Pool *);

void *PoolGet(Pool *);
void PoolReturn(Pool *, void *);

void PoolRegisterTests(void);

#endif /* __UTIL_POOL_H__ */

