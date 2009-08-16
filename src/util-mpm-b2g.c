/* Implementation of the SBNDMq pattern matching algorithm.
 *
 * Copyright (c) 2009 Victor Julien <victor@inliniac.net>
 *
 * Ideas:
 *  - B2g does a full match in the scan phase of up to 'm' characters,
 *    in case of a case insensitive search we could say it's match if
 *    the pattern is of len 'm' or just compare the rest of the chars.
 *
 * TODO:
 *  - Try to get the S0 calculation right.
 *
 */

//#define PRINTMATCH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>

#include "eidps.h"
#include "detect.h"
#include "util-bloomfilter.h"
#include "util-mpm-b2g.h"
#include "util-print.h"

#include "util-unittest.h"

#define INIT_HASH_SIZE 65536

#ifdef B2G_COUNTERS
#define COUNT(counter) \
        (counter)
#else
#define COUNT(counter)
#endif /* B2G_COUNTERS */

void B2gInitCtx (MpmCtx *);
void B2gThreadInitCtx(MpmCtx *, MpmThreadCtx *, uint32_t);
void B2gDestroyCtx(MpmCtx *);
void B2gThreadDestroyCtx(MpmCtx *, MpmThreadCtx *);
int B2gAddScanPatternCI(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, uint8_t);
int B2gAddScanPatternCS(MpmCtx *, uint8_t *, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, uint8_t);
int B2gAddPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen, uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid);
int B2gAddPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen, uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid);
int B2gPreparePatterns(MpmCtx *mpm_ctx);
uint32_t B2gScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
#ifdef B2G_SCAN2
uint32_t B2gScan2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
#endif
uint32_t B2gScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t B2gScanBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen);
uint32_t B2gSearch1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
uint32_t B2gSearchBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen);
uint32_t B2gSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, uint8_t *buf, uint16_t buflen);
void B2gPrintInfo(MpmCtx *mpm_ctx);
void B2gPrintSearchStats(MpmThreadCtx *mpm_thread_ctx);
void B2gRegisterTests(void);

void MpmB2gRegister (void) {
    mpm_table[MPM_B2G].name = "b2g";
    mpm_table[MPM_B2G].InitCtx = B2gInitCtx;
    mpm_table[MPM_B2G].InitThreadCtx = B2gThreadInitCtx;
    mpm_table[MPM_B2G].DestroyCtx = B2gDestroyCtx;
    mpm_table[MPM_B2G].DestroyThreadCtx = B2gThreadDestroyCtx;
    mpm_table[MPM_B2G].AddScanPattern = B2gAddScanPatternCS;
    mpm_table[MPM_B2G].AddScanPatternNocase = B2gAddScanPatternCI;
    mpm_table[MPM_B2G].AddPattern = B2gAddPatternCS;
    mpm_table[MPM_B2G].AddPatternNocase = B2gAddPatternCI;
    mpm_table[MPM_B2G].Prepare = B2gPreparePatterns;
    mpm_table[MPM_B2G].Scan = B2G_SCANFUNC; /* default to B2gSearch. We may fall back to 1 */
    mpm_table[MPM_B2G].Search = B2G_SEARCHFUNC; /* default to B2gSearch. We may fall back to 1 */
    mpm_table[MPM_B2G].Cleanup = MpmMatchCleanup;
    mpm_table[MPM_B2G].PrintCtx = B2gPrintInfo;
    mpm_table[MPM_B2G].PrintThreadCtx = B2gPrintSearchStats;
    mpm_table[MPM_B2G].RegisterUnittests = B2gRegisterTests;
}

/* append an endmatch to a pattern
 *
 * Only used in the initialization phase */
static inline void B2gEndMatchAppend(MpmCtx *mpm_ctx, B2gPattern *p,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid,
    uint8_t nosearch)
{
    MpmEndMatch *em = MpmAllocEndMatch(mpm_ctx);
    if (em == NULL) {
        printf("ERROR: B2gAllocEndMatch failed\n");
        return;
    }

    em->id = pid;
    em->sig_id = sid;
    em->depth = depth;
    em->offset = offset;

    if (nosearch)
        em->flags |= MPM_ENDMATCH_NOSEARCH;

    if (p->em == NULL) {
        p->em = em;
        return;
    }

    MpmEndMatch *m = p->em;
    while (m->next) {
        m = m->next;
    }
    m->next = em;
}

/** \todo XXX Unused??? */
#if 0
static void prt (uint8_t *buf, uint16_t buflen) {
    uint16_t i;

    for (i = 0; i < buflen; i++) {
        if (isprint(buf[i])) printf("%c", buf[i]);
        else                 printf("\\x%02X", buf[i]);
    }
    //printf("\n");
}
#endif

void B2gPrintInfo(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;

    printf("MPM B2g Information:\n");
    printf("Memory allocs:   %" PRIu32 "\n", mpm_ctx->memory_cnt);
    printf("Memory alloced:  %" PRIu32 "\n", mpm_ctx->memory_size);
    printf(" Sizeofs:\n");
    printf("  MpmCtx         %" PRIuMAX "\n", (uintmax_t)sizeof(MpmCtx));
    printf("  B2gCtx:         %" PRIuMAX "\n", (uintmax_t)sizeof(B2gCtx));
    printf("  B2gPattern      %" PRIuMAX "\n", (uintmax_t)sizeof(B2gPattern));
    printf("  B2gHashItem     %" PRIuMAX "\n", (uintmax_t)sizeof(B2gHashItem));
    printf("Unique Patterns: %" PRIu32 "\n", mpm_ctx->pattern_cnt);
    printf("Scan Patterns:   %" PRIu32 "\n", mpm_ctx->scan_pattern_cnt);
    printf("Total Patterns:  %" PRIu32 "\n", mpm_ctx->total_pattern_cnt);
    printf("Smallest:        %" PRIu32 "\n", mpm_ctx->scan_minlen);
    printf("Largest:         %" PRIu32 "\n", mpm_ctx->scan_maxlen);
    printf("Hash size:       %" PRIu32 "\n", ctx->scan_hash_size);
    printf("\n");
}

static inline B2gPattern *B2gAllocPattern(MpmCtx *mpm_ctx) {
    B2gPattern *p = malloc(sizeof(B2gPattern));
    if (p == NULL) {
        printf("ERROR: B2gAllocPattern: malloc failed\n");
    }
    memset(p,0,sizeof(B2gPattern));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gPattern);
    return p;
}

static inline B2gHashItem *
B2gAllocHashItem(MpmCtx *mpm_ctx) {
    B2gHashItem *hi = malloc(sizeof(B2gHashItem));
    if (hi == NULL) {
        printf("ERROR: B2gAllocHashItem: malloc failed\n");
    }
    memset(hi,0,sizeof(B2gHashItem));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gHashItem);
    return hi;
}

static void B2gHashFree(MpmCtx *mpm_ctx, B2gHashItem *hi) {
    if (hi == NULL)
        return;

    B2gHashItem *t = hi->nxt;
    B2gHashFree(mpm_ctx, t);

    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B2gHashItem);
    free(hi);
}

static inline void memcpy_tolower(uint8_t *d, uint8_t *s, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        d[i] = u8_tolower(s[i]);
    }
}

/*
 * INIT HASH START
 */
static inline uint32_t B2gInitHash(B2gPattern *p) {
    uint32_t hash = p->len * p->cs[0];
    if (p->len > 1)
        hash += p->cs[1];

    return (hash % INIT_HASH_SIZE);
}

static inline uint32_t B2gInitHashRaw(uint8_t *pat, uint16_t patlen) {
    uint32_t hash = patlen * pat[0];
    if (patlen > 1)
        hash += pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline int B2gInitHashAdd(B2gCtx *ctx, B2gPattern *p) {
    uint32_t hash = B2gInitHash(p);

    //printf("B2gInitHashAdd: %" PRIu32 "\n", hash);

    if (ctx->init_hash[hash] == NULL) {
        ctx->init_hash[hash] = p;
        //printf("B2gInitHashAdd: hash %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);
        return 0;
    }

    B2gPattern *t = ctx->init_hash[hash], *tt = NULL;
    for ( ; t != NULL; t = t->next) {
        tt = t;
    }
    tt->next = p;
    //printf("B2gInitHashAdd: hash %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);

    return 0;
}

static inline int B2gCmpPattern(B2gPattern *p, uint8_t *pat, uint16_t patlen, char nocase);

static inline B2gPattern *B2gInitHashLookup(B2gCtx *ctx, uint8_t *pat, uint16_t patlen, char nocase) {
    uint32_t hash = B2gInitHashRaw(pat,patlen);

    //printf("B2gInitHashLookup: %" PRIu32 ", head %p\n", hash, ctx->init_hash[hash]);

    if (ctx->init_hash[hash] == NULL) {
        return NULL;
    }

    B2gPattern *t = ctx->init_hash[hash];
    for ( ; t != NULL; t = t->next) {
        if (B2gCmpPattern(t,pat,patlen,nocase) == 1)
            return t;
    }

    return NULL;
}

static inline int B2gCmpPattern(B2gPattern *p, uint8_t *pat, uint16_t patlen, char nocase) {
    if (p->len != patlen)
        return 0;

    if (!((nocase && p->flags & B2G_NOCASE) || (!nocase && !(p->flags & B2G_NOCASE))))
        return 0;

    if (memcmp(p->cs, pat, patlen) != 0)
        return 0;

    return 1;
}

/*
 * INIT HASH END
 */

void B2gFreePattern(MpmCtx *mpm_ctx, B2gPattern *p) {
    if (p && p->em) {
        MpmEndMatchFreeAll(mpm_ctx, p->em);
    }

    if (p && p->cs && p->cs != p->ci) {
        free(p->cs);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p && p->ci) {
        free(p->ci);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= p->len;
    }

    if (p) {
        free(p);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= sizeof(B2gPattern); 
    }
}

/* B2gAddPattern
 *
 * pat: ptr to the pattern
 * patlen: length of the pattern
 * nocase: nocase flag: 1 enabled, 0 disable
 * pid: pattern id
 * sid: signature id (internal id)
 */
static inline int B2gAddPattern(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen, uint16_t offset, uint16_t depth, char nocase, char scan, uint32_t pid, uint32_t sid, uint8_t nosearch) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;

//    printf("B2gAddPattern: ctx %p \"", mpm_ctx); prt(pat, patlen);
//    printf("\" id %" PRIu32 ", nocase %s\n", id, nocase ? "true" : "false");

    if (patlen == 0)
        return 0;

    /* get a memory piece */
    B2gPattern *p = B2gInitHashLookup(ctx, pat, patlen, nocase);
    if (p == NULL) {
//        printf("B2gAddPattern: allocing new pattern\n");
        p = B2gAllocPattern(mpm_ctx);
        if (p == NULL)
            goto error;

        p->len = patlen;

        if (nocase) p->flags |= B2G_NOCASE;

        /* setup the case insensitive part of the pattern */
        p->ci = malloc(patlen);
        if (p->ci == NULL) goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy_tolower(p->ci, pat, patlen);

        /* setup the case sensitive part of the pattern */
        if (p->flags & B2G_NOCASE) {
            /* nocase means no difference between cs and ci */
            p->cs = p->ci;
        } else {
            if (memcmp(p->ci,pat,p->len) == 0) {
                /* no diff between cs and ci: pat is lowercase */
                p->cs = p->ci;
            } else {
                p->cs = malloc(patlen);
                if (p->cs == NULL) goto error;
                mpm_ctx->memory_cnt++;
                mpm_ctx->memory_size += patlen;
                memcpy(p->cs, pat, patlen);
            }
        }

        //printf("B2gAddPattern: ci \""); prt(p->ci,p->len);
        //printf("\" cs \""); prt(p->cs,p->len);
        //printf("\" prefix_ci %" PRIu32 ", prefix_cs %" PRIu32 "\n", p->prefix_ci, p->prefix_cs);

        /* put in the pattern hash */
        B2gInitHashAdd(ctx, p);

        if (mpm_ctx->pattern_cnt == 65535) {
            printf("Max search words reached\n");
            exit(1);
        }
        if (scan) mpm_ctx->scan_pattern_cnt++;
        mpm_ctx->pattern_cnt++;

        if (scan) { /* SCAN */
            if (mpm_ctx->scan_maxlen < patlen) mpm_ctx->scan_maxlen = patlen;
            if (mpm_ctx->pattern_cnt == 1) mpm_ctx->scan_minlen = patlen;
            else if (mpm_ctx->scan_minlen > patlen) mpm_ctx->scan_minlen = patlen;
            p->flags |= B2G_SCAN;
        } else { /* SEARCH */
            if (mpm_ctx->search_maxlen < patlen) mpm_ctx->search_maxlen = patlen;
            if (mpm_ctx->pattern_cnt == 1) mpm_ctx->search_minlen = patlen;
            else if (mpm_ctx->search_minlen > patlen) mpm_ctx->search_minlen = patlen;
        }
    }

    /* we need a match */
    B2gEndMatchAppend(mpm_ctx, p, offset, depth, pid, sid, nosearch);

    /* keep track of highest pattern id XXX still used? */
    if (pid > mpm_ctx->max_pattern_id)
        mpm_ctx->max_pattern_id = pid;

    mpm_ctx->total_pattern_cnt++;

    return 0;

error:
    B2gFreePattern(mpm_ctx, p);
    return -1;
}

int B2gAddScanPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid, uint8_t nosearch)
{
    return B2gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */1, /* scan */1, pid, sid, nosearch);
}

int B2gAddScanPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid, uint8_t nosearch)
{
    return B2gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */0, /* scan */1, pid, sid, nosearch);
}

int B2gAddPatternCI(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid)
{
    return B2gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */1, /* scan */0, pid, sid, 0);
}

int B2gAddPatternCS(MpmCtx *mpm_ctx, uint8_t *pat, uint16_t patlen,
    uint16_t offset, uint16_t depth, uint32_t pid, uint32_t sid)
{
    return B2gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */0, /* scan */0, pid, sid, 0);
}

static inline uint32_t B2gBloomHash(void *data, uint16_t datalen, uint8_t iter, uint32_t hash_size) {
     uint8_t *d = (uint8_t *)data;
     uint16_t i;
     uint32_t hash = (uint32_t)u8_tolower(*d);

     for (i = 1; i < datalen; i++) {
         d++;
         hash += (u8_tolower(*d)) ^ i;
     }
     hash <<= (iter+1);

     hash %= hash_size;
     return hash;
}

static void B2gPrepareScanHash(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    uint16_t i;
    uint16_t idx = 0;
    uint8_t idx8 = 0;

    ctx->scan_hash = (B2gHashItem **)malloc(sizeof(B2gHashItem *) * ctx->scan_hash_size);
    if (ctx->scan_hash == NULL) goto error;
    memset(ctx->scan_hash, 0, sizeof(B2gHashItem *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2gHashItem *) * ctx->scan_hash_size);

#ifdef B2G_SCAN2
    ctx->scan_hash2 = (B2gHashItem **)malloc(sizeof(B2gHashItem *) * ctx->scan_hash_size);
    if (ctx->scan_hash2 == NULL) goto error;
    memset(ctx->scan_hash2, 0, sizeof(B2gHashItem *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2gHashItem *) * ctx->scan_hash_size);
#endif

    /* alloc the pminlen array */
    ctx->scan_pminlen = (uint8_t *)malloc(sizeof(uint8_t) * ctx->scan_hash_size);
    if (ctx->scan_pminlen == NULL) goto error;
    memset(ctx->scan_pminlen, 0, sizeof(uint8_t) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(uint8_t) * ctx->scan_hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        /* ignore patterns that don't have the scan flag set */
        if (!(ctx->parray[i]->flags & B2G_SCAN))
            continue;

        if(ctx->parray[i]->len == 1) {
            idx8 = (uint8_t)ctx->parray[i]->ci[0];
            if (ctx->scan_hash1[idx8].flags == 0) {
                ctx->scan_hash1[idx8].idx = i;
                ctx->scan_hash1[idx8].flags |= 0x01;
            } else {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B2gHashItem *thi = &ctx->scan_hash1[idx8];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->scan_1_pat_cnt++;
#ifdef B2G_SCAN2
        } else if(ctx->parray[i]->len == 2) {
            idx = B2G_HASH16(ctx->parray[i]->ci[0],ctx->parray[i]->ci[1]);
            if (ctx->scan_hash2[idx] == NULL) {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                ctx->scan_hash2[idx] = hi;
            } else {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B2gHashItem *thi = ctx->scan_hash2[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->scan_2_pat_cnt++;
#endif
        } else {
            idx = B2G_HASH16(ctx->parray[i]->ci[ctx->scan_m - 2], ctx->parray[i]->ci[ctx->scan_m - 1]);
            //printf("idx %" PRIu32 ", %c.%c\n", idx, ctx->parray[i]->ci[ctx->m - 2], ctx->parray[i]->ci[ctx->m - 1]);

            if (ctx->scan_hash[idx] == NULL) {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;
                ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                ctx->scan_hash[idx] = hi;
            } else {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                if (ctx->parray[i]->len < ctx->scan_pminlen[idx])
                    ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                /* Append this HashItem to the list */
                B2gHashItem *thi = ctx->scan_hash[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->scan_x_pat_cnt++;
        }
    }

    /* alloc the bloom array */
    ctx->scan_bloom = (BloomFilter **)malloc(sizeof(BloomFilter *) * ctx->scan_hash_size);
    if (ctx->scan_bloom == NULL) goto error;
    memset(ctx->scan_bloom, 0, sizeof(BloomFilter *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(BloomFilter *) * ctx->scan_hash_size);

    int h;
    for (h = 0; h < ctx->scan_hash_size; h++) {
        B2gHashItem *hi = ctx->scan_hash[h];
        if (hi == NULL)
            continue;

        ctx->scan_bloom[h] = BloomFilterInit(B2G_BLOOMSIZE, 2, B2gBloomHash);
        if (ctx->scan_bloom[h] == NULL)
            continue;

        mpm_ctx->memory_cnt += BloomFilterMemoryCnt(ctx->scan_bloom[h]);
        mpm_ctx->memory_size += BloomFilterMemorySize(ctx->scan_bloom[h]);

        if (ctx->scan_pminlen[h] > 8)
            ctx->scan_pminlen[h] = 8;

        B2gHashItem *thi = hi;
        do {
            BloomFilterAdd(ctx->scan_bloom[h], ctx->parray[thi->idx]->ci, ctx->scan_pminlen[h]);
            thi = thi->nxt;
        } while (thi != NULL);
    }

    return;
error:
    return;
}

static void B2gPrepareSearchHash(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    uint16_t i;
    uint16_t idx = 0;
    uint8_t idx8 = 0;

    ctx->search_hash = (B2gHashItem **)malloc(sizeof(B2gHashItem *) * ctx->search_hash_size);
    if (ctx->search_hash == NULL) goto error;
    memset(ctx->search_hash, 0, sizeof(B2gHashItem *) * ctx->search_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2gHashItem *) * ctx->search_hash_size);

    /* alloc the pminlen array */
    ctx->search_pminlen = (uint8_t *)malloc(sizeof(uint8_t) * ctx->search_hash_size);
    if (ctx->search_pminlen == NULL) goto error;
    memset(ctx->search_pminlen, 0, sizeof(uint8_t) * ctx->search_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(uint8_t) * ctx->search_hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        /* ignore patterns that have the scan flag set */
        if (ctx->parray[i]->flags & B2G_SCAN)
            continue;

        if(ctx->parray[i]->len == 1) {
            idx8 = (uint8_t)ctx->parray[i]->ci[0];
            if (ctx->search_hash1[idx8].flags == 0) {
                ctx->search_hash1[idx8].idx = i;
                ctx->search_hash1[idx8].flags |= 0x01;
            } else {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B2gHashItem *thi = &ctx->search_hash1[idx8];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
        } else {
            idx = B2G_HASH16(ctx->parray[i]->ci[ctx->search_m - 2], ctx->parray[i]->ci[ctx->search_m - 1]);
            //printf("idx %" PRIu32 ", %c.%c\n", idx, ctx->parray[i]->ci[ctx->search_m - 2], ctx->parray[i]->ci[ctx->search_m - 1]);

            if (ctx->search_hash[idx] == NULL) {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;
                ctx->search_pminlen[idx] = ctx->parray[i]->len;

                ctx->search_hash[idx] = hi;
            } else {
                B2gHashItem *hi = B2gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                if (ctx->parray[i]->len < ctx->search_pminlen[idx])
                    ctx->search_pminlen[idx] = ctx->parray[i]->len;

                /* Append this HashItem to the list */
                B2gHashItem *thi = ctx->search_hash[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
        }
    }

    /* alloc the bloom array */
    ctx->search_bloom = (BloomFilter **)malloc(sizeof(BloomFilter *) * ctx->search_hash_size);
    if (ctx->search_bloom == NULL) goto error;
    memset(ctx->search_bloom, 0, sizeof(BloomFilter *) * ctx->search_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(BloomFilter *) * ctx->search_hash_size);

    int h;
    for (h = 0; h < ctx->search_hash_size; h++) {
        B2gHashItem *hi = ctx->search_hash[h];
        if (hi == NULL)
            continue;

        ctx->search_bloom[h] = BloomFilterInit(B2G_BLOOMSIZE, 2, B2gBloomHash);
        if (ctx->search_bloom[h] == NULL)
            continue;

        mpm_ctx->memory_cnt += BloomFilterMemoryCnt(ctx->search_bloom[h]);
        mpm_ctx->memory_size += BloomFilterMemorySize(ctx->search_bloom[h]);

        if (ctx->search_pminlen[h] > 8)
            ctx->search_pminlen[h] = 8;

        B2gHashItem *thi = hi;
        do {
            BloomFilterAdd(ctx->search_bloom[h], ctx->parray[thi->idx]->ci, ctx->search_pminlen[h]);
            thi = thi->nxt;
        } while (thi != NULL);
    }
    return;
error:
    return;
}

int B2gBuildScanMatchArray(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;

    ctx->scan_B2G = malloc(sizeof(B2G_TYPE) * ctx->scan_hash_size);
    if (ctx->scan_B2G == NULL)
        return -1;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2G_TYPE) * ctx->scan_hash_size);

    memset(ctx->scan_B2G,0, B2G_HASHSIZE * sizeof(B2G_TYPE));

    uint32_t j;
    int a;

    /* fill the match array */
    for (j = 0; j <= (ctx->scan_m - B2G_Q); j++) {
        for (a = 0; a < mpm_ctx->pattern_cnt; a++) {
            if (!(ctx->parray[a]->flags & B2G_SCAN))
                continue;

            if (ctx->parray[a]->len < ctx->scan_m)
                continue;

            uint16_t h = B2G_HASH16(u8_tolower(ctx->parray[a]->ci[j]),u8_tolower(ctx->parray[a]->ci[j+1]));
            ctx->scan_B2G[h] = ctx->scan_B2G[h] | (1 << (ctx->scan_m - j));
        }
    }

    ctx->scan_s0 = 1;
    return 0;
}

int B2gBuildSearchMatchArray(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;

    ctx->search_B2G = malloc(sizeof(B2G_TYPE) * ctx->search_hash_size);
    if (ctx->search_B2G == NULL)
        return -1;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B2G_TYPE) * ctx->search_hash_size);

    memset(ctx->search_B2G,0, B2G_HASHSIZE * sizeof(B2G_TYPE));

    uint32_t j;
    int a;

    /* fill the match array */
    for (j = 0; j <= (ctx->search_m - B2G_Q); j++) {
        for (a = 0; a < mpm_ctx->pattern_cnt; a++) {
            if (ctx->parray[a]->flags & B2G_SCAN)
                continue;

            if (ctx->parray[a]->len < ctx->search_m)
                continue;

            uint16_t h = B2G_HASH16(u8_tolower(ctx->parray[a]->ci[j]),u8_tolower(ctx->parray[a]->ci[j+1]));
            //printf("h %" PRIu32 ", %c.%c\n", h, u8_tolower(ctx->parray[a]->ci[j]),u8_tolower(ctx->parray[a]->ci[j+1]));
            ctx->search_B2G[h] = ctx->search_B2G[h] | (1 << (ctx->search_m - j));
        }
    }

#if 0
    /* calculate s0 */
    B2G_TYPE s;
    B2G_S0 = m;

    /* look at each pattern */
    for (a = 0; pat[a] != NULL; a++) {
        if (strlen(pat) < m)
            continue;

        uint16_t h = B2G_HASH16(u8_tolower(pat[a][m-2]),u8_tolower(pat[a][m-1]));
        s = B2G[h];
        printf("S0: h %" PRIu32 ", %c.%c\n", h, pat[a][m-2], pat[a][m-1]);

        int i = m - 1;
        for ( ; i > 0; i--) {
            printf("i: %" PRId32 ", s %8u -- ", i, s);

            if ((s & (1 << (m - 1))) != 0) {
                printf(" (s0 update) ");
                if (i < B2G_S0) B2G_S0 = i;
            } else {
                printf(" (  nope   ) ");
            }

            h = B2G_HASH16(u8_tolower(pat[a][i-1]),u8_tolower(pat[a][i-0]));
            printf("S:  h %" PRIu32 ", %c.%c ", h, pat[a][i-1], pat[a][i-0]);
            s = (s << 1) & B2G[h];
            printf("B2G_S0 %" PRId32 " (s %" PRIu32 ", b2g[h] %" PRIu32 ")\n", B2G_S0, s, B2G[h]);
        }
    }
    B2G_S0--;
    printf("B2G_S0 %" PRId32 "\n", B2G_S0);
#endif
    return 0;
}

int B2gPreparePatterns(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;

    /* alloc the pattern array */
    ctx->parray = (B2gPattern **)malloc(mpm_ctx->pattern_cnt * sizeof(B2gPattern *));
    if (ctx->parray == NULL) goto error;
    memset(ctx->parray, 0, mpm_ctx->pattern_cnt * sizeof(B2gPattern *));
    //printf("mpm_ctx %p, parray %p\n", mpm_ctx,ctx->parray);
    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (mpm_ctx->pattern_cnt * sizeof(B2gPattern *));

    /* populate it with the patterns in the hash */
    uint32_t i = 0, p = 0;
    for (i = 0; i < INIT_HASH_SIZE; i++) {
        B2gPattern *node = ctx->init_hash[i], *nnode = NULL;
        for ( ; node != NULL; ) {
            nnode = node->next;
            node->next = NULL;

            ctx->parray[p] = node;

            p++;
            node = nnode;
        }
    }
    /* we no longer need the hash, so free it's memory */
    free(ctx->init_hash);
    ctx->init_hash = NULL;

    /* set 'm' to the smallest pattern size */
    ctx->scan_m = mpm_ctx->scan_minlen;
    ctx->search_m = mpm_ctx->search_minlen;

    if (mpm_ctx->search_minlen == 1) {
        mpm_ctx->Search = B2gSearch1;
        ctx->MBSearch = B2G_SEARCHFUNC;
    }
    /* make sure 'm' stays in bounds */
    if (ctx->scan_m > B2G_WORD_SIZE) {
        printf("Warning: 'm' bigger than word size: %" PRIu32 " > %" PRIu32 " (scan).", ctx->scan_m, B2G_WORD_SIZE);
        ctx->scan_m = B2G_WORD_SIZE;
    }
    if (ctx->scan_m < 2) ctx->scan_m = 2;

    if (ctx->search_m > B2G_WORD_SIZE) {
        printf("Warning: 'm' bigger than word size: %" PRIu32 " > %" PRIu32 " (search).", ctx->search_m, B2G_WORD_SIZE);
        ctx->search_m = B2G_WORD_SIZE;
    }
    if (ctx->search_m < 2) ctx->search_m = 2;

    ctx->scan_hash_size = B2G_HASHSIZE;
    ctx->search_hash_size = B2G_HASHSIZE;
    B2gPrepareScanHash(mpm_ctx);
    B2gPrepareSearchHash(mpm_ctx);
    B2gBuildScanMatchArray(mpm_ctx);
    B2gBuildSearchMatchArray(mpm_ctx);

    if (ctx->scan_1_pat_cnt) {
        mpm_ctx->Scan = B2gScan1;
#ifdef B2G_SCAN2
        mpm_ctx->Scan = B2gScan2;
        if (ctx->scan_2_pat_cnt) {
            ctx->MBScan2 = B2gScan2;
        }
#endif
        ctx->MBScan = B2G_SCANFUNC;
#ifdef B2G_SCAN2
    } else if (ctx->scan_2_pat_cnt) {
        mpm_ctx->Scan = B2gScan2;
        ctx->MBScan = B2G_SCANFUNC;
#endif
    }

    return 0;
error:
    return -1;
}

void B2gPrintSearchStats(MpmThreadCtx *mpm_thread_ctx) {
#ifdef B2G_COUNTERS
    B2gThreadCtx *tctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;

    printf("B2g Thread Search stats (tctx %p)\n", tctx);
    printf("Scan phase:\n");
    printf("Total calls/scans: %" PRIu32 "\n", tctx->scan_stat_calls);
    printf("Avg m/scan: %0.2f\n", tctx->scan_stat_calls ? (float)((float)tctx->scan_stat_m_total / (float)tctx->scan_stat_calls) : 0);
    printf("D != 0 (possible match): %" PRIu32 "\n", tctx->scan_stat_d0);
    printf("Avg hash items per bucket %0.2f (%" PRIu32 ")\n", tctx->scan_stat_d0 ? (float)((float)tctx->scan_stat_d0_hashloop / (float)tctx->scan_stat_d0) : 0, tctx->scan_stat_d0_hashloop);
    printf("Loop match: %" PRIu32 "\n", tctx->scan_stat_loop_match);
    printf("Loop no match: %" PRIu32 "\n", tctx->scan_stat_loop_no_match);
    printf("Num shifts: %" PRIu32 "\n", tctx->scan_stat_num_shift);
    printf("Total shifts: %" PRIu32 "\n", tctx->scan_stat_total_shift);
    printf("Avg shifts: %0.2f\n", tctx->scan_stat_num_shift ? (float)((float)tctx->scan_stat_total_shift / (float)tctx->scan_stat_num_shift) : 0);
    printf("Total BloomFilter checks: %" PRIu32 "\n", tctx->scan_stat_bloom_calls);
    printf("BloomFilter hits: %0.4f%% (%" PRIu32 ")\n", tctx->scan_stat_bloom_calls ? (float)((float)((float)tctx->scan_stat_bloom_hits / (float)tctx->scan_stat_bloom_calls)*(float)100) : 0, tctx->scan_stat_bloom_hits);
    printf("Avg pminlen: %0.2f\n\n", tctx->scan_stat_pminlen_calls ? (float)((float)tctx->scan_stat_pminlen_total / (float)tctx->scan_stat_pminlen_calls) : 0);

    printf("Search phase:\n");
    printf("D 0 (possible match, shift = 1): %" PRIu32 "\n", tctx->search_stat_d0);
    printf("Loop match: %" PRIu32 "\n", tctx->search_stat_loop_match);
    printf("Loop no match: %" PRIu32 "\n", tctx->search_stat_loop_no_match);
    printf("Num shifts: %" PRIu32 "\n", tctx->search_stat_num_shift);
    printf("Total shifts: %" PRIu32 "\n", tctx->search_stat_total_shift);
    printf("Avg shifts: %0.2f\n\n", tctx->search_stat_num_shift ? (float)((float)tctx->search_stat_total_shift / (float)tctx->search_stat_num_shift) : 0);
#endif /* B2G_COUNTERS */
}

static inline int
memcmp_lowercase(uint8_t *s1, uint8_t *s2, uint16_t n) {
    size_t i;

    /* check backwards because we already tested the first
     * 2 to 4 chars. This way we are more likely to detect
     * a miss and thus speed up a little... */
    for (i = n - 1; i; i--) {
        if (u8_tolower(*(s2+i)) != s1[i])
            return 1;
    }

    return 0;
}

void B2gInitCtx (MpmCtx *mpm_ctx) {
    //printf("B2gInitCtx: mpm_ctx %p\n", mpm_ctx);

    memset(mpm_ctx, 0, sizeof(MpmCtx));

    mpm_ctx->ctx = malloc(sizeof(B2gCtx));
    if (mpm_ctx->ctx == NULL)
        return;

    memset(mpm_ctx->ctx, 0, sizeof(B2gCtx));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B2gCtx);

    /* initialize the hash we use to speed up pattern insertions */
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    ctx->init_hash = malloc(sizeof(B2gPattern *) * INIT_HASH_SIZE);
    if (ctx->init_hash == NULL)
        return;

    memset(ctx->init_hash, 0, sizeof(B2gPattern *) * INIT_HASH_SIZE);
}

void B2gDestroyCtx(MpmCtx *mpm_ctx) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    if (ctx == NULL)
        return;

    if (ctx->init_hash) {
        free(ctx->init_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (INIT_HASH_SIZE * sizeof(B2gPattern *));
    }

    if (ctx->parray) {
        uint32_t i;
        for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
            if (ctx->parray[i] != NULL) {
                B2gFreePattern(mpm_ctx, ctx->parray[i]);
            }
        }

        free(ctx->parray);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(B2gPattern));
    }

    if (ctx->scan_B2G) {
        free(ctx->scan_B2G);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2G_TYPE) * ctx->scan_hash_size);
    }

    if (ctx->search_B2G) {
        free(ctx->search_B2G);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2G_TYPE) * ctx->search_hash_size);
    }

    if (ctx->scan_bloom) {
        int h;
        for (h = 0; h < ctx->scan_hash_size; h++) {
            if (ctx->scan_bloom[h] == NULL)
                continue;

            mpm_ctx->memory_cnt -= BloomFilterMemoryCnt(ctx->scan_bloom[h]);
            mpm_ctx->memory_size -= BloomFilterMemorySize(ctx->scan_bloom[h]);

            BloomFilterFree(ctx->scan_bloom[h]);
        }

        free(ctx->scan_bloom);

        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(BloomFilter *) * ctx->scan_hash_size);
    }

    if (ctx->scan_hash) {
        int h;
        for (h = 0; h < ctx->scan_hash_size; h++) {
            if (ctx->scan_hash[h] == NULL)
                continue;

            B2gHashFree(mpm_ctx, ctx->scan_hash[h]);
        }

        free(ctx->scan_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2gHashItem) * ctx->scan_hash_size);
    }

    if (ctx->search_bloom) {
        int h;
        for (h = 0; h < ctx->search_hash_size; h++) {
            if (ctx->search_bloom[h] == NULL)
                continue;

            mpm_ctx->memory_cnt -= BloomFilterMemoryCnt(ctx->search_bloom[h]);
            mpm_ctx->memory_size -= BloomFilterMemorySize(ctx->search_bloom[h]);

            BloomFilterFree(ctx->search_bloom[h]);
        }

        free(ctx->search_bloom);

        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(BloomFilter *) * ctx->search_hash_size);
    }

    if (ctx->search_hash) {
        int h;
        for (h = 0; h < ctx->search_hash_size; h++) {
            if (ctx->search_hash[h] == NULL)
                continue;

            B2gHashFree(mpm_ctx, ctx->search_hash[h]);
        }

        free(ctx->search_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B2gHashItem) * ctx->search_hash_size);
    }

    if (ctx->scan_pminlen) {
        free(ctx->scan_pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(uint8_t) * ctx->scan_hash_size);
    }

    if (ctx->search_pminlen) {
        free(ctx->search_pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(uint8_t) * ctx->search_hash_size);
    }

    free(mpm_ctx->ctx);
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B2gCtx);
}

void B2gThreadInitCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, uint32_t matchsize) {
    memset(mpm_thread_ctx, 0, sizeof(MpmThreadCtx));

    mpm_thread_ctx->ctx = malloc(sizeof(B2gThreadCtx));
    if (mpm_thread_ctx->ctx == NULL)
        return;

    memset(mpm_thread_ctx->ctx, 0, sizeof(B2gThreadCtx));

    mpm_thread_ctx->memory_cnt++;
    mpm_thread_ctx->memory_size += sizeof(B2gThreadCtx);

    /* alloc an array with the size of _all_ keys in all instances.
     * this is done so the detect engine won't have to care about
     * what instance it's looking up in. The matches all have a
     * unique id and is the array lookup key at the same time */
    //uint32_t keys = mpm_ctx->max_pattern_id + 1;
    uint32_t keys = matchsize + 1;
    if (keys) {
        mpm_thread_ctx->match = malloc(keys * sizeof(MpmMatchBucket));
        if (mpm_thread_ctx->match == NULL) {
            printf("ERROR: could not setup memory for pattern matcher: %s\n", strerror(errno));
            exit(1);
        }
        memset(mpm_thread_ctx->match, 0, keys * sizeof(MpmMatchBucket));

        mpm_thread_ctx->memory_cnt++;
        mpm_thread_ctx->memory_size += (keys * sizeof(MpmMatchBucket));
    }
}

void B2gThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx) {
    B2gThreadCtx *ctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;

    B2gPrintSearchStats(mpm_thread_ctx);

    if (ctx) {
        if (mpm_thread_ctx->match != NULL) {
            mpm_thread_ctx->memory_cnt--;
            mpm_thread_ctx->memory_size -= ((mpm_ctx->max_pattern_id + 1) * sizeof(MpmMatchBucket));
            free(mpm_thread_ctx->match);
        }

        mpm_thread_ctx->memory_cnt--;
        mpm_thread_ctx->memory_size -= sizeof(B2gThreadCtx);
        free(mpm_thread_ctx->ctx);
    }

    MpmMatchFreeSpares(mpm_thread_ctx, mpm_thread_ctx->sparelist);
    MpmMatchFreeSpares(mpm_thread_ctx, mpm_thread_ctx->qlist);
}

uint32_t B2gScanBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
#ifdef B2G_COUNTERS
    B2gThreadCtx *tctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = ctx->scan_m - B2G_Q + 1, matches = 0;
    B2G_TYPE d;

    //printf("\n");
    //PrintRawDataFp(stdout, buf, buflen);

    COUNT(tctx->scan_stat_calls++);
    COUNT(tctx->scan_stat_m_total+=ctx->scan_m);

    if (buflen < ctx->scan_m)
        return 0;

    while (pos <= (buflen - B2G_Q + 1)) {
        uint16_t h = B2G_HASH16(u8_tolower(buf[pos - 1]),u8_tolower(buf[pos]));
        d = ctx->scan_B2G[h];

        if (d != 0) {
            COUNT(tctx->scan_stat_d0++);
            uint32_t j = pos;
            uint32_t first = pos - (ctx->scan_m - B2G_Q + 1);

            do {
                j = j - 1;
                if (d >= (1 << (ctx->scan_m - 1))) {
                    if (j > first) pos = j;
                    else {
                        /* get our patterns from the hash */
                        h = B2G_HASH16(u8_tolower(buf[j + ctx->scan_m - 2]),u8_tolower(buf[j + ctx->scan_m - 1]));

                        if (ctx->scan_bloom[h] != NULL) {
                            COUNT(tctx->scan_stat_pminlen_calls++);
                            COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                            if ((buflen - j) < ctx->scan_pminlen[h]) {
                                goto skip_loop;
                            } else {
                                COUNT(tctx->scan_stat_bloom_calls++);

                                if (BloomFilterTest(ctx->scan_bloom[h], buf+j, ctx->scan_pminlen[h]) == 0) {
                                    COUNT(tctx->scan_stat_bloom_hits++);

                                    //printf("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                                    goto skip_loop;
                                }
                            }
                        }

                        B2gHashItem *hi = ctx->scan_hash[h], *thi;
                        for (thi = hi; thi != NULL; thi = thi->nxt) {
                            COUNT(tctx->scan_stat_d0_hashloop++);
                            B2gPattern *p = ctx->parray[thi->idx];

                            if (p->flags & B2G_NOCASE) {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp_lowercase(p->ci, buf+j, p->len) == 0) {
                                    //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                                    COUNT(tctx->scan_stat_loop_match++);

                                    MpmEndMatch *em;
                                    for (em = p->em; em; em = em->next) {
                                        //printf("em %p id %" PRIu32 "\n", em, em->id);
                                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], j, p->len))
                                            matches++;
                                    }
                                } else {
                                    COUNT(tctx->scan_stat_loop_no_match++);
                                }
                            } else {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp(p->cs, buf+j, p->len) == 0) {
                                    //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                                    COUNT(tctx->scan_stat_loop_match++);

                                    MpmEndMatch *em;
                                    for (em = p->em; em; em = em->next) {
                                        //printf("em %p id %" PRIu32 "\n", em, em->id);
                                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], j, p->len))
                                            matches++;
                                    }
                                } else {
                                    COUNT(tctx->scan_stat_loop_no_match++);
                                }
                            }
                        }
skip_loop:
                        //printf("output at pos %" PRIu32 ": ", j); prt(buf + (j), ctx->scan_m); printf("\n");
                        ;
                    }
                }

                if (j == 0)
                    break;

                h = B2G_HASH16(u8_tolower(buf[j - 1]),u8_tolower(buf[j]));
                d = (d << 1) & ctx->scan_B2G[h];
            } while (d != 0);
        }
        COUNT(tctx->scan_stat_num_shift++);
        COUNT(tctx->scan_stat_total_shift += (ctx->scan_m - B2G_Q + 1));
        pos = pos + ctx->scan_m - B2G_Q + 1;
    }
    return matches;
}

uint32_t B2gScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
#ifdef B2G_COUNTERS
    B2gThreadCtx *tctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = 0, matches = 0;
    B2G_TYPE d;
    uint32_t j;

    COUNT(tctx->scan_stat_calls++);
    COUNT(tctx->scan_stat_m_total+=ctx->scan_m);

    if (buflen < ctx->scan_m)
        return 0;

    while (pos <= (buflen - ctx->scan_m)) {
        j = ctx->scan_m - 1;
        d = ~0;

        do {
            uint16_t h = B2G_HASH16(u8_tolower(buf[pos + j - 1]),u8_tolower(buf[pos + j]));
            d = ((d << 1) & ctx->scan_B2G[h]);
            j = j - 1;
        } while (d != 0 && j != 0);
        //printf("scan: d %" PRIu32 ", j %" PRIu32 "\n", d, j);

        /* (partial) match, move on to verification */
        if (d != 0) {
            COUNT(tctx->scan_stat_d0++);
            //printf("output at pos %" PRIu32 ": ", pos); prt(buf + pos, ctx->scan_m); printf("\n");

            /* get our patterns from the hash */
            uint16_t h = B2G_HASH16(u8_tolower(buf[pos + ctx->scan_m - 2]),u8_tolower(buf[pos + ctx->scan_m - 1]));

            if (ctx->scan_bloom[h] != NULL) {
                COUNT(tctx->scan_stat_pminlen_calls++);
                COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                if ((buflen - pos) < ctx->scan_pminlen[h]) {
                    goto skip_loop;
                } else {
                    COUNT(tctx->scan_stat_bloom_calls++);

                    if (BloomFilterTest(ctx->scan_bloom[h], buf+pos, ctx->scan_pminlen[h]) == 0) {
                        COUNT(tctx->scan_stat_bloom_hits++);

                        //printf("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                        goto skip_loop;
                    }
                }
            }

            B2gHashItem *hi = ctx->scan_hash[h], *thi;
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                COUNT(tctx->scan_stat_d0_hashloop++);
                B2gPattern *p = ctx->parray[thi->idx];

                if (p->flags & B2G_NOCASE) {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp_lowercase(p->ci, buf+pos, p->len) == 0) {
#ifdef PRINTMATCH
                        printf("CI Exact match: \""); prt(p->ci, p->len); printf("\" ");
#endif
                        COUNT(tctx->scan_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
#ifdef PRINTMATCH
printf("(%" PRIu32 "%s) ", g_de_ctx->sig_array[em->sig_id]->id, em->flags & MPM_ENDMATCH_NOSEARCH ? "" : " (searchable)");
#endif
                            //printf("em %p id %" PRIu32 "\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }
#ifdef PRINTMATCH
printf("\n");
#endif
                    } else {
                        COUNT(tctx->scan_stat_loop_no_match++);
                    }
                } else {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp(p->cs, buf+pos, p->len) == 0) {
#ifdef PRINTMATCH
                        printf("CS Exact match: \""); prt(p->cs, p->len); printf("\" ");
#endif
                        COUNT(tctx->scan_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
#ifdef PRINTMATCH
printf("(%" PRIu32 "%s) ", g_de_ctx->sig_array[em->sig_id]->id, em->flags & MPM_ENDMATCH_NOSEARCH ? "" : " (searchable)");
#endif
                            //printf("em %p id %" PRIu32 "\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }
#ifdef PRINTMATCH
printf("\n");
#endif
                    } else {
                        COUNT(tctx->scan_stat_loop_no_match++);
                    }
                }
            }
skip_loop:
            //pos = pos + ctx->scan_s0;
            pos = pos + 1;
        } else {
            COUNT(tctx->scan_stat_num_shift++);
            COUNT(tctx->scan_stat_total_shift += (j + 1));

            pos = pos + j + 1;
        }
    }

    //printf("Total matches %" PRIu32 "\n", matches);
    return matches;
}

#ifdef B2G_SCAN2
uint32_t B2gScan2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    B2gPattern *p;
    MpmEndMatch *em; 
    B2gHashItem *thi, *hi;

    if (buflen < 2)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        uint8_t h8 = u8_tolower(*buf);
        hi = &ctx->scan_hash1[h8];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->flags & B2G_NOCASE) {
                    if (h8 == p->ci[0]) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                } else {
                    if (*buf == p->cs[0]) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                }
            }
        }

        /* save one conversion by reusing h8 */
        uint16_t h16 = B2G_HASH16(h8, u8_tolower(*(buf+1)));
        hi = ctx->scan_hash2[h16];

        for (thi = hi; thi != NULL; thi = thi->nxt) {
            p = ctx->parray[thi->idx];

            if (p->flags & B2G_NOCASE) {
                if (h8 == p->ci[0] && u8_tolower(*(buf+1)) == p->ci[1]) {
                    //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                    for (em = p->em; em; em = em->next) {
                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                            cnt++;
                    }
                }
            } else {
                if (*buf == p->cs[0] && *(buf+1) == p->cs[1]) {
                    //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                    for (em = p->em; em; em = em->next) {
                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                            cnt++;
                    }
                }
            }
        }
        buf += 1;
    }

    //printf("B2gSearch2: after 2byte cnt %" PRIu32 "\n", cnt);
    if (ctx->scan_x_pat_cnt > 0) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBScan(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B2gSearch1: after 2+byte cnt %" PRIu32 "\n", cnt);
    }
    return cnt;
}
#endif

uint32_t B2gScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    B2gPattern *p;
    MpmEndMatch *em; 
    B2gHashItem *thi, *hi;

    if (buflen == 0)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        uint8_t h = u8_tolower(*buf);
        hi = &ctx->scan_hash1[h];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->len != 1)
                    continue;

                if (p->flags & B2G_NOCASE) {
                    if (u8_tolower(*buf) == p->ci[0]) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                } else {
                    if (*buf == p->cs[0]) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                }
            }
        }
        buf += 1;
    }

    //printf("B2gSearch1: after 1byte cnt %" PRIu32 "\n", cnt);
#ifdef B2G_SCAN2
    if (ctx->scan_2_pat_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBScan2(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B2gSearch1: after 2+byte cnt %" PRIu32 "\n", cnt);
    } else
#endif
    if (ctx->scan_x_pat_cnt) {
        cnt += ctx->MBScan(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
    }
    return cnt;
}

uint32_t B2gSearchBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
#ifdef B2G_COUNTERS
    B2gThreadCtx *tctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = ctx->search_m - B2G_Q + 1, matches = 0;
    B2G_TYPE d;

    COUNT(tctx->search_stat_calls++);
    COUNT(tctx->search_stat_m_total+=ctx->search_m);

    if (buflen < ctx->search_m)
        return 0;

    while (pos <= (buflen - B2G_Q + 1)) {
        uint16_t h = B2G_HASH16(u8_tolower(buf[pos - 1]),u8_tolower(buf[pos]));
        d = ctx->search_B2G[h];

        if (d != 0) {
            COUNT(tctx->search_stat_d0++);
            uint32_t j = pos;
            uint32_t first = pos - (ctx->search_m - B2G_Q + 1);

            do {
                j = j - 1;
                if (d >= (1 << (ctx->search_m - 1))) {
                    if (j > first) pos = j;
                    else {
                        /* get our patterns from the hash */
                        h = B2G_HASH16(u8_tolower(buf[j + ctx->search_m - 2]),u8_tolower(buf[j + ctx->search_m - 1]));

                        if (ctx->search_bloom[h] != NULL) {
                            COUNT(tctx->search_stat_pminlen_calls++);
                            COUNT(tctx->search_stat_pminlen_total+=ctx->search_pminlen[h]);

                            if ((buflen - j) < ctx->search_pminlen[h]) {
                                goto skip_loop;
                            } else {
                                COUNT(tctx->search_stat_bloom_calls++);

                                if (BloomFilterTest(ctx->search_bloom[h], buf+j, ctx->search_pminlen[h]) == 0) {
                                    COUNT(tctx->search_stat_bloom_hits++);

                                    //printf("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                                    goto skip_loop;
                                }
                            }
                        }

                        B2gHashItem *hi = ctx->search_hash[h], *thi;
                        for (thi = hi; thi != NULL; thi = thi->nxt) {
                            COUNT(tctx->search_stat_d0_hashloop++);
                            B2gPattern *p = ctx->parray[thi->idx];

                            if (p->flags & B2G_NOCASE) {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp_lowercase(p->ci, buf+j, p->len) == 0) {
                                    //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                                    COUNT(tctx->search_stat_loop_match++);

                                    MpmEndMatch *em; 
                                    for (em = p->em; em; em = em->next) {
                                        //printf("em %p id %" PRIu32 "\n", em, em->id);
                                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], j, p->len))
                                            matches++;
                                    }
                                } else {
                                    COUNT(tctx->search_stat_loop_no_match++);
                                }
                            } else {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp(p->cs, buf+j, p->len) == 0) {
                                    //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                                    COUNT(tctx->search_stat_loop_match++);

                                    MpmEndMatch *em; 
                                    for (em = p->em; em; em = em->next) {
                                        //printf("em %p id %" PRIu32 "\n", em, em->id);
                                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], j, p->len))
                                            matches++;
                                    }
                                } else {
                                    COUNT(tctx->search_stat_loop_no_match++);
                                }
                            }
                        }
skip_loop:
                        //printf("output at pos %" PRIu32 ": ", j); prt(buf + (j), ctx->scan_m); printf("\n");
                        ;
                    }
                }

                if (j == 0)
                    break;

                h = B2G_HASH16(u8_tolower(buf[j - 1]),u8_tolower(buf[j]));
                d = (d << 1) & ctx->search_B2G[h];
            } while (d != 0);
        }
        COUNT(tctx->search_stat_num_shift++);
        COUNT(tctx->search_stat_total_shift += (ctx->search_m - B2G_Q + 1));
        pos = pos + ctx->search_m - B2G_Q + 1;
    }
    return matches;
}

uint32_t B2gSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
#ifdef B2G_COUNTERS
    B2gThreadCtx *tctx = (B2gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    uint32_t pos = 0, matches = 0;
    B2G_TYPE d;
    uint32_t j;

    if (buflen < ctx->search_m)
        return 0;

    while (pos <= (buflen - ctx->search_m)) {
        j = ctx->search_m - 1;
        d = ~0;

        do {
            uint16_t h = B2G_HASH16(u8_tolower(buf[pos + j - 1]),u8_tolower(buf[pos + j]));
            d &= ctx->search_B2G[h];
            d <<= 1;
            j = j - 1;
            //printf("h %" PRIu32 " d %" PRId32 " %c.%c\n", h, d, u8_tolower(buf[pos + j - 1]),u8_tolower(buf[pos + j]));
        } while (d != 0 && j != 0);

        /* (partial) match, move on to verification */
        if (d != 0) {
            COUNT(tctx->search_stat_d0++);

            /* get our patterns from the hash */
            uint16_t h = B2G_HASH16(u8_tolower(buf[pos + ctx->search_m - 2]),u8_tolower(buf[pos + ctx->search_m - 1]));

            if (ctx->scan_bloom[h] != NULL) {
                COUNT(tctx->scan_stat_pminlen_calls++);
                COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                if ((buflen - pos) < ctx->scan_pminlen[h]) {
                    goto skip_loop;
                } else {
                    COUNT(tctx->scan_stat_bloom_calls++);

                    if (BloomFilterTest(ctx->scan_bloom[h], buf+pos, ctx->scan_pminlen[h]) == 0) {
                        COUNT(tctx->scan_stat_bloom_hits++);

                        //printf("Bloom: %p, buflen %" PRIu32 ", pos %" PRIu32 ", p_min_len %" PRIu32 "\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                        goto skip_loop;
                    }
                }
            }

            B2gHashItem *hi = ctx->search_hash[h], *thi;
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                B2gPattern *p = ctx->parray[thi->idx];
                if (p->flags & B2G_NOCASE) {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp_lowercase(p->ci, buf+pos, p->len) == 0) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                        COUNT(tctx->search_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
                            //printf("em %p id %" PRIu32 "\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }

                    } else {
                        COUNT(tctx->search_stat_loop_no_match++);
                    }
                } else {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp(p->cs, buf+pos, p->len) == 0) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                        COUNT(tctx->search_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
                            //printf("em %p id %" PRIu32 "\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }

                    } else {
                        COUNT(tctx->search_stat_loop_no_match++);
                    }
                }
            }

            //printf("output at pos %" PRId32 ": ", pos); prt(buf + pos, ctx->search_m); printf("\n");
skip_loop:
            pos = pos + 1;
        } else {
            COUNT(tctx->search_stat_num_shift++);
            COUNT(tctx->search_stat_total_shift += (j + 1));
            pos = pos + j + 1;
        }
    }

    //printf("Total matches %" PRIu32 "\n", matches);
    return matches;
}

uint32_t B2gSearch1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, uint8_t *buf, uint16_t buflen) {
    B2gCtx *ctx = (B2gCtx *)mpm_ctx->ctx;
    uint8_t *bufmin = buf;
    uint8_t *bufend = buf + buflen - 1;
    uint32_t cnt = 0;
    B2gPattern *p;
    MpmEndMatch *em; 
    B2gHashItem *thi, *hi;

    if (buflen == 0)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    if (mpm_ctx->search_minlen == 1) {
        while (buf <= bufend) {
            uint8_t h = u8_tolower(*buf);
            hi = &ctx->search_hash1[h];

            if (hi->flags & 0x01) {
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    if (p->len != 1)
                        continue;

                    if (p->flags & B2G_NOCASE) {
                        if (u8_tolower(*buf) == p->ci[0]) {
                            //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                            for (em = p->em; em; em = em->next) {
                                if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                    cnt++;
                            }
                        }
                    } else {
                        if (*buf == p->cs[0]) {
                            //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B2gSearch1)\n");
                            for (em = p->em; em; em = em->next) {
                                if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                    cnt++;
                            }
                        }
                    }
                }
            }
            buf += 1;
        }
    }
    //printf("B2gSearch1: after 1byte cnt %" PRIu32 "\n", cnt);
    if (mpm_ctx->search_maxlen > 1) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBSearch(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B2gSearch1: after 2+byte cnt %" PRIu32 "\n", cnt);
    }
    return cnt;
}

/*
 * TESTS
 */

static int B2gTestInit01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->search_m == 4)
        result = 1;
    else
        printf("4 != %" PRIu32 " ", ctx->search_m);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}

#if 0
static int B2gTestS0Init01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 4)
        result = 1;
    else
        printf("4 != %" PRIu32 " ", ctx->scan_s0);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestS0Init02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"cdef", 4, 0, 0, 1, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ", ctx->scan_s0);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestS0Init03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"bcde", 4, 0, 0, 1, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ", ctx->scan_s0);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestS0Init04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abab", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ", ctx->scan_s0);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestS0Init05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcab", 5, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ", ctx->scan_s0);

    B2gDestroyCtx(&mpm_ctx);
    return result;
}
#endif

static int B2gTestScan01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abce", 4, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"bcde", 4, 0, 0, 1, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"fghj", 4, 0, 0, 2, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

/* test patterns longer than 'm'. M is 4 here. */
static int B2gTestScan04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"bcdegh", 6, 0, 0, 1, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"fghjxyz", 7, 0, 0, 2, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

/* case insensitive test patterns longer than 'm'. M is 4 here. */
static int B2gTestScan05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCI(&mpm_ctx, (uint8_t *)"ABCD", 4, 0, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCI(&mpm_ctx, (uint8_t *)"bCdEfG", 6, 0, 0, 1, 0, 0); /* 1 match */
    B2gAddScanPatternCI(&mpm_ctx, (uint8_t *)"fghJikl", 7, 0, 0, 2, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan07 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    //B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0, 0); /* should match 30 times */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0, 0); /* should match 29 times */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0, 0); /* should match 28 times */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0, 0); /* 26 */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0, 0); /* 21 */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0, 0); /* 1 */
    /* total matches: 135 */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan08 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"a", 1);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan09 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"ab", 2, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"ab", 2);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan10 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcdefgh", 8, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679abcdefgh012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679", 208);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan11 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"abcde", 5, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2 /* 2 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestScan12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"wxyz", 4, 0, 0, 0, 0, 0); /* 1 match */
    B2gAddScanPatternCS(&mpm_ctx, (uint8_t *)"vwxyz", 5, 0, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2 /* 2 patterns */);

    uint32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abce", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"bcde", 4, 0, 0, 1, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"fghj", 4, 0, 0, 2, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

/* test patterns longer than 'm'. M is 4 here. */
static int B2gTestSearch04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"bcdegh", 6, 0, 0, 1, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"fghjxyz", 7, 0, 0, 2, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

/* case insensitive test patterns longer than 'm'. M is 4 here. */
static int B2gTestSearch05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCI(&mpm_ctx, (uint8_t *)"ABCD", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddPatternCI(&mpm_ctx, (uint8_t *)"bCdEfG", 6, 0, 0, 1, 0); /* 1 match */
    B2gAddPatternCI(&mpm_ctx, (uint8_t *)"fghJikl", 7, 0, 0, 2, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcd", 4);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch07 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);
    //B2gCtx *ctx = (B2gCtx *)mpm_ctx.ctx;

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"A", 1, 0, 0, 0, 0); /* should match 30 times */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"AA", 2, 0, 0, 1, 0); /* should match 29 times */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"AAA", 3, 0, 0, 2, 0); /* should match 28 times */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAA", 5, 0, 0, 3, 0); /* 26 */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0); /* 21 */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0); /* 1 */
    /* total matches: 135 */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 135)
        result = 1;
    else
        printf("135 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch08 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"a", 1);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch09 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"ab", 2, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"ab", 2);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch10 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcdefgh", 8, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679abcdefgh012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679", 208);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch11 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"abcde", 5, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2 /* 2 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

static int B2gTestSearch12 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B2G);

    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"wxyz", 4, 0, 0, 0, 0); /* 1 match */
    B2gAddPatternCS(&mpm_ctx, (uint8_t *)"vwxyz", 5, 0, 0, 0, 0); /* 1 match */

    B2gPreparePatterns(&mpm_ctx);
    B2gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 2 /* 2 patterns */);

    uint32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (uint8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 2)
        result = 1;
    else
        printf("2 != %" PRIu32 " ",cnt);

    B2gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B2gDestroyCtx(&mpm_ctx);
    return result;
}

void B2gRegisterTests(void) {
    UtRegisterTest("B2gTestInit01", B2gTestInit01, 1);
/*
    UtRegisterTest("B2gTestS0Init01", B2gTestS0Init01, 1);
    UtRegisterTest("B2gTestS0Init02", B2gTestS0Init02, 1);
    UtRegisterTest("B2gTestS0Init03", B2gTestS0Init03, 1);
    UtRegisterTest("B2gTestS0Init04", B2gTestS0Init04, 1);
    UtRegisterTest("B2gTestS0Init05", B2gTestS0Init05, 1);
*/
    UtRegisterTest("B2gTestScan01", B2gTestScan01, 1);
    UtRegisterTest("B2gTestScan02", B2gTestScan02, 1);
    UtRegisterTest("B2gTestScan03", B2gTestScan03, 1);
    UtRegisterTest("B2gTestScan04", B2gTestScan04, 1);
    UtRegisterTest("B2gTestScan05", B2gTestScan05, 1);
    UtRegisterTest("B2gTestScan06", B2gTestScan06, 1);
    UtRegisterTest("B2gTestScan07", B2gTestScan07, 1);
    UtRegisterTest("B2gTestScan08", B2gTestScan08, 1);
    UtRegisterTest("B2gTestScan09", B2gTestScan09, 1);
    UtRegisterTest("B2gTestScan10", B2gTestScan10, 1);
    UtRegisterTest("B2gTestScan11", B2gTestScan11, 1);
    UtRegisterTest("B2gTestScan12", B2gTestScan12, 1);

    UtRegisterTest("B2gTestSearch01", B2gTestSearch01, 1);
    UtRegisterTest("B2gTestSearch02", B2gTestSearch02, 1);
    UtRegisterTest("B2gTestSearch03", B2gTestSearch03, 1);
    UtRegisterTest("B2gTestSearch04", B2gTestSearch04, 1);
    UtRegisterTest("B2gTestSearch05", B2gTestSearch05, 1);
    UtRegisterTest("B2gTestSearch06", B2gTestSearch06, 1);
    UtRegisterTest("B2gTestSearch07", B2gTestSearch07, 1);
    UtRegisterTest("B2gTestSearch08", B2gTestSearch08, 1);
    UtRegisterTest("B2gTestSearch09", B2gTestSearch09, 1);
    UtRegisterTest("B2gTestSearch10", B2gTestSearch10, 1);
    UtRegisterTest("B2gTestSearch11", B2gTestSearch11, 1);
    UtRegisterTest("B2gTestSearch12", B2gTestSearch12, 1);
}

#if 0
int main () {
#define R 4
int i;
    B2gCtx bg_ctx;
    B2gInitCtx(&bg_ctx);

    B2gAddPatternCI(&bg_ctx, "grep", 4, 0, 0, 0, 0);
    B2pPrepare(&bg_ctx);

    B2GSearch(&bg_ctx,Text,strlen(Text));

    exit(EXIT_SUCCESS);
}
#endif
