#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "util-unittest.h"

UtTest *ut_list;

static UtTest *UtAllocTest(void) {
    UtTest *ut = malloc(sizeof(UtTest));
    if (ut == NULL) {
        printf("ERROR: UtTest *ut = malloc(sizeof(UtTest)); failed\n");
        return NULL;
    }

    memset(ut, 0, sizeof(UtTest));

    return ut;
}

static int UtAppendTest(UtTest **list, UtTest *test) {
    if (*list == NULL) {
        *list = test;
    } else {
        UtTest *tmp = *list;

        while (tmp->next != NULL) {
             tmp = tmp->next;
        }
        tmp->next = test;
    }

    return 0;
}

void UtRegisterTest(char *name, int(*testfn)(void), int evalue) {
    UtTest *ut = UtAllocTest();
    if (ut == NULL)
        return;

    ut->name = name;
    ut->testfn = testfn;
    ut->evalue = evalue;
    ut->next = NULL;

    /* append */
    UtAppendTest(&ut_list, ut);
}

int UtRunTests(void) {
    UtTest *ut;
    int result = 0;
    u_int32_t good = 0, bad = 0;

    for (ut = ut_list; ut != NULL; ut = ut->next) {
        printf("Test %-60s : ", ut->name);
        fflush(stdout); /* flush so in case of a segv we see the testname */
        int ret = ut->testfn();
        printf("%s\n", (ret == ut->evalue) ? "pass" : "FAILED");
        if (ret != ut->evalue) {
            result = 1;
            bad++;
        } else {
            good++;
        }
    }

    printf("==== TEST RESULTS ====\n");
    printf("PASSED: %u\n", good);
    printf("FAILED: %u\n", bad);
    printf("======================\n");
    return result;
}

void UtInitialize(void) {
    ut_list = NULL;
}

void UtCleanup(void) {

    UtTest *tmp = ut_list, *otmp;

    while (tmp != NULL) {
        otmp = tmp->next;
        free(tmp);
        tmp = otmp;
    }

    ut_list = NULL;
}

int UtSelftestTrue(void) {
    if (1)return 1;
    else  return 0;
}
int UtSelftestFalse(void) {
    if (0)return 1;
    else  return 0;
}

int UtRunSelftest (void) {
    printf("* Running Unittesting subsystem selftests...\n");

    UtInitialize();

    UtRegisterTest("true",  UtSelftestTrue,  1);
    UtRegisterTest("false", UtSelftestFalse, 0);

    int ret = UtRunTests();

    if (ret == 0)
        printf("* Done running Unittesting subsystem selftests...\n");
    else
        printf("* ERROR running Unittesting subsystem selftests failed...\n");

    UtCleanup();
    return 0;
}

