/* 3 gram implementation of the (S)BNDMq pattern matching algorithm.
 *
 * Copyright (c) 2009 Victor Julien <victor@inliniac.net>
 *
 * Ideas:
 *  - B3g does a full match in the scan phase of up to 'm' characters,
 *    in case of a case insensitive search we could say it's match if
 *    the pattern is of len 'm' or just compare the rest of the chars.
 *
 * TODO:
 *  - Try to get the S0 calculation right.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "util-bloomfilter.h"
#include "util-mpm-b3g.h"

#include "util-unittest.h"

/* uppercase to lowercase conversion lookup table */
static u_int8_t lowercasetable[256];
/* marco to do the actual lookup */
#define bg_tolower(c) lowercasetable[(c)]

#define INIT_HASH_SIZE 65536

#ifdef B3G_COUNTERS
#define COUNT(counter) \
        (counter)
#else
#define COUNT(counter)
#endif /* B3G_COUNTERS */

void B3gInitCtx (MpmCtx *mpm_ctx);
void B3gThreadInitCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, u_int32_t);
void B3gDestroyCtx(MpmCtx *mpm_ctx);
void B3gThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx);
int B3gAddScanPatternCI(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen, u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid);
int B3gAddScanPatternCS(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen, u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid);
int B3gAddPatternCI(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen, u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid);
int B3gAddPatternCS(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen, u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid);
int B3gPreparePatterns(MpmCtx *mpm_ctx);
u_int32_t B3gScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gScan2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gScanBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gSearch1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gSearch2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
u_int32_t B3gSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *, u_int8_t *buf, u_int16_t buflen);
void B3gPrintInfo(MpmCtx *mpm_ctx);
void B3gPrintSearchStats(MpmThreadCtx *mpm_thread_ctx);
void B3gRegisterTests(void);

void MpmB3gRegister (void) {
    mpm_table[MPM_B3G].name = "b2g";
    mpm_table[MPM_B3G].InitCtx = B3gInitCtx;
    mpm_table[MPM_B3G].InitThreadCtx = B3gThreadInitCtx;
    mpm_table[MPM_B3G].DestroyCtx = B3gDestroyCtx;
    mpm_table[MPM_B3G].DestroyThreadCtx = B3gThreadDestroyCtx;
    mpm_table[MPM_B3G].AddScanPattern = B3gAddScanPatternCS;
    mpm_table[MPM_B3G].AddScanPatternNocase = B3gAddScanPatternCI;
    mpm_table[MPM_B3G].AddPattern = B3gAddPatternCS;
    mpm_table[MPM_B3G].AddPatternNocase = B3gAddPatternCI;
    mpm_table[MPM_B3G].Prepare = B3gPreparePatterns;
    mpm_table[MPM_B3G].Scan = B3G_SCANFUNC; /* default to B3gSearch. We may fall back to 1 */
    mpm_table[MPM_B3G].Search = B3gSearch; /* default to B3gSearch. We may fall back to 1 */
    mpm_table[MPM_B3G].Cleanup = MpmMatchCleanup;
    mpm_table[MPM_B3G].PrintCtx = B3gPrintInfo;
    mpm_table[MPM_B3G].PrintThreadCtx = B3gPrintSearchStats;
    mpm_table[MPM_B3G].RegisterUnittests = B3gRegisterTests;

    /* create table for O(1) lowercase conversion lookup */
    u_int8_t c = 0;
    for ( ; c < 255; c++) {
       if (c >= 'A' && c <= 'Z')
           lowercasetable[c] = (c + ('a' - 'A'));
       else
           lowercasetable[c] = c;
    }
}

/* append an endmatch to a pattern
 *
 * Only used in the initialization phase */
static inline void B3gEndMatchAppend(MpmCtx *mpm_ctx, B3gPattern *p,
    u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid)
{
    MpmEndMatch *em = MpmAllocEndMatch(mpm_ctx);
    if (em == NULL) {
        printf("ERROR: B3gAllocEndMatch failed\n");
        return;
    }

    em->id = pid;
    em->sig_id = sid;
    em->depth = depth;
    em->offset = offset;

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

static void prt (u_int8_t *buf, u_int16_t buflen) {
    u_int16_t i;

    for (i = 0; i < buflen; i++) {
        if (isprint(buf[i])) printf("%c", buf[i]);
        else                 printf("\\x%X", buf[i]);
    }
    //printf("\n");
}

void B3gPrintInfo(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;

    printf("MPM B3g Information:\n");
    printf("Memory allocs:   %u\n", mpm_ctx->memory_cnt);
    printf("Memory alloced:  %u\n", mpm_ctx->memory_size);
    printf(" Sizeofs:\n");
    printf("  MpmCtx         %u\n", sizeof(MpmCtx));
    printf("  B3gCtx:         %u\n", sizeof(B3gCtx));
    printf("  B3gPattern      %u\n", sizeof(B3gPattern));
    printf("  B3gHashItem     %u\n", sizeof(B3gHashItem));
    printf("Unique Patterns: %u\n", mpm_ctx->pattern_cnt);
    printf("Scan Patterns:   %u\n", mpm_ctx->scan_pattern_cnt);
    printf("Total Patterns:  %u\n", mpm_ctx->total_pattern_cnt);
    printf("Smallest:        %u\n", mpm_ctx->scan_minlen);
    printf("Largest:         %u\n", mpm_ctx->scan_maxlen);
    printf("Hash size:       %u\n", ctx->scan_hash_size);
    printf("\n");
}

static inline B3gPattern *B3gAllocPattern(MpmCtx *mpm_ctx) {
    B3gPattern *p = malloc(sizeof(B3gPattern));
    if (p == NULL) {
        printf("ERROR: B3gAllocPattern: malloc failed\n");
    }
    memset(p,0,sizeof(B3gPattern));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B3gPattern);
    return p;
}

static inline B3gHashItem *
B3gAllocHashItem(MpmCtx *mpm_ctx) {
    B3gHashItem *hi = malloc(sizeof(B3gHashItem));
    if (hi == NULL) {
        printf("ERROR: B3gAllocHashItem: malloc failed\n");
    }
    memset(hi,0,sizeof(B3gHashItem));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B3gHashItem);
    return hi;
}

static void B3gHashFree(MpmCtx *mpm_ctx, B3gHashItem *hi) {
    if (hi == NULL)
        return;

    B3gHashItem *t = hi->nxt;
    B3gHashFree(mpm_ctx, t);

    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B3gHashItem);
    free(hi);
}

static inline void memcpy_tolower(u_int8_t *d, u_int8_t *s, u_int16_t len) {
    u_int16_t i;
    for (i = 0; i < len; i++) {
        d[i] = bg_tolower(s[i]);
    }
}

/*
 * INIT HASH START
 */
static inline u_int32_t B3gInitHash(B3gPattern *p) {
    u_int32_t hash = p->len * p->cs[0];
    if (p->len > 1)
        hash += p->cs[1];

    return (hash % INIT_HASH_SIZE);
}

static inline u_int32_t B3gInitHashRaw(u_int8_t *pat, u_int16_t patlen) {
    u_int32_t hash = patlen * pat[0];
    if (patlen > 1)
        hash += pat[1];

    return (hash % INIT_HASH_SIZE);
}

static inline int B3gInitHashAdd(B3gCtx *ctx, B3gPattern *p) {
    u_int32_t hash = B3gInitHash(p);

    //printf("B3gInitHashAdd: %u\n", hash);

    if (ctx->init_hash[hash] == NULL) {
        ctx->init_hash[hash] = p;
        //printf("B3gInitHashAdd: hash %u, head %p\n", hash, ctx->init_hash[hash]);
        return 0;
    }

    B3gPattern *t = ctx->init_hash[hash], *tt = NULL;
    for ( ; t != NULL; t = t->next) {
        tt = t;
    }
    tt->next = p;
    //printf("B3gInitHashAdd: hash %u, head %p\n", hash, ctx->init_hash[hash]);

    return 0;
}

static inline int B3gCmpPattern(B3gPattern *p, u_int8_t *pat, u_int16_t patlen, char nocase);

static inline B3gPattern *B3gInitHashLookup(B3gCtx *ctx, u_int8_t *pat, u_int16_t patlen, char nocase) {
    u_int32_t hash = B3gInitHashRaw(pat,patlen);

    //printf("B3gInitHashLookup: %u, head %p\n", hash, ctx->init_hash[hash]);

    if (ctx->init_hash[hash] == NULL) {
        return NULL;
    }

    B3gPattern *t = ctx->init_hash[hash];
    for ( ; t != NULL; t = t->next) {
        if (B3gCmpPattern(t,pat,patlen,nocase) == 1)
            return t;
    }

    return NULL;
}

static inline int B3gCmpPattern(B3gPattern *p, u_int8_t *pat, u_int16_t patlen, char nocase) {
    if (p->len != patlen)
        return 0;

    if (!((nocase && p->flags & B3G_NOCASE) || (!nocase && !(p->flags & B3G_NOCASE))))
        return 0;

    if (memcmp(p->cs, pat, patlen) != 0)
        return 0;

    return 1;
}

/*
 * INIT HASH END
 */

void B3gFreePattern(MpmCtx *mpm_ctx, B3gPattern *p) {
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
        mpm_ctx->memory_size -= sizeof(B3gPattern); 
    }
}

/* B3gAddPattern
 *
 * pat: ptr to the pattern
 * patlen: length of the pattern
 * nocase: nocase flag: 1 enabled, 0 disable
 * pid: pattern id
 * sid: signature id (internal id)
 */
static inline int B3gAddPattern(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen, u_int16_t offset, u_int16_t depth, char nocase, char scan, u_int32_t pid, u_int32_t sid) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;

//    printf("B3gAddPattern: ctx %p \"", mpm_ctx); prt(pat, patlen);
//    printf("\" id %u, nocase %s\n", id, nocase ? "true" : "false");

    if (patlen == 0)
        return 0;

    /* get a memory piece */
    B3gPattern *p = B3gInitHashLookup(ctx, pat, patlen, nocase);
    if (p == NULL) {
//        printf("B3gAddPattern: allocing new pattern\n");
        p = B3gAllocPattern(mpm_ctx);
        if (p == NULL)
            goto error;

        p->len = patlen;

        if (nocase) p->flags |= B3G_NOCASE;

        /* setup the case insensitive part of the pattern */
        p->ci = malloc(patlen);
        if (p->ci == NULL) goto error;
        mpm_ctx->memory_cnt++;
        mpm_ctx->memory_size += patlen;
        memcpy_tolower(p->ci, pat, patlen);

        /* setup the case sensitive part of the pattern */
        if (p->flags & B3G_NOCASE) {
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

        //printf("B3gAddPattern: ci \""); prt(p->ci,p->len);
        //printf("\" cs \""); prt(p->cs,p->len);
        //printf("\" prefix_ci %u, prefix_cs %u\n", p->prefix_ci, p->prefix_cs);

        /* put in the pattern hash */
        B3gInitHashAdd(ctx, p);

        if (mpm_ctx->pattern_cnt == 65535) {
            printf("Max search words reached\n");
            exit(1);
        }
        mpm_ctx->pattern_cnt++;

        if (scan) { /* SCAN */
            if (mpm_ctx->scan_maxlen < patlen) mpm_ctx->scan_maxlen = patlen;
            if (mpm_ctx->pattern_cnt == 1) mpm_ctx->scan_minlen = patlen;
            else if (mpm_ctx->scan_minlen > patlen) mpm_ctx->scan_minlen = patlen;
            p->flags |= B3G_SCAN;
        } else { /* SEARCH */
            if (mpm_ctx->search_maxlen < patlen) mpm_ctx->search_maxlen = patlen;
            if (mpm_ctx->pattern_cnt == 1) mpm_ctx->search_minlen = patlen;
            else if (mpm_ctx->search_minlen > patlen) mpm_ctx->search_minlen = patlen;
        }
    }

    /* we need a match */
    B3gEndMatchAppend(mpm_ctx, p, offset, depth, pid, sid);

    /* keep track of highest pattern id XXX still used? */
    if (pid > mpm_ctx->max_pattern_id)
        mpm_ctx->max_pattern_id = pid;

    mpm_ctx->total_pattern_cnt++;

    return 0;

error:
    B3gFreePattern(mpm_ctx, p);
    return -1;
}

int B3gAddScanPatternCI(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen,
    u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid)
{
    return B3gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */1, /* scan */1, pid, sid);
}

int B3gAddScanPatternCS(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen,
    u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid)
{
    return B3gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */0, /* scan */1, pid, sid);
}

int B3gAddPatternCI(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen,
    u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid)
{
    return B3gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */1, /* scan */0, pid, sid);
}

int B3gAddPatternCS(MpmCtx *mpm_ctx, u_int8_t *pat, u_int16_t patlen,
    u_int16_t offset, u_int16_t depth, u_int32_t pid, u_int32_t sid)
{
    return B3gAddPattern(mpm_ctx, pat, patlen, offset, depth, /* nocase */0, /* scan */0, pid, sid);
}

static u_int32_t B3gBloomHash(void *data, u_int16_t datalen, u_int8_t iter, u_int32_t hash_size) {
     u_int8_t *d = (u_int8_t *)data;
     u_int16_t i;
     u_int32_t hash = (u_int32_t)bg_tolower(*d);

     for (i = 1; i < datalen; i++) {
         d++;
         hash += (bg_tolower(*d)) ^ i;
     }
     hash <<= (iter+1);

     hash %= hash_size;
     return hash;
}

static void B3gPrepareScanHash(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int16_t i;
    u_int16_t idx = 0;
    u_int8_t idx8 = 0;

    ctx->scan_hash = (B3gHashItem **)malloc(sizeof(B3gHashItem *) * ctx->scan_hash_size);
    if (ctx->scan_hash == NULL) goto error;
    memset(ctx->scan_hash, 0, sizeof(B3gHashItem *) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B3gHashItem *) * ctx->scan_hash_size);

    /* alloc the pminlen array */
    ctx->scan_pminlen = (u_int8_t *)malloc(sizeof(u_int8_t) * ctx->scan_hash_size);
    if (ctx->scan_pminlen == NULL) goto error;
    memset(ctx->scan_pminlen, 0, sizeof(u_int8_t) * ctx->scan_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(u_int8_t) * ctx->scan_hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        /* ignore patterns that don't have the scan flag set */
        if (!(ctx->parray[i]->flags & B3G_SCAN))
            continue;

        if(ctx->parray[i]->len == 1) {
            idx8 = (u_int8_t)ctx->parray[i]->ci[0];
            if (ctx->scan_hash1[idx8].flags == 0) {
                ctx->scan_hash1[idx8].idx = i;
                ctx->scan_hash1[idx8].flags |= 0x01;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B3gHashItem *thi = &ctx->scan_hash1[idx8];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->scan_1_pat_cnt++;
        } else if(ctx->parray[i]->len == 2) {
            idx = (u_int16_t)(ctx->parray[i]->ci[0] << 8 | ctx->parray[i]->ci[1]);
            if (ctx->scan_hash2[idx].flags == 0) {
                ctx->scan_hash2[idx].idx = i;
                ctx->scan_hash2[idx].flags |= 0x01;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B3gHashItem *thi = &ctx->scan_hash2[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->scan_2_pat_cnt++;
        } else {
            idx = B3G_HASH(ctx->parray[i]->ci[ctx->scan_m - 3], ctx->parray[i]->ci[ctx->scan_m - 2], ctx->parray[i]->ci[ctx->scan_m - 1]);
            //printf("idx %u, %c.%c.%c\n", idx, ctx->parray[i]->ci[ctx->scan_m - 3], ctx->parray[i]->ci[ctx->scan_m - 2], ctx->parray[i]->ci[ctx->scan_m - 1]);

            if (ctx->scan_hash[idx] == NULL) {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;
                ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                ctx->scan_hash[idx] = hi;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                if (ctx->parray[i]->len < ctx->scan_pminlen[idx])
                    ctx->scan_pminlen[idx] = ctx->parray[i]->len;

                /* Append this HashItem to the list */
                B3gHashItem *thi = ctx->scan_hash[idx];
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
        B3gHashItem *hi = ctx->scan_hash[h];
        if (hi == NULL)
            continue;

        ctx->scan_bloom[h] = BloomFilterInit(B3G_BLOOMSIZE, 2, B3gBloomHash);
        if (ctx->scan_bloom[h] == NULL)
            continue;

        mpm_ctx->memory_cnt += BloomFilterMemoryCnt(ctx->scan_bloom[h]);
        mpm_ctx->memory_size += BloomFilterMemorySize(ctx->scan_bloom[h]);

        if (ctx->scan_pminlen[h] > 8)
            ctx->scan_pminlen[h] = 8;

        B3gHashItem *thi = hi;
        do {
            BloomFilterAdd(ctx->scan_bloom[h], ctx->parray[thi->idx]->ci, ctx->scan_pminlen[h]);
            thi = thi->nxt;
        } while (thi != NULL);
    }

    return;
error:
    return;
}

static void B3gPrepareSearchHash(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int16_t i;
    u_int16_t idx = 0;
    u_int8_t idx8 = 0;

    ctx->search_hash = (B3gHashItem **)malloc(sizeof(B3gHashItem *) * ctx->search_hash_size);
    if (ctx->search_hash == NULL) goto error;
    memset(ctx->search_hash, 0, sizeof(B3gHashItem *) * ctx->search_hash_size);

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B3gHashItem *) * ctx->search_hash_size);

    for (i = 0; i < mpm_ctx->pattern_cnt; i++)
    {
        /* ignore patterns that have the scan flag set */
        if (ctx->parray[i]->flags & B3G_SCAN)
            continue;

        if(ctx->parray[i]->len == 1) {
            idx8 = (u_int8_t)ctx->parray[i]->ci[0];
            if (ctx->search_hash1[idx8].flags == 0) {
                ctx->search_hash1[idx8].idx = i;
                ctx->search_hash1[idx8].flags |= 0x01;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B3gHashItem *thi = &ctx->search_hash1[idx8];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->search_1_pat_cnt++;
        } else if(ctx->parray[i]->len == 2) {
            idx = (u_int16_t)(ctx->parray[i]->ci[0] << 8 | ctx->parray[i]->ci[1]);
            if (ctx->search_hash2[idx].flags == 0) {
                ctx->search_hash2[idx].idx = i;
                ctx->search_hash2[idx].flags |= 0x01;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B3gHashItem *thi = &ctx->search_hash2[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->search_2_pat_cnt++;
        } else {
            idx = B3G_HASH(ctx->parray[i]->ci[ctx->search_m - 3], ctx->parray[i]->ci[ctx->search_m - 2], ctx->parray[i]->ci[ctx->search_m - 1]);
            //printf("idx %u, %c.%c\n", idx, ctx->parray[i]->ci[ctx->search_m - 2], ctx->parray[i]->ci[ctx->search_m - 1]);

            if (ctx->search_hash[idx] == NULL) {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                ctx->search_hash[idx] = hi;
            } else {
                B3gHashItem *hi = B3gAllocHashItem(mpm_ctx);
                hi->idx = i;
                hi->flags |= 0x01;

                /* Append this HashItem to the list */
                B3gHashItem *thi = ctx->search_hash[idx];
                while (thi->nxt) thi = thi->nxt;
                thi->nxt = hi;
            }
            ctx->search_x_pat_cnt++;
        }
    }
    return;
error:
    return;
}

int B3gBuildScanMatchArray(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;

    ctx->scan_B3G = malloc(sizeof(B3G_TYPE) * ctx->scan_hash_size);
    if (ctx->scan_B3G == NULL)
        return -1;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B3G_TYPE) * ctx->scan_hash_size);

    memset(ctx->scan_B3G,0, B3G_HASHSIZE * sizeof(B3G_TYPE));

    u_int j;
    int a;

    /* fill the match array */
    for (j = 0; j <= (ctx->scan_m - B3G_Q); j++) {
        for (a = 0; a < mpm_ctx->pattern_cnt; a++) {
            if (!(ctx->parray[a]->flags & B3G_SCAN))
                continue;

            if (ctx->parray[a]->len < ctx->scan_m)
                continue;
          
            u_int16_t h = B3G_HASH(bg_tolower(ctx->parray[a]->ci[j]),bg_tolower(ctx->parray[a]->ci[j+1]), bg_tolower(ctx->parray[a]->ci[j+2]));
//printf("B3gBuildScanMatchArray: h %u, %c.%c.%c\n", h, bg_tolower(ctx->parray[a]->ci[j]),bg_tolower(ctx->parray[a]->ci[j+1]), bg_tolower(ctx->parray[a]->ci[j+2]));
            ctx->scan_B3G[h] = ctx->scan_B3G[h] | (1 << (ctx->scan_m - j));
        }
    }

    ctx->scan_s0 = 1;
    return 0;
}

int B3gBuildSearchMatchArray(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;

    ctx->search_B3G = malloc(sizeof(B3G_TYPE) * ctx->search_hash_size);
    if (ctx->search_B3G == NULL)
        return -1;

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (sizeof(B3G_TYPE) * ctx->search_hash_size);

    memset(ctx->search_B3G,0, B3G_HASHSIZE * sizeof(B3G_TYPE));

    u_int j;
    int a;

    /* fill the match array */
    for (j = 0; j <= (ctx->search_m - B3G_Q); j++) {
        for (a = 0; a < mpm_ctx->pattern_cnt; a++) {
            if (ctx->parray[a]->flags & B3G_SCAN)
                continue;

            if (ctx->parray[a]->len < ctx->search_m)
                continue;
          
            u_int16_t h = B3G_HASH(bg_tolower(ctx->parray[a]->ci[j]),bg_tolower(ctx->parray[a]->ci[j+1]), bg_tolower(ctx->parray[a]->ci[j+2]));
            //printf("h %u, %c.%c\n", h, bg_tolower(ctx->parray[a]->ci[j]),bg_tolower(ctx->parray[a]->ci[j+1]));
            ctx->search_B3G[h] = ctx->search_B3G[h] | (1 << (ctx->search_m - j));
        }
    }

#if 0
    /* calculate s0 */
    B3G_TYPE s;
    B3G_S0 = m;

    /* look at each pattern */
    for (a = 0; pat[a] != NULL; a++) {
        if (strlen(pat) < m)
            continue;

        u_int16_t h = B3G_HASH16(bg_tolower(pat[a][m-2]),bg_tolower(pat[a][m-1]));
        s = B3G[h];
        printf("S0: h %u, %c.%c\n", h, pat[a][m-2], pat[a][m-1]);

        int i = m - 1;
        for ( ; i > 0; i--) {
            printf("i: %d, s %8u -- ", i, s);

            if ((s & (1 << (m - 1))) != 0) {
                printf(" (s0 update) ");
                if (i < B3G_S0) B3G_S0 = i;
            } else {
                printf(" (  nope   ) ");
            }

            h = B3G_HASH16(bg_tolower(pat[a][i-1]),bg_tolower(pat[a][i-0]));
            printf("S:  h %u, %c.%c ", h, pat[a][i-1], pat[a][i-0]);
            s = (s << 1) & B3G[h];
            printf("B3G_S0 %d (s %u, b2g[h] %u)\n", B3G_S0, s, B3G[h]);
        }
    }
    B3G_S0--;
    printf("B3G_S0 %d\n", B3G_S0);
#endif
    return 0;
}

int B3gPreparePatterns(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;

    /* alloc the pattern array */
    ctx->parray = (B3gPattern **)malloc(mpm_ctx->pattern_cnt * sizeof(B3gPattern *));
    if (ctx->parray == NULL) goto error;
    memset(ctx->parray, 0, mpm_ctx->pattern_cnt * sizeof(B3gPattern *));
    //printf("mpm_ctx %p, parray %p\n", mpm_ctx,ctx->parray);
    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += (mpm_ctx->pattern_cnt * sizeof(B3gPattern *));

    /* populate it with the patterns in the hash */
    u_int32_t i = 0, p = 0;
    for (i = 0; i < INIT_HASH_SIZE; i++) {
        B3gPattern *node = ctx->init_hash[i], *nnode = NULL;
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

    /* make sure 'm' stays in bounds */
    if (ctx->scan_m > B3G_WORD_SIZE) {
        printf("Warning: 'm' bigger than word size: %u > %u (scan).", ctx->scan_m, B3G_WORD_SIZE);
        ctx->scan_m = B3G_WORD_SIZE;
    }
    if (ctx->scan_m < 3) ctx->scan_m = 3;

    if (ctx->search_m > B3G_WORD_SIZE) {
        printf("Warning: 'm' bigger than word size: %u > %u (search).", ctx->search_m, B3G_WORD_SIZE);
        ctx->search_m = B3G_WORD_SIZE;
    }
    if (ctx->search_m < 3) ctx->search_m = 3;

    ctx->scan_hash_size = B3G_HASHSIZE;
    ctx->search_hash_size = B3G_HASHSIZE;
    B3gPrepareScanHash(mpm_ctx);
    B3gPrepareSearchHash(mpm_ctx);
    B3gBuildScanMatchArray(mpm_ctx);
    B3gBuildSearchMatchArray(mpm_ctx);

    if (ctx->scan_1_pat_cnt) {
        mpm_ctx->Scan = B3gScan1;
        if (ctx->scan_2_pat_cnt) {
            ctx->MBScan2 = B3gScan2;
        }
        ctx->MBScan = B3G_SCANFUNC;
    } else if (ctx->scan_2_pat_cnt) {
        mpm_ctx->Scan = B3gScan2;
        ctx->MBScan = B3G_SCANFUNC;
    }

    if (ctx->search_1_pat_cnt) {
        mpm_ctx->Search = B3gSearch1;
        if (ctx->search_2_pat_cnt) {
            ctx->MBSearch2 = B3gSearch2;
        }
        ctx->MBSearch = B3gSearch;
    } else if (ctx->search_2_pat_cnt) {
        mpm_ctx->Search = B3gSearch2;
        ctx->MBSearch = B3gSearch;
    }

    return 0;
error:
    return -1;
}

void B3gPrintSearchStats(MpmThreadCtx *mpm_thread_ctx) {
#ifdef B3G_COUNTERS
    B3gThreadCtx *tctx = (B3gThreadCtx *)mpm_thread_ctx->ctx;

    printf("B3g Thread Search stats (tctx %p)\n", tctx);
    printf("Scan phase:\n");
    printf("Total calls/scans: %u\n", tctx->scan_stat_calls);
    printf("Avg m/scan: %0.2f\n", tctx->scan_stat_calls ? (float)((float)tctx->scan_stat_m_total / (float)tctx->scan_stat_calls) : 0);
    printf("D != 0 (possible match): %u\n", tctx->scan_stat_d0);
    printf("Avg hash items per bucket %0.2f (%u)\n", tctx->scan_stat_d0 ? (float)((float)tctx->scan_stat_d0_hashloop / (float)tctx->scan_stat_d0) : 0, tctx->scan_stat_d0_hashloop);
    printf("Loop match: %u\n", tctx->scan_stat_loop_match);
    printf("Loop no match: %u\n", tctx->scan_stat_loop_no_match);
    printf("Num shifts: %u\n", tctx->scan_stat_num_shift);
    printf("Total shifts: %u\n", tctx->scan_stat_total_shift);
    printf("Avg shifts: %0.2f\n", tctx->scan_stat_num_shift ? (float)((float)tctx->scan_stat_total_shift / (float)tctx->scan_stat_num_shift) : 0);
    printf("Total BloomFilter checks: %u\n", tctx->scan_stat_bloom_calls);
    printf("BloomFilter hits: %0.4f%% (%u)\n", tctx->scan_stat_bloom_calls ? (float)((float)((float)tctx->scan_stat_bloom_hits / (float)tctx->scan_stat_bloom_calls)*(float)100) : 0, tctx->scan_stat_bloom_hits);
    printf("Avg pminlen: %0.2f\n\n", tctx->scan_stat_pminlen_calls ? (float)((float)tctx->scan_stat_pminlen_total / (float)tctx->scan_stat_pminlen_calls) : 0);

    printf("Search phase:\n");
    printf("D 0 (possible match, shift = 1): %u\n", tctx->search_stat_d0);
    printf("Loop match: %u\n", tctx->search_stat_loop_match);
    printf("Loop no match: %u\n", tctx->search_stat_loop_no_match);
    printf("Num shifts: %u\n", tctx->search_stat_num_shift);
    printf("Total shifts: %u\n", tctx->search_stat_total_shift);
    printf("Avg shifts: %0.2f\n\n", tctx->search_stat_num_shift ? (float)((float)tctx->search_stat_total_shift / (float)tctx->search_stat_num_shift) : 0);
#endif /* B3G_COUNTERS */
}

static inline int
memcmp_lowercase(u_int8_t *s1, u_int8_t *s2, u_int16_t n) {
    size_t i;

    /* check backwards because we already tested the first
     * 2 to 4 chars. This way we are more likely to detect
     * a miss and thus speed up a little... */
    for (i = n - 1; i; i--) {
        if (bg_tolower(*(s2+i)) != s1[i])
            return 1;
    }

    return 0;
}

void B3gInitCtx (MpmCtx *mpm_ctx) {
    //printf("B3gInitCtx: mpm_ctx %p\n", mpm_ctx);

    memset(mpm_ctx, 0, sizeof(MpmCtx));

    mpm_ctx->ctx = malloc(sizeof(B3gCtx));
    if (mpm_ctx->ctx == NULL)
        return;

    memset(mpm_ctx->ctx, 0, sizeof(B3gCtx));

    mpm_ctx->memory_cnt++;
    mpm_ctx->memory_size += sizeof(B3gCtx);

    /* initialize the hash we use to speed up pattern insertions */
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    ctx->init_hash = malloc(sizeof(B3gPattern *) * INIT_HASH_SIZE);
    if (ctx->init_hash == NULL)
        return;

    memset(ctx->init_hash, 0, sizeof(B3gPattern *) * INIT_HASH_SIZE);
}

void B3gDestroyCtx(MpmCtx *mpm_ctx) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    if (ctx == NULL)
        return;

    if (ctx->init_hash) {
        free(ctx->init_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (INIT_HASH_SIZE * sizeof(B3gPattern *));
    }

    if (ctx->parray) {
        u_int32_t i;
        for (i = 0; i < mpm_ctx->pattern_cnt; i++) {
            if (ctx->parray[i] != NULL) {
                B3gFreePattern(mpm_ctx, ctx->parray[i]);
            }
        }

        free(ctx->parray);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (mpm_ctx->pattern_cnt * sizeof(B3gPattern));
    }

    if (ctx->scan_B3G) {
        free(ctx->scan_B3G);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B3G_TYPE) * ctx->scan_hash_size);
    }

    if (ctx->search_B3G) {
        free(ctx->search_B3G);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B3G_TYPE) * ctx->search_hash_size);
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

            B3gHashFree(mpm_ctx, ctx->scan_hash[h]);
        }

        free(ctx->scan_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B3gHashItem) * ctx->scan_hash_size);
    }
#if 0
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
#endif
    if (ctx->search_hash) {
        int h;
        for (h = 0; h < ctx->search_hash_size; h++) {
            if (ctx->search_hash[h] == NULL)
                continue;

            B3gHashFree(mpm_ctx, ctx->search_hash[h]);
        }

        free(ctx->search_hash);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(B3gHashItem) * ctx->search_hash_size);
    }

    if (ctx->scan_pminlen) {
        free(ctx->scan_pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(u_int8_t) * ctx->scan_hash_size);
    }
#if 0
    if (ctx->search_pminlen) {
        free(ctx->search_pminlen);
        mpm_ctx->memory_cnt--;
        mpm_ctx->memory_size -= (sizeof(u_int8_t) * ctx->search_hash_size);
    }
#endif
    free(mpm_ctx->ctx);
    mpm_ctx->memory_cnt--;
    mpm_ctx->memory_size -= sizeof(B3gCtx);
}

void B3gThreadInitCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, u_int32_t matchsize) {
    memset(mpm_thread_ctx, 0, sizeof(MpmThreadCtx));

    mpm_thread_ctx->ctx = malloc(sizeof(B3gThreadCtx));
    if (mpm_thread_ctx->ctx == NULL)
        return;

    memset(mpm_thread_ctx->ctx, 0, sizeof(B3gThreadCtx));

    mpm_thread_ctx->memory_cnt++;
    mpm_thread_ctx->memory_size += sizeof(B3gThreadCtx);

    /* alloc an array with the size of _all_ keys in all instances.
     * this is done so the detect engine won't have to care about
     * what instance it's looking up in. The matches all have a
     * unique id and is the array lookup key at the same time */
    //u_int32_t keys = mpm_ctx->max_pattern_id + 1;
    u_int32_t keys = matchsize + 1;
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

void B3gThreadDestroyCtx(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx) {
    B3gThreadCtx *ctx = (B3gThreadCtx *)mpm_thread_ctx->ctx;

    B3gPrintSearchStats(mpm_thread_ctx);

    if (ctx) {
        if (mpm_thread_ctx->match != NULL) {
            mpm_thread_ctx->memory_cnt--;
            mpm_thread_ctx->memory_size -= ((mpm_ctx->max_pattern_id + 1) * sizeof(MpmMatchBucket));
            free(mpm_thread_ctx->match);
        }

        mpm_thread_ctx->memory_cnt--;
        mpm_thread_ctx->memory_size -= sizeof(B3gThreadCtx);
        free(mpm_thread_ctx->ctx);
    }

    MpmMatchFreeSpares(mpm_thread_ctx, mpm_thread_ctx->sparelist);
    MpmMatchFreeSpares(mpm_thread_ctx, mpm_thread_ctx->qlist);
}

u_int32_t B3gScanBNDMq(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
#ifdef B3G_COUNTERS
    B3gThreadCtx *tctx = (B3gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    u_int32_t pos = ctx->scan_m - B3G_Q + 1, matches = 0;
    B3G_TYPE d;

    COUNT(tctx->scan_stat_calls++);
    COUNT(tctx->scan_stat_m_total+=ctx->scan_m);

    if (buflen < ctx->scan_m)
        return 0;

    while (pos <= (buflen - B3G_Q + 1)) {
        u_int16_t h = B3G_HASH(bg_tolower(buf[pos - 1]), bg_tolower(buf[pos]),bg_tolower(buf[pos + 1]));
        d = ctx->scan_B3G[h];

        if (d != 0) {
            COUNT(tctx->scan_stat_d0++);
            u_int j = pos;
            u_int first = pos - (ctx->scan_m - B3G_Q + 1);

            do {
                j = j - 1;
                if (d >= (1 << (ctx->scan_m - 1))) {
                    if (j > first) pos = j;
                    else {
                        /* get our patterns from the hash */
                        h = B3G_HASH(bg_tolower(buf[j + ctx->scan_m - 3]), bg_tolower(buf[j + ctx->scan_m - 2]),bg_tolower(buf[j + ctx->scan_m - 1]));

                        if (ctx->scan_bloom[h] != NULL) {
                            COUNT(tctx->scan_stat_pminlen_calls++);
                            COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                            if ((buflen - j) < ctx->scan_pminlen[h]) {
                                goto skip_loop;
                            } else {
                                COUNT(tctx->scan_stat_bloom_calls++);

                                if (BloomFilterTest(ctx->scan_bloom[h], buf+j, ctx->scan_pminlen[h]) == 0) {
                                    COUNT(tctx->scan_stat_bloom_hits++);

                                    //printf("Bloom: %p, buflen %u, pos %u, p_min_len %u\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                                    goto skip_loop;
                                }
                            }
                        }

                        B3gHashItem *hi = ctx->scan_hash[h], *thi;
                        for (thi = hi; thi != NULL; thi = thi->nxt) {
                            COUNT(tctx->scan_stat_d0_hashloop++);
                            B3gPattern *p = ctx->parray[thi->idx];

                            if (p->flags & B3G_NOCASE) {
                                if (buflen - j < p->len)
                                    continue;

                                if (memcmp_lowercase(p->ci, buf+j, p->len) == 0) {
                                    //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                                    COUNT(tctx->scan_stat_loop_match++);

                                    MpmEndMatch *em; 
                                    for (em = p->em; em; em = em->next) {
                                        //printf("em %p id %u\n", em, em->id);
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
                                        //printf("em %p id %u\n", em, em->id);
                                        if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], j, p->len))
                                            matches++;
                                    }
                                } else {
                                    COUNT(tctx->scan_stat_loop_no_match++);
                                }
                            }
                        }
skip_loop:
                        //printf("output at pos %u: ", j); prt(buf + (j), ctx->scan_m); printf("\n");
                        ; // gcc doesn't like the goto label without this :-S
                    }
                }

                h = B3G_HASH(bg_tolower(buf[j - 1]), bg_tolower(buf[j - 0]),bg_tolower(buf[j+1]));
                d = (d << 1) & ctx->scan_B3G[h];
            } while (d != 0);
        }
        COUNT(tctx->scan_stat_num_shift++);
        COUNT(tctx->scan_stat_total_shift += (ctx->scan_m - B3G_Q + 1));
        pos = pos + ctx->scan_m - B3G_Q + 1;
    }
    return matches;
}

u_int32_t B3gScan(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
#ifdef B3G_COUNTERS
    B3gThreadCtx *tctx = (B3gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    u_int32_t pos = 0, matches = 0;
    B3G_TYPE d;
    u_int j;

    COUNT(tctx->scan_stat_calls++);
    COUNT(tctx->scan_stat_m_total+=ctx->scan_m);

    if (buflen < ctx->scan_m)
        return 0;

    while (pos <= (buflen - ctx->scan_m)) {
        j = ctx->scan_m - 2;
        d = ~0;

        do {
            u_int16_t h = B3G_HASH(bg_tolower(buf[pos + j - 1]), bg_tolower(buf[pos + j - 0]),bg_tolower(buf[pos + j + 1]));
//            printf("scan: h %u, %c.%c.%c\n", h, bg_tolower(buf[pos + j - 1]), bg_tolower(buf[pos + j - 0]),bg_tolower(buf[pos + j + 1]));
            d = ((d << 1) & ctx->scan_B3G[h]);
            j = j - 1;
        } while (d != 0 && j != 0);
//        printf("scan: d %u, j %u\n", d, j);

        /* (partial) match, move on to verification */
        if (d != 0) {
            COUNT(tctx->scan_stat_d0++);
            //printf("output at pos %u: ", pos); prt(buf + pos, ctx->scan_m); printf("\n");

            /* get our patterns from the hash */
            u_int16_t h = B3G_HASH(bg_tolower(buf[pos + ctx->scan_m - 3]), bg_tolower(buf[pos + ctx->scan_m - 2]),bg_tolower(buf[pos + ctx->scan_m - 1]));

            if (ctx->scan_bloom[h] != NULL) {
                COUNT(tctx->scan_stat_pminlen_calls++);
                COUNT(tctx->scan_stat_pminlen_total+=ctx->scan_pminlen[h]);

                if ((buflen - pos) < ctx->scan_pminlen[h]) {
                    goto skip_loop;
                } else {
                    COUNT(tctx->scan_stat_bloom_calls++);

                    if (BloomFilterTest(ctx->scan_bloom[h], buf+pos, ctx->scan_pminlen[h]) == 0) {
                        COUNT(tctx->scan_stat_bloom_hits++);

                        //printf("Bloom: %p, buflen %u, pos %u, p_min_len %u\n", ctx->scan_bloom[h], buflen, pos, ctx->scan_pminlen[h]);
                        goto skip_loop;
                    }
                }
            }

            B3gHashItem *hi = ctx->scan_hash[h], *thi;
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                COUNT(tctx->scan_stat_d0_hashloop++);
                B3gPattern *p = ctx->parray[thi->idx];

                if (p->flags & B3G_NOCASE) {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp_lowercase(p->ci, buf+pos, p->len) == 0) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                        COUNT(tctx->scan_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
                            //printf("em %p id %u\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }
                    } else {
                        COUNT(tctx->scan_stat_loop_no_match++);
                    }
                } else {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp(p->cs, buf+pos, p->len) == 0) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf("\n");
                        COUNT(tctx->scan_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
                            //printf("em %p id %u\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }
                    } else {
                        COUNT(tctx->scan_stat_loop_no_match++);
                    }
                }
            }
skip_loop:
            pos = pos + ctx->scan_s0;
        } else {
            COUNT(tctx->scan_stat_num_shift++);
            COUNT(tctx->scan_stat_total_shift += (j + 1));

            pos = pos + j + 1;
        }
    }

    //printf("Total matches %u\n", matches);
    return matches;
}

u_int32_t B3gScan2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int8_t *bufmin = buf;
    u_int8_t *bufend = buf + buflen - 1;
    u_int32_t cnt = 0;
    B3gPattern *p;
    MpmEndMatch *em; 
    B3gHashItem *thi, *hi;

    if (buflen < 2)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        u_int16_t h = bg_tolower(*buf) << 8 | bg_tolower(*(buf+1));
        hi = &ctx->scan_hash2[h];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->len != 2)
                    continue;

                if (p->flags & B3G_NOCASE) {
                    if (bg_tolower(*buf) == p->ci[0] && bg_tolower(*(buf+1)) == p->ci[1]) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                } else {
                    if (*buf == p->cs[0] && *(buf+1) == p->cs[1]) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
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

    //printf("B3gSearch2: after 2byte cnt %u\n", cnt);
    if (ctx->scan_x_pat_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBScan(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B3gSearch1: after 2+byte cnt %u\n", cnt);
    }
    return cnt;
}
u_int32_t B3gScan1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int8_t *bufmin = buf;
    u_int8_t *bufend = buf + buflen - 1;
    u_int32_t cnt = 0;
    B3gPattern *p;
    MpmEndMatch *em; 
    B3gHashItem *thi, *hi;

    if (buflen == 0)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        u_int8_t h = bg_tolower(*buf);
        hi = &ctx->scan_hash1[h];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->len != 1)
                    continue;

                if (p->flags & B3G_NOCASE) {
                    if (bg_tolower(*buf) == p->ci[0]) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                } else {
                    if (*buf == p->cs[0]) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
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

    //printf("B3gSearch1: after 1byte cnt %u\n", cnt);
    if (ctx->scan_2_pat_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBScan2(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B3gSearch1: after 2+byte cnt %u\n", cnt);
    } else if (ctx->scan_x_pat_cnt) {
        cnt += ctx->MBScan(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
    }
    return cnt;
}

u_int32_t B3gSearch(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
#ifdef B3G_COUNTERS
    B3gThreadCtx *tctx = (B3gThreadCtx *)mpm_thread_ctx->ctx;
#endif
    u_int32_t pos = 0, matches = 0;
    B3G_TYPE d;
    u_int j;

    if (buflen < ctx->search_m)
        return 0;

    while (pos <= (buflen - ctx->search_m)) {
        j = ctx->search_m - 2;
        d = ~0;

        do {
            u_int16_t h = B3G_HASH(bg_tolower(buf[pos + j - 1]), bg_tolower(buf[pos + j]),bg_tolower(buf[pos + j + 1]));
            d &= ctx->search_B3G[h];
            d <<= 1;
            j = j - 1;
            //printf("h %u d %d %c.%c\n", h, d, bg_tolower(buf[pos + j - 1]),bg_tolower(buf[pos + j]));
        } while (d != 0 && j != 0);

        /* (partial) match, move on to verification */
        if (d != 0) {
            COUNT(tctx->search_stat_d0++);

            /* get our patterns from the hash */
            u_int16_t h = B3G_HASH(bg_tolower(buf[pos + ctx->search_m - 3]), bg_tolower(buf[pos + ctx->search_m - 2]),bg_tolower(buf[pos + ctx->search_m - 1]));
            B3gHashItem *hi = ctx->search_hash[h], *thi;
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                B3gPattern *p = ctx->parray[thi->idx];
                if (p->flags & B3G_NOCASE) {
                    if (buflen - pos < p->len)
                        continue;

                    if (memcmp_lowercase(p->ci, buf+pos, p->len) == 0) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf("\n");
                        COUNT(tctx->search_stat_loop_match++);

                        MpmEndMatch *em; 
                        for (em = p->em; em; em = em->next) {
                            //printf("em %p id %u\n", em, em->id);
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
                            //printf("em %p id %u\n", em, em->id);
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id], pos, p->len))
                                matches++;
                        }

                    } else {
                        COUNT(tctx->search_stat_loop_no_match++);
                    }
                }
            }

            //printf("output at pos %d: ", pos); prt(buf + pos, ctx->search_m); printf("\n");
            pos = pos + 1;
        } else {
            COUNT(tctx->search_stat_num_shift++);
            COUNT(tctx->search_stat_total_shift += (j + 1));
            pos = pos + j + 1;
        }
    }

    //printf("Total matches %u\n", matches);
    return matches;
}

u_int32_t B3gSearch2(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int8_t *bufmin = buf;
    u_int8_t *bufend = buf + buflen - 1;
    u_int32_t cnt = 0;
    B3gPattern *p;
    MpmEndMatch *em; 
    B3gHashItem *thi, *hi;

    if (buflen < 2)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    while (buf <= bufend) {
        u_int16_t h = bg_tolower(*buf) << 8 | bg_tolower(*(buf+1));
        hi = &ctx->search_hash2[h];

        if (hi->flags & 0x01) {
            for (thi = hi; thi != NULL; thi = thi->nxt) {
                p = ctx->parray[thi->idx];

                if (p->len != 2)
                    continue;

                if (p->flags & B3G_NOCASE) {
                    if (bg_tolower(*buf) == p->ci[0] && bg_tolower(*(buf+1)) == p->ci[1]) {
                        //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
                        for (em = p->em; em; em = em->next) {
                            if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                cnt++;
                        }
                    }
                } else {
                    if (*buf == p->cs[0] && *(buf+1) == p->cs[1]) {
                        //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
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

    //printf("B3gSearch2: after 2byte cnt %u\n", cnt);
    if (ctx->search_x_pat_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBSearch(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B3gSearch1: after 2+byte cnt %u\n", cnt);
    }
    return cnt;
}

u_int32_t B3gSearch1(MpmCtx *mpm_ctx, MpmThreadCtx *mpm_thread_ctx, PatternMatcherQueue *pmq, u_int8_t *buf, u_int16_t buflen) {
    B3gCtx *ctx = (B3gCtx *)mpm_ctx->ctx;
    u_int8_t *bufmin = buf;
    u_int8_t *bufend = buf + buflen - 1;
    u_int32_t cnt = 0;
    B3gPattern *p;
    MpmEndMatch *em; 
    B3gHashItem *thi, *hi;

    if (buflen == 0)
        return 0;

    //printf("BUF "); prt(buf,buflen); printf("\n");

    if (mpm_ctx->search_minlen == 1) {
        while (buf <= bufend) {
            u_int8_t h = bg_tolower(*buf);
            hi = &ctx->search_hash1[h];

            if (hi->flags & 0x01) {
                for (thi = hi; thi != NULL; thi = thi->nxt) {
                    p = ctx->parray[thi->idx];

                    if (p->len != 1)
                        continue;

                    if (p->flags & B3G_NOCASE) {
                        if (bg_tolower(*buf) == p->ci[0]) {
                            //printf("CI Exact match: "); prt(p->ci, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
                            for (em = p->em; em; em = em->next) {
                                if (MpmMatchAppend(mpm_thread_ctx, pmq, em, &mpm_thread_ctx->match[em->id],(buf+1 - bufmin), p->len))
                                    cnt++;
                            }
                        }
                    } else {
                        if (*buf == p->cs[0]) {
                            //printf("CS Exact match: "); prt(p->cs, p->len); printf(" in buf "); prt(buf, p->len);printf(" (B3gSearch1)\n");
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
    //printf("B3gSearch1: after 1byte cnt %u\n", cnt);
    if (ctx->search_2_pat_cnt) {
        /* Pass bufmin on because buf no longer points to the
         * start of the buffer. */
        cnt += ctx->MBSearch2(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
        //printf("B3gSearch1: after 2+byte cnt %u\n", cnt);
    } else if (ctx->search_x_pat_cnt) {
        cnt += ctx->MBSearch(mpm_ctx, mpm_thread_ctx, pmq, bufmin, buflen);
    }
    return cnt;
}

/*
 * TESTS
 */

static int B3gTestInit01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */


    B3gPreparePatterns(&mpm_ctx);


    if (ctx->search_m == 4)
        result = 1;
    else
        printf("4 != %u ", ctx->search_m);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}

#if 0
static int B3gTestS0Init01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 4)
        result = 1;
    else
        printf("4 != %u ", ctx->scan_s0);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestS0Init02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"cdef", 4, 0, 0, 1, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 2)
        result = 1;
    else
        printf("2 != %u ", ctx->scan_s0);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestS0Init03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"bcde", 4, 0, 0, 1, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 1)
        result = 1;
    else
        printf("1 != %u ", ctx->scan_s0);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestS0Init04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abab", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 2)
        result = 1;
    else
        printf("2 != %u ", ctx->scan_s0);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestS0Init05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcab", 5, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);

    if (ctx->scan_s0 == 3)
        result = 1;
    else
        printf("3 != %u ", ctx->scan_s0);

    B3gDestroyCtx(&mpm_ctx);
    return result;
}
#endif

static int B3gTestScan01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abce", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"bcde", 4, 0, 0, 1, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"fghj", 4, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

/* test patterns longer than 'm'. M is 4 here. */
static int B3gTestScan04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"bcdegh", 6, 0, 0, 1, 0); /* 1 match */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"fghjxyz", 7, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

/* case insensitive test patterns longer than 'm'. M is 4 here. */
static int B3gTestScan05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCI(&mpm_ctx, (u_int8_t *)"ABCD", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddScanPatternCI(&mpm_ctx, (u_int8_t *)"bCdEfG", 6, 0, 0, 1, 0); /* 1 match */
    B3gAddScanPatternCI(&mpm_ctx, (u_int8_t *)"fghJikl", 7, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcd", 4);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan07 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    //B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"A", 1, 0, 0, 0, 0); /* should match 30 times */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"AA", 2, 0, 0, 1, 0); /* should match 29 times */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"AAA", 3, 0, 0, 2, 0); /* should match 28 times */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"AAAAA", 5, 0, 0, 3, 0); /* 26 */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0); /* 21 */
    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0); /* 1 */
    /* total matches: 135 */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 135)
        result = 1;
    else
        printf("135 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan08 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"a", 1);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan09 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"ab", 2, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"ab", 2);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestScan10 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddScanPatternCS(&mpm_ctx, (u_int8_t *)"abcdefgh", 8, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Scan(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679abcdefgh012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679", 208);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch01 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch02 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abce", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch03 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"bcde", 4, 0, 0, 1, 0); /* 1 match */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"fghj", 4, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

/* test patterns longer than 'm'. M is 4 here. */
static int B3gTestSearch04 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"bcdegh", 6, 0, 0, 1, 0); /* 1 match */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"fghjxyz", 7, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

/* case insensitive test patterns longer than 'm'. M is 4 here. */
static int B3gTestSearch05 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCI(&mpm_ctx, (u_int8_t *)"ABCD", 4, 0, 0, 0, 0); /* 1 match */
    B3gAddPatternCI(&mpm_ctx, (u_int8_t *)"bCdEfG", 6, 0, 0, 1, 0); /* 1 match */
    B3gAddPatternCI(&mpm_ctx, (u_int8_t *)"fghJikl", 7, 0, 0, 2, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 3 /* 3 patterns */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcdefghjiklmnopqrstuvwxyz", 26);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 3)
        result = 1;
    else
        printf("3 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch06 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"abcd", 4);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch07 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);
    //B3gCtx *ctx = (B3gCtx *)mpm_ctx.ctx;

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"A", 1, 0, 0, 0, 0); /* should match 30 times */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"AA", 2, 0, 0, 1, 0); /* should match 29 times */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"AAA", 3, 0, 0, 2, 0); /* should match 28 times */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"AAAAA", 5, 0, 0, 3, 0); /* 26 */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"AAAAAAAAAA", 10, 0, 0, 4, 0); /* 21 */
    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30, 0, 0, 5, 0); /* 1 */
    /* total matches: 135 */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 6 /* 6 patterns */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 30);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 135)
        result = 1;
    else
        printf("135 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch08 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcd", 4, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"a", 1);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 0)
        result = 1;
    else
        printf("0 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch09 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"ab", 2, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"ab", 2);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

static int B3gTestSearch10 (void) {
    int result = 0;
    MpmCtx mpm_ctx;
    MpmThreadCtx mpm_thread_ctx;
    MpmInitCtx(&mpm_ctx, MPM_B3G);

    B3gAddPatternCS(&mpm_ctx, (u_int8_t *)"abcdefgh", 8, 0, 0, 0, 0); /* 1 match */

    B3gPreparePatterns(&mpm_ctx);
    B3gThreadInitCtx(&mpm_ctx, &mpm_thread_ctx, 1 /* 1 pattern */);

    u_int32_t cnt = mpm_ctx.Search(&mpm_ctx, &mpm_thread_ctx, NULL, (u_int8_t *)"012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679abcdefgh012345679012345679012345679012345679012345679012345679012345679012345679012345679012345679", 208);

    MpmMatchCleanup(&mpm_thread_ctx);

    if (cnt == 1)
        result = 1;
    else
        printf("1 != %u ",cnt);

    B3gThreadDestroyCtx(&mpm_ctx, &mpm_thread_ctx);
    B3gDestroyCtx(&mpm_ctx);
    return result;
}

void B3gRegisterTests(void) {
    UtRegisterTest("B3gTestInit01", B3gTestInit01, 1);
/*
    UtRegisterTest("B3gTestS0Init01", B3gTestS0Init01, 1);
    UtRegisterTest("B3gTestS0Init02", B3gTestS0Init02, 1);
    UtRegisterTest("B3gTestS0Init03", B3gTestS0Init03, 1);
    UtRegisterTest("B3gTestS0Init04", B3gTestS0Init04, 1);
    UtRegisterTest("B3gTestS0Init05", B3gTestS0Init05, 1);
*/
    UtRegisterTest("B3gTestScan01", B3gTestScan01, 1);

    UtRegisterTest("B3gTestScan02", B3gTestScan02, 1);
    UtRegisterTest("B3gTestScan03", B3gTestScan03, 1);
    UtRegisterTest("B3gTestScan04", B3gTestScan04, 1);
    UtRegisterTest("B3gTestScan05", B3gTestScan05, 1);
    UtRegisterTest("B3gTestScan06", B3gTestScan06, 1);
    UtRegisterTest("B3gTestScan07", B3gTestScan07, 1);
    UtRegisterTest("B3gTestScan08", B3gTestScan08, 1);
    UtRegisterTest("B3gTestScan09", B3gTestScan09, 1);
    UtRegisterTest("B3gTestScan10", B3gTestScan10, 1);

    UtRegisterTest("B3gTestSearch01", B3gTestSearch01, 1);
    UtRegisterTest("B3gTestSearch02", B3gTestSearch02, 1);
    UtRegisterTest("B3gTestSearch03", B3gTestSearch03, 1);
    UtRegisterTest("B3gTestSearch04", B3gTestSearch04, 1);
    UtRegisterTest("B3gTestSearch05", B3gTestSearch05, 1);
    UtRegisterTest("B3gTestSearch06", B3gTestSearch06, 1);
    UtRegisterTest("B3gTestSearch07", B3gTestSearch07, 1);
    UtRegisterTest("B3gTestSearch08", B3gTestSearch08, 1);
    UtRegisterTest("B3gTestSearch09", B3gTestSearch09, 1);
    UtRegisterTest("B3gTestSearch10", B3gTestSearch10, 1);

}

#if 0
int main () {
#define R 4
int i;
    B3gCtx bg_ctx;
    B3gInitCtx(&bg_ctx);

    B3gAddPatternCI(&bg_ctx, "grep", 4, 0, 0, 0, 0);
    B2pPrepare(&bg_ctx);

    B3GSearch(&bg_ctx,Text,strlen(Text));

    exit(EXIT_SUCCESS);
}
#endif
