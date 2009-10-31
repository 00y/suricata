/* sig group
 *
 *
 */

#include "eidps-common.h"
#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include "util-cidr.h"
#include "util-unittest.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-engine-address.h"
#include "detect-engine-mpm.h"
#include "detect-engine-siggroup.h"

#include "detect-content.h"
#include "detect-uricontent.h"

#include "util-hash.h"
#include "util-hashlist.h"

#include "util-debug.h"

/* prototypes */
int SigGroupHeadClearSigs(SigGroupHead *);

static uint32_t detect_siggroup_head_memory = 0;
static uint32_t detect_siggroup_head_init_cnt = 0;
static uint32_t detect_siggroup_head_free_cnt = 0;
static uint32_t detect_siggroup_head_initdata_memory = 0;
static uint32_t detect_siggroup_head_initdata_init_cnt = 0;
static uint32_t detect_siggroup_head_initdata_free_cnt = 0;
static uint32_t detect_siggroup_sigarray_memory = 0;
static uint32_t detect_siggroup_sigarray_init_cnt = 0;
static uint32_t detect_siggroup_sigarray_free_cnt = 0;
static uint32_t detect_siggroup_matcharray_memory = 0;
static uint32_t detect_siggroup_matcharray_init_cnt = 0;
static uint32_t detect_siggroup_matcharray_free_cnt = 0;

static SigGroupHeadInitData *SigGroupHeadInitDataAlloc(uint32_t size) {
    SigGroupHeadInitData *sghid = malloc(sizeof(SigGroupHeadInitData));
    if (sghid == NULL)
        return NULL;

    memset(sghid, 0x00, sizeof(SigGroupHeadInitData));

    detect_siggroup_head_initdata_init_cnt++;
    detect_siggroup_head_initdata_memory += sizeof(SigGroupHeadInitData);
    return sghid;
}

void SigGroupHeadInitDataFree(SigGroupHeadInitData *sghid) {
    if (sghid->content_array != NULL) {
        free(sghid->content_array);
        sghid->content_array = NULL;
        sghid->content_size = 0;
    }
    if (sghid->uri_content_array != NULL) {
        free(sghid->uri_content_array);
        sghid->uri_content_array = NULL;
        sghid->uri_content_size = 0;
    }
    free(sghid);

    detect_siggroup_head_initdata_free_cnt++;
    detect_siggroup_head_initdata_memory -= sizeof(SigGroupHeadInitData);
}

/**
 * \brief Alloc a sig group head and it's sig_array
 *
 * \param size Size of the sig array

 * \retval sgh Pointer to newly init SigGroupHead on succuess; or NULL in case
 *             of error
 */
static SigGroupHead *SigGroupHeadAlloc(uint32_t size) {
    SigGroupHead *sgh = malloc(sizeof(SigGroupHead));
    if (sgh == NULL) {
        return NULL;
    }
    memset(sgh, 0, sizeof(SigGroupHead));

    sgh->init = SigGroupHeadInitDataAlloc(size);
    if (sgh->init == NULL)
        goto error;

    detect_siggroup_head_init_cnt++;
    detect_siggroup_head_memory += sizeof(SigGroupHead);

    /* initialize the signature bitarray */
    sgh->sig_size = size;
    sgh->sig_array = malloc(sgh->sig_size);
    if (sgh->sig_array == NULL)
        goto error;
    memset(sgh->sig_array, 0, sgh->sig_size);

    detect_siggroup_sigarray_init_cnt++;
    detect_siggroup_sigarray_memory += sgh->sig_size;

    return sgh;
error:
    if (sgh != NULL)
        SigGroupHeadFree(sgh);
    return NULL;
}

/** \brief Free a sgh
 *  \param sgh the sig group head to free */
void SigGroupHeadFree(SigGroupHead *sgh) {
    SCLogDebug("sgh %p", sgh);

    if (sgh == NULL)
        return;

    PatternMatchDestroyGroup(sgh);

    if (sgh->sig_array != NULL) {
        free(sgh->sig_array);
        sgh->sig_array = NULL;

        detect_siggroup_sigarray_free_cnt++;
        detect_siggroup_sigarray_memory -= sgh->sig_size;
    }

    if (sgh->match_array != NULL) {
        detect_siggroup_matcharray_free_cnt++;
        detect_siggroup_matcharray_memory -= (sgh->sig_cnt * sizeof(SigIntId));
        free(sgh->match_array);
        sgh->match_array = NULL;
        sgh->sig_cnt = 0;
    }

    if (sgh->init != NULL) {
        SigGroupHeadInitDataFree(sgh->init);
    }

    free(sgh);

    detect_siggroup_head_free_cnt++;
    detect_siggroup_head_memory -= sizeof(SigGroupHead);
}

/*
 * initialization hashes
 */

/* mpm sgh hash */
uint32_t SigGroupHeadMpmHashFunc(HashListTable *ht, void *data, uint16_t datalen) {
    SigGroupHead *sgh = (SigGroupHead *)data;
    uint32_t hash = 0;

    uint32_t b;
    for (b = 0; b < sgh->init->content_size; b+=1) {
        hash += sgh->init->content_array[b];
    }
    return hash % ht->array_size;
}

char SigGroupHeadMpmCompareFunc(void *data1, uint16_t len1, void *data2, uint16_t len2) {
    SigGroupHead *sgh1 = (SigGroupHead *)data1;
    SigGroupHead *sgh2 = (SigGroupHead *)data2;

    if (sgh1->init->content_size != sgh2->init->content_size)
        return 0;

    if (memcmp(sgh1->init->content_array,sgh2->init->content_array,sgh1->init->content_size) != 0)
        return 0;

    return 1;
}

int SigGroupHeadMpmHashInit(DetectEngineCtx *de_ctx) {
    de_ctx->sgh_mpm_hash_table = HashListTableInit(4096, SigGroupHeadMpmHashFunc, SigGroupHeadMpmCompareFunc, NULL);
    if (de_ctx->sgh_mpm_hash_table == NULL)
        goto error;

    return 0;
error:
    return -1;
}

int SigGroupHeadMpmHashAdd(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableAdd(de_ctx->sgh_mpm_hash_table, (void *)sgh, 0);
}

SigGroupHead *SigGroupHeadMpmHashLookup(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    SigGroupHead *rsgh = HashListTableLookup(de_ctx->sgh_mpm_hash_table, (void *)sgh, 0);
    return rsgh;
}

void SigGroupHeadMpmHashFree(DetectEngineCtx *de_ctx) {
    if (de_ctx->sgh_mpm_hash_table == NULL)
        return;

    HashListTableFree(de_ctx->sgh_mpm_hash_table);
    de_ctx->sgh_mpm_hash_table = NULL;
}

/* mpm uri sgh hash */

uint32_t SigGroupHeadMpmUriHashFunc(HashListTable *ht, void *data, uint16_t datalen) {
    SigGroupHead *sgh = (SigGroupHead *)data;
    uint32_t hash = 0;

    uint32_t b;
    for (b = 0; b < sgh->init->uri_content_size; b+=1) {
        hash += sgh->init->uri_content_array[b];
    }
    return hash % ht->array_size;
}

char SigGroupHeadMpmUriCompareFunc(void *data1, uint16_t len1, void *data2, uint16_t len2) {
    SigGroupHead *sgh1 = (SigGroupHead *)data1;
    SigGroupHead *sgh2 = (SigGroupHead *)data2;

    if (sgh1->init->uri_content_size != sgh2->init->uri_content_size)
        return 0;

    if (memcmp(sgh1->init->uri_content_array,sgh2->init->uri_content_array,sgh1->init->uri_content_size) != 0)
        return 0;

    return 1;
}

int SigGroupHeadMpmUriHashInit(DetectEngineCtx *de_ctx) {
    de_ctx->sgh_mpm_uri_hash_table = HashListTableInit(4096, SigGroupHeadMpmUriHashFunc, SigGroupHeadMpmUriCompareFunc, NULL);
    if (de_ctx->sgh_mpm_uri_hash_table == NULL)
        goto error;

    return 0;
error:
    return -1;
}

int SigGroupHeadMpmUriHashAdd(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableAdd(de_ctx->sgh_mpm_uri_hash_table, (void *)sgh, 0);
}

SigGroupHead *SigGroupHeadMpmUriHashLookup(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    SigGroupHead *rsgh = HashListTableLookup(de_ctx->sgh_mpm_uri_hash_table, (void *)sgh, 0);
    return rsgh;
}

void SigGroupHeadMpmUriHashFree(DetectEngineCtx *de_ctx) {
    if (de_ctx->sgh_mpm_uri_hash_table == NULL)
        return;

    HashListTableFree(de_ctx->sgh_mpm_uri_hash_table);
    de_ctx->sgh_mpm_uri_hash_table = NULL;
}

/* non-port sgh hash */

uint32_t SigGroupHeadHashFunc(HashListTable *ht, void *data, uint16_t datalen) {
    SigGroupHead *sgh = (SigGroupHead *)data;
    uint32_t hash = 0;
    SCLogDebug("hashing sgh %p", sgh);

    uint32_t b;
    for (b = 0; b < sgh->sig_size; b+=1) {
        hash += sgh->sig_array[b];
    }

    hash %= ht->array_size;
    SCLogDebug("hash %"PRIu32" (sig_size %"PRIu32")", hash, sgh->sig_size);
    return hash;
}

char SigGroupHeadCompareFunc(void *data1, uint16_t len1, void *data2, uint16_t len2) {
    SigGroupHead *sgh1 = (SigGroupHead *)data1;
    SigGroupHead *sgh2 = (SigGroupHead *)data2;

    if (sgh1->sig_size != sgh2->sig_size)
        return 0;

    if (memcmp(sgh1->sig_array,sgh2->sig_array,sgh1->sig_size) != 0)
        return 0;

    return 1;
}

/* sgh */

int SigGroupHeadHashInit(DetectEngineCtx *de_ctx) {
    de_ctx->sgh_hash_table = HashListTableInit(4096, SigGroupHeadHashFunc, SigGroupHeadCompareFunc, NULL);
    if (de_ctx->sgh_hash_table == NULL)
        goto error;

    return 0;
error:
    return -1;
}

int SigGroupHeadHashAdd(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableAdd(de_ctx->sgh_hash_table, (void *)sgh, 0);
}

int SigGroupHeadHashRemove(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableRemove(de_ctx->sgh_hash_table, (void *)sgh, 0);
}

SigGroupHead *SigGroupHeadHashLookup(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    SigGroupHead *rsgh = HashListTableLookup(de_ctx->sgh_hash_table, (void *)sgh, 0);
    return rsgh;
}

void SigGroupHeadHashFree(DetectEngineCtx *de_ctx) {
    if (de_ctx->sgh_hash_table == NULL)
        return;

    HashListTableFree(de_ctx->sgh_hash_table);
    de_ctx->sgh_hash_table = NULL;
}

/* port based sgh hash */

/* dport */

int SigGroupHeadDPortHashInit(DetectEngineCtx *de_ctx) {
    de_ctx->sgh_dport_hash_table = HashListTableInit(4096, SigGroupHeadHashFunc, SigGroupHeadCompareFunc, NULL);
    if (de_ctx->sgh_dport_hash_table == NULL)
        goto error;

    return 0;
error:
    return -1;
}

int SigGroupHeadDPortHashAdd(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableAdd(de_ctx->sgh_dport_hash_table, (void *)sgh, 0);
}

SigGroupHead *SigGroupHeadDPortHashLookup(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    SigGroupHead *rsgh = HashListTableLookup(de_ctx->sgh_dport_hash_table, (void *)sgh, 0);
    return rsgh;
}

void SigGroupHeadDPortHashFree(DetectEngineCtx *de_ctx) {
    if (de_ctx->dport_hash_table == NULL)
        return;

    HashListTableFree(de_ctx->sgh_dport_hash_table);
    de_ctx->sgh_dport_hash_table = NULL;
}

/* sport */

int SigGroupHeadSPortHashInit(DetectEngineCtx *de_ctx) {
    de_ctx->sgh_sport_hash_table = HashListTableInit(4096, SigGroupHeadHashFunc, SigGroupHeadCompareFunc, NULL);
    if (de_ctx->sgh_sport_hash_table == NULL)
        goto error;

    return 0;
error:
    return -1;
}

int SigGroupHeadSPortHashAdd(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableAdd(de_ctx->sgh_sport_hash_table, (void *)sgh, 0);
}

int SigGroupHeadSPortHashRemove(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    return HashListTableRemove(de_ctx->sgh_sport_hash_table, (void *)sgh, 0);
}

SigGroupHead *SigGroupHeadSPortHashLookup(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    SigGroupHead *rsgh = HashListTableLookup(de_ctx->sgh_sport_hash_table, (void *)sgh, 0);
    return rsgh;
}

void SigGroupHeadSPortHashFree(DetectEngineCtx *de_ctx) {
    if (de_ctx->sport_hash_table == NULL)
        return;

    HashListTableFree(de_ctx->sgh_sport_hash_table);
    de_ctx->sgh_sport_hash_table = NULL;
}

/* end hashes */

static void SigGroupHeadFreeSigArraysHash2(DetectEngineCtx *de_ctx, HashListTable *ht) {
    HashListTableBucket *htb = NULL;

    for (htb = HashListTableGetListHead(ht); htb != NULL; htb = HashListTableGetListNext(htb)) {
        SigGroupHead *sgh = (SigGroupHead *)HashListTableGetListData(htb);

        if (sgh->sig_array != NULL) {
            detect_siggroup_sigarray_free_cnt++;
            detect_siggroup_sigarray_memory -= sgh->sig_size;

            free(sgh->sig_array);
            sgh->sig_array = NULL;
            sgh->sig_size = 0;
        }

        if (sgh->init != NULL) {
            SigGroupHeadInitDataFree(sgh->init);
            sgh->init = NULL;
        }
    }
}

static void SigGroupHeadFreeSigArraysHash(DetectEngineCtx *de_ctx, HashListTable *ht) {
    HashListTableBucket *htb = NULL;

    for (htb = HashListTableGetListHead(ht); htb != NULL; htb = HashListTableGetListNext(htb)) {
        SigGroupHead *sgh = (SigGroupHead *)HashListTableGetListData(htb);

        if (sgh->sig_array != NULL) {
            detect_siggroup_sigarray_free_cnt++;
            detect_siggroup_sigarray_memory -= sgh->sig_size;

            free(sgh->sig_array);
            sgh->sig_array = NULL;
            sgh->sig_size = 0;
        }

        if (sgh->init != NULL) {
            SigGroupHeadInitDataFree(sgh->init);
            sgh->init = NULL;
        }
    }
}

/* Free the sigarrays in the sgh's. Those are only
 * used during the init stage. */
void SigGroupHeadFreeSigArrays(DetectEngineCtx *de_ctx) {
    SigGroupHeadFreeSigArraysHash2(de_ctx, de_ctx->sgh_hash_table);

    SigGroupHeadFreeSigArraysHash(de_ctx, de_ctx->sgh_dport_hash_table);
    SigGroupHeadFreeSigArraysHash(de_ctx, de_ctx->sgh_sport_hash_table);
}

/* Free the mpm arrays that are only used during the
 * init stage */
void SigGroupHeadFreeMpmArrays(DetectEngineCtx *de_ctx) {
    HashListTableBucket *htb = NULL;

    for (htb = HashListTableGetListHead(de_ctx->sgh_dport_hash_table); htb != NULL; htb = HashListTableGetListNext(htb)) {
        SigGroupHead *sgh = (SigGroupHead *)HashListTableGetListData(htb);
        if (sgh->init != NULL) {
            SigGroupHeadInitDataFree(sgh->init);
            sgh->init = NULL;
        }
    }

    for (htb = HashListTableGetListHead(de_ctx->sgh_sport_hash_table); htb != NULL; htb = HashListTableGetListNext(htb)) {
        SigGroupHead *sgh = (SigGroupHead *)HashListTableGetListData(htb);
        if (sgh->init != NULL) {
            SigGroupHeadInitDataFree(sgh->init);
            sgh->init = NULL;
        }
    }
}

/** \brief Add a signature to a sgh
 *  \param de_ctx detection engine ctx
 *  \param sgh pointer to a sgh, can be NULL
 *  \param s signature to append
 *  \retval 0 success
 *  \retval -1 error
 */
int SigGroupHeadAppendSig(DetectEngineCtx *de_ctx, SigGroupHead **sgh, Signature *s) {
    if (de_ctx == NULL)
        return 0;

    /* see if we have a head already */
    if (*sgh == NULL) {
        *sgh = SigGroupHeadAlloc(DetectEngineGetMaxSigId(de_ctx) / 8 + 1);
        if (*sgh == NULL)
            goto error;
    }

    /* enable the sig in the bitarray */
    (*sgh)->sig_array[s->num / 8] |= 1 << (s->num % 8);

    /* update maxlen for mpm */
    if (s->flags & SIG_FLAG_MPM) {
        /* check with the precalculated values from the sig */
        if (s->mpm_content_maxlen > 0) {
            if ((*sgh)->mpm_content_maxlen == 0)
                (*sgh)->mpm_content_maxlen = s->mpm_content_maxlen;

            if ((*sgh)->mpm_content_maxlen > s->mpm_content_maxlen)
                (*sgh)->mpm_content_maxlen = s->mpm_content_maxlen;
        }
        if (s->mpm_uricontent_maxlen > 0) {
            if ((*sgh)->mpm_uricontent_maxlen == 0)
                (*sgh)->mpm_uricontent_maxlen = s->mpm_uricontent_maxlen;

            if ((*sgh)->mpm_uricontent_maxlen > s->mpm_uricontent_maxlen)
                (*sgh)->mpm_uricontent_maxlen = s->mpm_uricontent_maxlen;
        }
    }
    return 0;
error:
    return -1;
}

int SigGroupHeadClearSigs(SigGroupHead *sgh) {
    if (sgh == NULL)
        return 0;

    if (sgh->sig_array != NULL) {
        memset(sgh->sig_array,0,sgh->sig_size);
    }
    sgh->sig_cnt = 0;

    sgh->mpm_content_maxlen = 0;
    sgh->mpm_uricontent_maxlen = 0;
    return 0;
}

/** \brief copy signature array from one sgh to another */
int SigGroupHeadCopySigs(DetectEngineCtx *de_ctx, SigGroupHead *src, SigGroupHead **dst) {
    if (src == NULL || de_ctx == NULL)
        return 0;

    if (*dst == NULL) {
        *dst = SigGroupHeadAlloc(DetectEngineGetMaxSigId(de_ctx) / 8 + 1);
        if (*dst == NULL) {
            goto error;
        }
    }

    /* do the copy */
    uint32_t idx;
    for (idx = 0; idx < src->sig_size; idx++) {
        (*dst)->sig_array[idx] = (*dst)->sig_array[idx] | src->sig_array[idx];
    }

    if (src->mpm_content_maxlen != 0) {
        if ((*dst)->mpm_content_maxlen == 0)
            (*dst)->mpm_content_maxlen = src->mpm_content_maxlen;

        if ((*dst)->mpm_content_maxlen > src->mpm_content_maxlen)
            (*dst)->mpm_content_maxlen = src->mpm_content_maxlen;

        SCLogDebug("src (%p)->mpm_content_maxlen %u", src, src->mpm_content_maxlen);
        SCLogDebug("dst (%p)->mpm_content_maxlen %u", (*dst), (*dst)->mpm_content_maxlen);
        BUG_ON((*dst)->mpm_content_maxlen == 0);
    }
    if (src->mpm_uricontent_maxlen != 0) {
        if ((*dst)->mpm_uricontent_maxlen == 0)
            (*dst)->mpm_uricontent_maxlen = src->mpm_uricontent_maxlen;

        if ((*dst)->mpm_uricontent_maxlen > src->mpm_uricontent_maxlen)
            (*dst)->mpm_uricontent_maxlen = src->mpm_uricontent_maxlen;
    }
    return 0;
error:
    return -1;
}

void SigGroupHeadSetSigCnt(SigGroupHead *sgh, uint32_t max_idx) {
    uint32_t sig;

    for (sig = 0; sig < max_idx+1; sig++) {
        if (sgh->sig_array[(sig/8)] & (1<<(sig%8))) {
            sgh->sig_cnt++;
        }
    }
}

void DetectSigGroupPrintMemory(void) {
    printf(" * Sig group head memory stats (SigGroupHead %" PRIuMAX "):\n", (uintmax_t)sizeof(SigGroupHead));
    printf("  - detect_siggroup_head_memory %" PRIu32 "\n", detect_siggroup_head_memory);
    printf("  - detect_siggroup_head_init_cnt %" PRIu32 "\n", detect_siggroup_head_init_cnt);
    printf("  - detect_siggroup_head_free_cnt %" PRIu32 "\n", detect_siggroup_head_free_cnt);
    printf("  - outstanding sig group heads %" PRIu32 "\n", detect_siggroup_head_init_cnt - detect_siggroup_head_free_cnt);
    printf(" * Sig group head memory stats done\n");
    printf(" * Sig group head initdata memory stats (SigGroupHeadInitData %" PRIuMAX "):\n", (uintmax_t)sizeof(SigGroupHeadInitData));
    printf("  - detect_siggroup_head_initdata_memory %" PRIu32 "\n", detect_siggroup_head_initdata_memory);
    printf("  - detect_siggroup_head_initdata_init_cnt %" PRIu32 "\n", detect_siggroup_head_initdata_init_cnt);
    printf("  - detect_siggroup_head_initdata_free_cnt %" PRIu32 "\n", detect_siggroup_head_initdata_free_cnt);
    printf("  - outstanding sig group head initdatas %" PRIu32 "\n", detect_siggroup_head_initdata_init_cnt - detect_siggroup_head_initdata_free_cnt);
    printf(" * Sig group head memory initdata stats done\n");
    printf(" * Sig group sigarray memory stats:\n");
    printf("  - detect_siggroup_sigarray_memory %" PRIu32 "\n", detect_siggroup_sigarray_memory);
    printf("  - detect_siggroup_sigarray_init_cnt %" PRIu32 "\n", detect_siggroup_sigarray_init_cnt);
    printf("  - detect_siggroup_sigarray_free_cnt %" PRIu32 "\n", detect_siggroup_sigarray_free_cnt);
    printf("  - outstanding sig group sigarrays %" PRIu32 "\n", detect_siggroup_sigarray_init_cnt - detect_siggroup_sigarray_free_cnt);
    printf(" * Sig group sigarray memory stats done\n");
    printf(" * Sig group matcharray memory stats:\n");
    printf("  - detect_siggroup_matcharray_memory %" PRIu32 "\n", detect_siggroup_matcharray_memory);
    printf("  - detect_siggroup_matcharray_init_cnt %" PRIu32 "\n", detect_siggroup_matcharray_init_cnt);
    printf("  - detect_siggroup_matcharray_free_cnt %" PRIu32 "\n", detect_siggroup_matcharray_free_cnt);
    printf("  - outstanding sig group matcharrays %" PRIu32 "\n", detect_siggroup_matcharray_init_cnt - detect_siggroup_matcharray_free_cnt);
    printf(" * Sig group sigarray memory stats done\n");
    printf(" X Total %" PRIu32 "\n", detect_siggroup_head_memory + detect_siggroup_sigarray_memory + detect_siggroup_matcharray_memory);
}

void SigGroupHeadPrintSigs(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    printf("SigGroupHeadPrintSigs: ");

    uint32_t i;
    for (i = 0; i < sgh->sig_size; i++) {
        if (sgh->sig_array[(i/8)] & (1<<(i%8))) {
            printf("%" PRIu32 " ", i);
        }
    }

    printf("\n");
}

void SigGroupHeadPrintContent(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    printf("SigGroupHeadPrintContent: ");

    uint32_t i;
    for (i = 0; i < DetectContentMaxId(de_ctx); i++) {
        if (sgh->init->content_array[(i/8)] & (1<<(i%8))) {
            printf("%" PRIu32 " ", i);
        }
    }

    printf("\n");
}

void SigGroupHeadPrintContentCnt(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    printf("SigGroupHeadPrintContent: ");

    uint32_t i, cnt = 0;
    for (i = 0; i < DetectContentMaxId(de_ctx); i++) {
        if (sgh->init->content_array[(i/8)] & (1<<(i%8))) {
            cnt++;
        }
    }

    printf("cnt %" PRIu32 "\n", cnt);
}

/* load all pattern id's into a single bitarray that we can memcmp
 * with other bitarrays. A fast and efficient way of comparing pattern
 * sets. */
int SigGroupHeadLoadContent(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    if (sgh == NULL)
        return 0;

    Signature *s;
    SigMatch *sm;

    if (DetectContentMaxId(de_ctx) == 0)
        return 0;

    BUG_ON(sgh->init == NULL);

    sgh->init->content_size = (DetectContentMaxId(de_ctx) / 8) + 1;
    sgh->init->content_array = malloc(sgh->init->content_size);
    if (sgh->init->content_array == NULL)
        return -1;

    memset(sgh->init->content_array,0, sgh->init->content_size);

    uint32_t sig;
    for (sig = 0; sig < sgh->sig_cnt; sig++) {
        SigIntId num = sgh->match_array[sig];

        s = de_ctx->sig_array[num];
        if (s == NULL)
            continue;

        if (!(s->flags & SIG_FLAG_MPM))
            continue;

        sm = s->match;
        if (sm == NULL)
            continue;

        for ( ; sm != NULL; sm = sm->next) {
            if (sm->type == DETECT_CONTENT) {
                DetectContentData *co = (DetectContentData *)sm->ctx;

                sgh->init->content_array[(co->id/8)] |= 1<<(co->id%8);
            }
        }
    }

    return 0;
}

int SigGroupHeadClearContent(SigGroupHead *sh) {
    if (sh == NULL || sh->init == NULL)
        return 0;

    if (sh->init->content_array != NULL) {
        free(sh->init->content_array);
        sh->init->content_array = NULL;
        sh->init->content_size = 0;
    }
    return 0;
}

int SigGroupHeadLoadUricontent(DetectEngineCtx *de_ctx, SigGroupHead *sgh) {
    if (sgh == NULL)
        return 0;

    Signature *s;
    SigMatch *sm;

    if (DetectUricontentMaxId(de_ctx) == 0)
        return 0;

    BUG_ON(sgh->init == NULL);

    sgh->init->uri_content_size = (DetectUricontentMaxId(de_ctx) / 8) + 1;
    sgh->init->uri_content_array = malloc(sgh->init->uri_content_size);
    if (sgh->init->uri_content_array == NULL)
        return -1;

    memset(sgh->init->uri_content_array, 0, sgh->init->uri_content_size);

    uint32_t sig;
    for (sig = 0; sig < sgh->sig_cnt; sig++) {
        SigIntId num = sgh->match_array[sig];

        s = de_ctx->sig_array[num];
        if (s == NULL)
            continue;

        if (!(s->flags & SIG_FLAG_MPM))
            continue;

        sm = s->match;
        if (sm == NULL)
            continue;

        for ( ; sm != NULL; sm = sm->next) {
            if (sm->type == DETECT_URICONTENT) {
                DetectUricontentData *co = (DetectUricontentData *)sm->ctx;

                sgh->init->uri_content_array[(co->id/8)] |= 1<<(co->id%8);
            }
        }
    }
    return 0;
}

int SigGroupHeadClearUricontent(SigGroupHead *sh) {
    if (sh == NULL||sh->init == NULL)
        return 0;

    if (sh->init->uri_content_array != NULL) {
        free(sh->init->uri_content_array);
        sh->init->uri_content_array = NULL;
        sh->init->uri_content_size = 0;
    }

    return 0;
}

/** \brief Create an array with all the internal id's of the sigs that this
 *         sig group head will check for.
 *  \param de_ctx detection engine ctx
 *  \param sgh sig group head
 *  \param max_idx max idx for deternmining the array size
 *  \retval 0 success
 *  \retval -1 error
 */
int SigGroupHeadBuildMatchArray (DetectEngineCtx *de_ctx, SigGroupHead *sgh, uint32_t max_idx) {
    uint32_t idx = 0;
    uint32_t sig = 0;

    if (sgh == NULL)
        return 0;

    BUG_ON(sgh->match_array != NULL);

    sgh->match_array = malloc(sgh->sig_cnt * sizeof(SigIntId));
    if (sgh->match_array == NULL)
        return -1;

    memset(sgh->match_array,0, sgh->sig_cnt * sizeof(SigIntId));

    detect_siggroup_matcharray_init_cnt++;
    detect_siggroup_matcharray_memory += (sgh->sig_cnt * sizeof(SigIntId));

    for (sig = 0; sig < max_idx+1; sig++) {
        if (!(sgh->sig_array[(sig/8)] & (1<<(sig%8))))
            continue;

        Signature *s = de_ctx->sig_array[sig];
        if (s == NULL)
            continue;

        sgh->match_array[idx] = s->num;
        idx++;
    }

    return 0;
}

/** \brief Check if a sgh contains a sid
 *  \param de_ctx detection engine ctx
 *  \param sgh sig group head
 *  \param sid the signature id to check for
 *  \retval 0 no
 *  \retval 1 yes
 */
int SigGroupHeadContainsSigId (DetectEngineCtx *de_ctx, SigGroupHead *sgh, uint32_t sid) {
    uint32_t sig = 0;

    if (sgh == NULL)
        return 0;

    for (sig = 0; sig < sgh->sig_cnt; sig++) {
        if (sgh->sig_array == NULL)
            return 0;

        if (!(sgh->sig_array[(sig/8)] & (1<<(sig%8))))
            continue;

        Signature *s = de_ctx->sig_array[sig];
        if (s == NULL)
            continue;

        if (s->id == sid)
            return 1;
    }

    return 0;
}
