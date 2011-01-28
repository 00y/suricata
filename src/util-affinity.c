/* Copyright (C) 2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/** \file
 *
 *  \author Eric Leblond <eric@regit.org>
 *
 *  CPU affinity related code and helper.
 */

#include "suricata-common.h"
#define _THREAD_AFFINITY
#include "util-affinity.h"
#include "util-cpu.h"
#include "conf.h"
#include "threads.h"
#include "queue.h"
#include "runmodes.h"

ThreadsAffinityType thread_affinity[MAX_CPU_SET] = {
    {
        .name = "receive_cpu_set",
        .mode_flag = EXCLUSIVE_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "decode_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "stream_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "detect_cpu_set",
        .mode_flag = EXCLUSIVE_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "verdict_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "reject_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "output_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },
    {
        .name = "management_cpu_set",
        .mode_flag = BALANCED_AFFINITY,
        .prio = PRIO_MEDIUM,
        .lcpu = 0,
    },

};

/**
 * \brief find affinity by its name
 * \retval a pointer to the affinity or NULL if not found
 */
ThreadsAffinityType * GetAffinityTypeFromName(const char *name) {
    int i;
    for (i = 0; i < MAX_CPU_SET; i++) {
        if (!strcmp(thread_affinity[i].name, name)) {
            return &thread_affinity[i];
        }
    }
    return NULL;
}

static void AffinitySetupInit()
{
    int i, j;
    int ncpu = UtilCpuGetNumProcessorsConfigured();

    SCLogDebug("Initialize affinity setup\n");
    /* be conservative relatively to OS: use all cpus by default */
    for (i = 0; i < MAX_CPU_SET; i++) {
        cpu_set_t *cs = &thread_affinity[i].cpu_set;
        CPU_ZERO(cs);
        for (j = 0; j < ncpu; j++) {
            CPU_SET(j, cs);
        }
    }

    return;
}

/**
 * \brief Extract cpu affinity configuration from current config file
 */

void AffinitySetupLoadFromConfig()
{
    ConfNode *root = ConfGetNode("threading.cpu_affinity");
    ConfNode *affinity;

    AffinitySetupInit();

    SCLogDebug("Load affinity from config\n");
    if (root == NULL) {
        SCLogInfo("can't get cpu_affinity node");
        return;
    }

    TAILQ_FOREACH(affinity, &root->head, next) {
        ThreadsAffinityType *taf = GetAffinityTypeFromName(affinity->val);
        ConfNode *node = NULL;
        ConfNode *lnode;

        if (taf == NULL) {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "unknown cpu_affinity type");
            exit(EXIT_FAILURE);
        } else {
            SCLogInfo("Found affinity definition for \"%s\"",
                      affinity->val);
        }
        CPU_ZERO(&taf->cpu_set);

        node = ConfNodeLookupChild(affinity->head.tqh_first, "cpu");

        if (node == NULL) {
            SCLogInfo("unable to find 'cpu'");
        } else {
            TAILQ_FOREACH(lnode, &node->head, next) {
                int i;
                long int a,b;
                int stop = 0;
                if (!strcmp(lnode->val, "all")) {
                    a = 0;
                    b = UtilCpuGetNumProcessorsConfigured();
                    stop = 1;
                } else if (index(lnode->val, '-') != NULL) {
                    char *sep = index(lnode->val, '-');
                    char *end;
                    a = strtoul(lnode->val, &end, 10);
                    if (end != sep) {
                        SCLogError(SC_ERR_INVALID_ARGUMENT,
                                "invalid cpu range (start invalid): \"%s\"",
                                lnode->val);
                        exit(EXIT_FAILURE);
                    }
                    b = strtol(sep + 1, &end, 10);
                    if (end != sep + strlen(sep)) {
                        SCLogError(SC_ERR_INVALID_ARGUMENT,
                                "invalid cpu range (end invalid): \"%s\"",
                                lnode->val);
                        exit(EXIT_FAILURE);
                    }
                    if (a > b) {
                        SCLogError(SC_ERR_INVALID_ARGUMENT,
                                "invalid cpu range (bad order): \"%s\"",
                                lnode->val);
                        exit(EXIT_FAILURE);
                    }
                } else {
                    char *end;
                    a = strtoul(lnode->val, &end, 10);
                    if (end != lnode->val + strlen(lnode->val)) {
                        SCLogError(SC_ERR_INVALID_ARGUMENT,
                                "invalid cpu range (not an integer): \"%s\"",
                                lnode->val);
                        exit(EXIT_FAILURE);
                    }
                    b = a;
                }
                for (i = a; i<= b; i++) {
                    CPU_SET(i, &taf->cpu_set);
                }
                if (stop)
                    break;
            }
        }

        node = ConfNodeLookupChild(affinity->head.tqh_first, "mode");
        if (node != NULL) {
            if (!strcmp(node->val, "exclusive")) {
                taf->mode_flag = EXCLUSIVE_AFFINITY;
            } else if (!strcmp(node->val, "balanced")) {
                taf->mode_flag = BALANCED_AFFINITY;
            } else {
                SCLogError(SC_ERR_INVALID_ARGUMENT, "unknown cpu_affinity node");
                exit(EXIT_FAILURE);
            }
        }

        node = ConfNodeLookupChild(affinity->head.tqh_first, "prio");
        if (node == NULL)
            continue;
        if (!strcmp(node->val, "low")) {
            taf->prio = PRIO_LOW;
        } else if (!strcmp(node->val, "medium")) {
            taf->prio = PRIO_MEDIUM;
        } else if (!strcmp(node->val, "high")) {
            taf->prio = PRIO_HIGH;
        } else {
            SCLogError(SC_ERR_INVALID_ARGUMENT, "unknown cpu_affinity prio");
            exit(EXIT_FAILURE);
        }


    }
}

/**
 * \brief Return next cpu to use for a given thread family
 * \retval the cpu to used given by its id
 */
int AffinityGetNextCPU(ThreadsAffinityType *taf)
{
    int ncpu = taf->lcpu;
    while (!CPU_ISSET(ncpu, &taf->cpu_set)) {
        ncpu++;
        if (ncpu >= UtilCpuGetNumProcessorsOnline())
            ncpu = 0;
    }
    taf->lcpu = ncpu + 1;
    if (taf->lcpu >= UtilCpuGetNumProcessorsOnline())
        taf->lcpu = 0;
    return ncpu;
}
