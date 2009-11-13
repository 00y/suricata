/**
 * Copyright (c) 2009 Open Information Security Foundation
 *
 * \file
 * \author Victor Julien <victor@inliniac.net>
 */

#include "eidps-common.h"
#include "threads.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"

#include "flow.h"
#include "flow-var.h"

#include "util-debug.h"
#include "util-unittest.h"

#include "app-layer.h"

#include "app-layer-tls.h"
#include "app-layer-tls-detect-version.h"


/**
 * \brief Regex for parsing "id" option, matching number or "number"
 */
#define PARSE_REGEX  "^\\s*([A-z0-9\\.]+|\"[A-z0-9\\.]+\")\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int AppLayerTlsDetectVersionMatch (ThreadVars *, DetectEngineThreadCtx *, Flow *, uint8_t, void *, Signature *, SigMatch *);
int AppLayerTlsDetectVersionSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void AppLayerTlsDetectVersionRegisterTests(void);
void AppLayerTlsDetectVersionFree(void *);

/**
 * \brief Registration function for keyword: tls.version
 */
void AppLayerTlsDetectVersionRegister (void) {
    sigmatch_table[DETECT_AL_TLS_VERSION].name = "tls.version";
    sigmatch_table[DETECT_AL_TLS_VERSION].Match = NULL;
    sigmatch_table[DETECT_AL_TLS_VERSION].AppLayerMatch = AppLayerTlsDetectVersionMatch;
    sigmatch_table[DETECT_AL_TLS_VERSION].Setup = AppLayerTlsDetectVersionSetup;
    sigmatch_table[DETECT_AL_TLS_VERSION].Free  = AppLayerTlsDetectVersionFree;
    sigmatch_table[DETECT_AL_TLS_VERSION].RegisterTests = AppLayerTlsDetectVersionRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

	SCLogDebug("registering tls.version rule option\n");

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if (parse_regex == NULL) {
        SCLogDebug("Compile of \"%s\" failed at offset %" PRId32 ": %s\n",
                    PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if (eb != NULL) {
        SCLogDebug("pcre study failed: %s\n", eb);
        goto error;
    }
    return;

error:
    return;
}

/**
 * \brief match the specified version on a tls session
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into AppLayerTlsDetectVersionData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int AppLayerTlsDetectVersionMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f, uint8_t flags, void *state, Signature *s, SigMatch *m)
{
    AppLayerTlsDetectVersionData *tls_data = (AppLayerTlsDetectVersionData *)m->ctx;
    TlsState *tls_state = (TlsState *)state;
    if (tls_state == NULL)
        return 0;

    int ret = 0;
    mutex_lock(&f->m);
    if (flags & STREAM_TOCLIENT) {
        if (tls_data->ver == tls_state->client_version)
            ret = 1;
    } else if (flags & STREAM_TOSERVER) {
        if (tls_data->ver == tls_state->server_version)
            ret = 1;
    }
    mutex_unlock(&f->m);
    return ret;
}

/**
 * \brief This function is used to parse IPV4 ip_id passed via keyword: "id"
 *
 * \param idstr Pointer to the user provided id option
 *
 * \retval id_d pointer to AppLayerTlsDetectVersionData on success
 * \retval NULL on failure
 */
AppLayerTlsDetectVersionData *AppLayerTlsDetectVersionParse (char *str)
{
    uint8_t temp;
    AppLayerTlsDetectVersionData *tls = NULL;
	#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, str, strlen(str), 0, 0,
                    ov, MAX_SUBSTRINGS);

    if (ret < 1 || ret > 3) {
        SCLogDebug("invalid tls.version option");
        goto error;
    }

    if (ret > 1) {
        const char *str_ptr;
        char *orig;
        char *tmp_str;
        res = pcre_get_substring((char *)str, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            SCLogDebug("AppLayerTlsDetectVersionParse: pcre_get_substring failed\n");
            goto error;
        }

        /* We have a correct id option */
        tls = malloc(sizeof(AppLayerTlsDetectVersionData));
        if (tls == NULL) {
            SCLogDebug("AppLayerTlsDetectVersionParse malloc failed\n");
            goto error;
        }

        orig = strdup((char*)str_ptr);
        tmp_str=orig;
        /* Let's see if we need to scape "'s */
        if (tmp_str[0] == '"')
        {
            tmp_str[strlen(tmp_str) - 1] = '\0';
            tmp_str += 1;
        }

        if (strcmp("1.0", tmp_str) == 0) {
            temp = TLS_VERSION_10;
        } else if (strcmp("1.1", tmp_str) == 0) {
            temp = TLS_VERSION_11;
        } else if (strcmp("1.2", tmp_str) == 0) {
            temp = TLS_VERSION_12;
        } else {
            goto error;
        }

        tls->ver = temp;

        free(orig);

        SCLogDebug("will look for tls %"PRIu8"\n", tls->ver);
    }

    return tls;

error:
    if (tls != NULL)
        AppLayerTlsDetectVersionFree(tls);
    return NULL;

}

/**
 * \brief this function is used to add the parsed "id" option
 * \brief into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param idstr pointer to the user provided "id" option
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int AppLayerTlsDetectVersionSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *str)
{
    AppLayerTlsDetectVersionData *tls = NULL;
    SigMatch *sm = NULL;

    tls = AppLayerTlsDetectVersionParse(str);
    if (tls == NULL) goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_AL_TLS_VERSION;
    sm->ctx = (void *)tls;

    SigMatchAppend(s,m,sm);

    return 0;

error:
    if (tls != NULL) AppLayerTlsDetectVersionFree(tls);
    if (sm != NULL) free(sm);
    return -1;

}

/**
 * \brief this function will free memory associated with AppLayerTlsDetectVersionData
 *
 * \param id_d pointer to AppLayerTlsDetectVersionData
 */
void AppLayerTlsDetectVersionFree(void *ptr) {
    AppLayerTlsDetectVersionData *id_d = (AppLayerTlsDetectVersionData *)ptr;
    free(id_d);
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test AppLayerTlsDetectVersionTestParse01 is a test to make sure that we parse the "id"
 *       option correctly when given valid id option
 */
int AppLayerTlsDetectVersionTestParse01 (void) {
    AppLayerTlsDetectVersionData *tls = NULL;
    tls = AppLayerTlsDetectVersionParse("1.0");
    if (tls != NULL && tls->ver == TLS_VERSION_10) {
        AppLayerTlsDetectVersionFree(tls);
        return 1;
    }

    return 0;
}

/**
 * \test AppLayerTlsDetectVersionTestParse02 is a test to make sure that we parse the "id"
 *       option correctly when given an invalid id option
 *       it should return id_d = NULL
 */
int AppLayerTlsDetectVersionTestParse02 (void) {
    AppLayerTlsDetectVersionData *tls = NULL;
    tls = AppLayerTlsDetectVersionParse("2.5");
    if (tls == NULL) {
        AppLayerTlsDetectVersionFree(tls);
        return 1;
    }

    return 0;
}

#include "stream-tcp-reassemble.h"

/** \test Send a get request in three chunks + more data. */
static int AppLayerTlsDetectVersionTestDetect01(void) {
    int result = 1;
    Flow f;
    uint8_t tlsbuf1[] = { 0x16 };
    uint32_t tlslen1 = sizeof(tlsbuf1);
    uint8_t tlsbuf2[] = { 0x03 };
    uint32_t tlslen2 = sizeof(tlsbuf2);
    uint8_t tlsbuf3[] = { 0x01 };
    uint32_t tlslen3 = sizeof(tlsbuf3);
    uint8_t tlsbuf4[] = { 0x01, 0x00, 0x00, 0xad, 0x03, 0x01 };
    uint32_t tlslen4 = sizeof(tlsbuf4);
    TcpSession ssn;
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = NULL;
    p.payload_len = 0;
    p.proto = IPPROTO_TCP;

    StreamL7DataPtrInit(&ssn,StreamL7GetStorageSize());
    f.protoctx = (void *)&ssn;
    p.flow = &f;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tls any any -> any any (msg:\"TLS\"; tls.version:1.0; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf1, tlslen1, FALSE);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf2, tlslen2, FALSE);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf3, tlslen3, FALSE);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf4, tlslen4, FALSE);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    TlsState *tls_state = ssn.aldata[AlpGetStateIdx(ALPROTO_TLS)];
    if (tls_state == NULL) {
        printf("no tls state: ");
        result = 0;
        goto end;
    }

    if (tls_state->client_content_type != 0x16) {
        printf("expected content_type %" PRIu8 ", got %" PRIu8 ": ", 0x16, tls_state->client_content_type);
        result = 0;
        goto end;
    }

    if (tls_state->client_version != TLS_VERSION_10) {
        printf("expected version %04" PRIu16 ", got %04" PRIu16 ": ", TLS_VERSION_10, tls_state->client_version);
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    if (!(PacketAlertCheck(&p, 1))) {
        goto end;
    }

    result = 1;
end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

static int AppLayerTlsDetectVersionTestDetect02(void) {
    int result = 1;
    Flow f;
    uint8_t tlsbuf1[] = { 0x16 };
    uint32_t tlslen1 = sizeof(tlsbuf1);
    uint8_t tlsbuf2[] = { 0x03 };
    uint32_t tlslen2 = sizeof(tlsbuf2);
    uint8_t tlsbuf3[] = { 0x01 };
    uint32_t tlslen3 = sizeof(tlsbuf3);
    uint8_t tlsbuf4[] = { 0x01, 0x00, 0x00, 0xad, 0x03, 0x02 };
    uint32_t tlslen4 = sizeof(tlsbuf4);
    TcpSession ssn;
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = NULL;
    p.payload_len = 0;
    p.proto = IPPROTO_TCP;

    StreamL7DataPtrInit(&ssn,StreamL7GetStorageSize());
    f.protoctx = (void *)&ssn;
    p.flow = &f;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tls any any -> any any (msg:\"TLS\"; tls.version:1.0; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf1, tlslen1, FALSE);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf2, tlslen2, FALSE);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf3, tlslen3, FALSE);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_TLS, STREAM_TOSERVER, tlsbuf4, tlslen4, FALSE);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    TlsState *tls_state = ssn.aldata[AlpGetStateIdx(ALPROTO_TLS)];
    if (tls_state == NULL) {
        printf("no tls state: ");
        result = 0;
        goto end;
    }

    if (tls_state->client_content_type != 0x16) {
        printf("expected content_type %" PRIu8 ", got %" PRIu8 ": ", 0x16, tls_state->client_content_type);
        result = 0;
        goto end;
    }

    if (tls_state->client_version != TLS_VERSION_10) {
        printf("expected version %04" PRIu16 ", got %04" PRIu16 ": ", TLS_VERSION_10, tls_state->client_version);
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    if (PacketAlertCheck(&p, 1)) {
        goto end;
    }

    result = 1;
end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for AppLayerTlsDetectVersion
 */
void AppLayerTlsDetectVersionRegisterTests(void) {
#ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("AppLayerTlsDetectVersionTestParse01", AppLayerTlsDetectVersionTestParse01, 1);
    UtRegisterTest("AppLayerTlsDetectVersionTestParse02", AppLayerTlsDetectVersionTestParse02, 1);
    UtRegisterTest("AppLayerTlsDetectVersionTestDetect01", AppLayerTlsDetectVersionTestDetect01, 1);
    UtRegisterTest("AppLayerTlsDetectVersionTestDetect02", AppLayerTlsDetectVersionTestDetect02, 1);
#endif /* UNITTESTS */
}
