/* NOALERT part of the detection engine. */

#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include <pcre.h>

int DetectNoalertSetup (Signature *, SigMatch *, char *);

void DetectNoalertRegister (void) {
    sigmatch_table[DETECT_NOALERT].name = "noalert";
    sigmatch_table[DETECT_NOALERT].Match = NULL;
    sigmatch_table[DETECT_NOALERT].Setup = DetectNoalertSetup;
    sigmatch_table[DETECT_NOALERT].Free  = NULL;
    sigmatch_table[DETECT_NOALERT].RegisterTests = NULL;

    sigmatch_table[DETECT_NOALERT].flags |= SIGMATCH_NOOPT;
}

int DetectNoalertSetup (Signature *s, SigMatch *m, char *nullstr)
{
    if (nullstr != NULL) {
        printf("DetectNoalertSetup: nocase has no value\n");
        return -1;
    }

    s->flags |= SIG_FLAG_NOALERT;
    return 0;
}

