/* OFFSET part of the detection engine. */

#include "suricata-common.h"

#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include "detect-content.h"
#include "detect-pcre.h"

#include "util-debug.h"

static int DetectOffsetSetup (DetectEngineCtx *, Signature *, char *);

void DetectOffsetRegister (void) {
    sigmatch_table[DETECT_OFFSET].name = "offset";
    sigmatch_table[DETECT_OFFSET].Match = NULL;
    sigmatch_table[DETECT_OFFSET].Setup = DetectOffsetSetup;
    sigmatch_table[DETECT_OFFSET].Free  = NULL;
    sigmatch_table[DETECT_OFFSET].RegisterTests = NULL;

    sigmatch_table[DETECT_OFFSET].flags |= SIGMATCH_PAYLOAD;
}

int DetectOffsetSetup (DetectEngineCtx *de_ctx, Signature *s, char *offsetstr)
{
    char *str = offsetstr;
    char dubbed = 0;

    /* strip "'s */
    if (offsetstr[0] == '\"' && offsetstr[strlen(offsetstr)-1] == '\"') {
        str = SCStrdup(offsetstr+1);
        str[strlen(offsetstr)-2] = '\0';
        dubbed = 1;
    }

    /* Search for the first previous DetectContent
     * SigMatch (it can be the same as this one) */
    SigMatch *pm = DetectContentFindPrevApplicableSM(s->pmatch_tail);
    if (pm == NULL) {
        SCLogError(SC_ERR_OFFSET_MISSING_CONTENT, "offset needs a preceeding content option");
        if (dubbed) SCFree(str);
        return -1;
    }

    DetectContentData *cd = (DetectContentData *)pm->ctx;
    if (cd == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "invalid argument");
        if (dubbed) SCFree(str);
        return -1;
    }

    cd->offset = (uint32_t)atoi(str);

    /* check if offset and depth make sense with the pattern len */
    if (cd->depth != 0) {
        if (cd->content_len + cd->offset > cd->depth) {
            SCLogDebug("depth increased to %"PRIu32" to match pattern len and offset", cd->content_len + cd->offset);
            cd->depth = cd->content_len + cd->offset;
        }
    }

    if (dubbed) SCFree(str);
    return 0;
}

