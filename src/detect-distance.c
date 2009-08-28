/* DISTANCE part of the detection engine. */

#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include <pcre.h>
#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-pcre.h"

int DetectDistanceSetup (DetectEngineCtx *, Signature *s, SigMatch *m, char *distancestr);

void DetectDistanceRegister (void) {
    sigmatch_table[DETECT_DISTANCE].name = "distance";
    sigmatch_table[DETECT_DISTANCE].Match = NULL;
    sigmatch_table[DETECT_DISTANCE].Setup = DetectDistanceSetup;
    sigmatch_table[DETECT_DISTANCE].Free  = NULL;
    sigmatch_table[DETECT_DISTANCE].RegisterTests = NULL;
}

int DetectDistanceSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *distancestr)
{
    char *str = distancestr;
    char dubbed = 0;

    //printf("DetectDistanceSetup: s->match:%p,m:%p,distancestr:\'%s\'\n", s->match, m, distancestr);

    /* strip "'s */
    if (distancestr[0] == '\"' && distancestr[strlen(distancestr)-1] == '\"') {
        str = strdup(distancestr+1);
        str[strlen(distancestr)-2] = '\0';
        dubbed = 1;
    }

    SigMatch *pm = m;
    if (pm == NULL) {
        printf("DetectDistanceSetup: No previous match!\n");
        goto error;
    }

    if (pm->type == DETECT_PCRE) {
        DetectPcreData *pe = (DetectPcreData *)pm->ctx;

        pe->distance = strtol(str, NULL, 10);
        pe->flags |= DETECT_PCRE_DISTANCE;

        //printf("DetectDistanceSetup: set distance %" PRId32 " for previous pcre\n", pe->distance);
    } else if (pm->type == DETECT_CONTENT) {
        DetectContentData *cd = (DetectContentData *)pm->ctx;

        cd->distance = strtol(str, NULL, 10);
        cd->flags |= DETECT_CONTENT_DISTANCE;

        //printf("DetectDistanceSetup: set distance %" PRId32 " for previous content\n", cd->distance);
    } else if (pm->type == DETECT_URICONTENT) {
        DetectUricontentData *cd = (DetectUricontentData *)pm->ctx;

        cd->distance = strtol(str, NULL, 10);
        cd->flags |= DETECT_URICONTENT_DISTANCE;

        //printf("DetectDistanceSetup: set distance %" PRId32 " for previous content\n", cd->distance);
    } else {
        printf("DetectDistanceSetup: Unknown previous keyword!\n");
        goto error;
    }

    pm = m->prev;
    if (pm == NULL) {
        printf("DetectDistanceSetup: No previous-previous match!\n");
        goto error;
    }

    if (pm->type == DETECT_PCRE) {
        DetectPcreData *pe = (DetectPcreData *)pm->ctx;
        pe->flags |= DETECT_PCRE_DISTANCE_NEXT;
    } else if (pm->type == DETECT_CONTENT) {
        DetectContentData *cd = (DetectContentData *)pm->ctx;
        cd->flags |= DETECT_CONTENT_DISTANCE_NEXT;
    } else if (pm->type == DETECT_URICONTENT) {
        DetectUricontentData *cd = (DetectUricontentData *)pm->ctx;
        cd->flags |= DETECT_URICONTENT_DISTANCE_NEXT;
    } else {
        printf("DetectDistanceSetup: Unknown previous-previous keyword!\n");
        goto error;
    }

    if (dubbed) free(str);
    return 0;
error:
    if (dubbed) free(str);
    return -1;
}

