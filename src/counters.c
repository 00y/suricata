/* Copyright (C) 2007-2013 Open Information Security Foundation
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

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 * \author Victor Julien <victor@inliniac.net>
 *
 * Performance counters
 */

#include "suricata-common.h"
#include "suricata.h"
#include "counters.h"
#include "threadvars.h"
#include "tm-threads.h"
#include "conf.h"
#include "util-time.h"
#include "util-unittest.h"
#include "util-debug.h"
#include "util-privs.h"
#include "util-signal.h"
#include "unix-manager.h"
#include "output.h"

/** \todo Get the default log directory from some global resource. */
#define SC_PERF_DEFAULT_LOG_FILENAME "stats.log"

/* Used to parse the interval for Timebased counters */
#define SC_PERF_PCRE_TIMEBASED_INTERVAL "^(?:(\\d+)([shm]))(?:(\\d+)([shm]))?(?:(\\d+)([shm]))?$"

/* Time interval for syncing the local counters with the global ones */
#define SC_PERF_WUT_TTS 3

/* Time interval at which the mgmt thread o/p the stats */
#define SC_PERF_MGMTT_TTS 8

/**
 * \brief Different kinds of qualifier that can be used to modify the behaviour
 *        of the Perf counter to be registered
 */
enum {
    SC_PERF_TYPE_Q_NORMAL = 1,
    SC_PERF_TYPE_Q_AVERAGE = 2,
    SC_PERF_TYPE_Q_MAXIMUM = 3,
    SC_PERF_TYPE_Q_FUNC = 4,

    SC_PERF_TYPE_Q_MAX = 5,
};

/**
 * \brief Different output interfaces made available by the Perf counter API
 */
enum {
    SC_PERF_IFACE_FILE,
    SC_PERF_IFACE_CONSOLE,
    SC_PERF_IFACE_SYSLOG,
    SC_PERF_IFACE_MAX,
};

/**
 * \brief Holds multiple instances of the same TM together, used when the stats
 *        have to be clubbed based on TM, before being sent out
 */
typedef struct SCPerfClubTMInst_ {
    char *name;

    char *tm_name;

    SCPerfPublicContext *ctx;

    SCPerfPublicContext **head;
    uint32_t size;

    struct SCPerfClubTMInst_ *next;
} SCPerfClubTMInst;

/**
 * \brief Holds the output interface context for the counter api
 */
typedef struct SCPerfOPIfaceContext_ {
    SCPerfClubTMInst *pctmi;
    SCMutex pctmi_lock;
    HashTable *counters_id_hash;

    //SCPerfCounter *global_counters;
    SCPerfPublicContext global_counter_ctx;
} SCPerfOPIfaceContext;

static void *stats_thread_data = NULL;
static SCPerfOPIfaceContext *sc_perf_op_ctx = NULL;
static time_t sc_start_time;
/** refresh interval in seconds */
static uint32_t sc_counter_tts = SC_PERF_MGMTT_TTS;
/** is the stats counter enabled? */
static char sc_counter_enabled = TRUE;

static int SCPerfOutputCounterFileIface(ThreadVars *tv);

/** stats table is filled each interval and passed to the
 *  loggers. Initialized at first use. */
static StatsTable stats_table = { NULL, 0, 0, {0 , 0}};

static uint16_t counters_global_id = 0;

/**
 * \brief The output interface dispatcher for the counter api
 */
void SCPerfOutputCounters(ThreadVars *tv)
{
    SCPerfOutputCounterFileIface(tv);
}

/**
 * \brief Adds a value of type uint64_t to the local counter.
 *
 * \param id  ID of the counter as set by the API
 * \param pca Counter array that holds the local counter for this TM
 * \param x   Value to add to this local counter
 */
void SCPerfCounterAddUI64(ThreadVars *tv, uint16_t id, uint64_t x)
{
    SCPerfPrivateContext *pca = &tv->perf_private_ctx;
#ifdef UNITTESTS
    if (pca->initialized == 0)
        return;
#endif
#ifdef DEBUG
    BUG_ON ((id < 1) || (id > pca->size));
#endif
    pca->head[id].value += x;
    pca->head[id].updates++;
    return;
}

/**
 * \brief Increments the local counter
 *
 * \param id  Index of the counter in the counter array
 * \param pca Counter array that holds the local counters for this TM
 */
void SCPerfCounterIncr(ThreadVars *tv, uint16_t id)
{
    SCPerfPrivateContext *pca = &tv->perf_private_ctx;
#ifdef UNITTESTS
    if (pca->initialized == 0)
        return;
#endif
#ifdef DEBUG
    BUG_ON ((id < 1) || (id > pca->size));
#endif
    pca->head[id].value++;
    pca->head[id].updates++;
    return;
}

/**
 * \brief Sets a value of type double to the local counter
 *
 * \param id  Index of the local counter in the counter array
 * \param pca Pointer to the SCPerfPrivateContext
 * \param x   The value to set for the counter
 */
void SCPerfCounterSetUI64(ThreadVars *tv, uint16_t id, uint64_t x)
{
    SCPerfPrivateContext *pca = &tv->perf_private_ctx;
#ifdef UNITTESTS
    if (pca->initialized == 0)
        return;
#endif
#ifdef DEBUG
    BUG_ON ((id < 1) || (id > pca->size));
#endif

    if ((pca->head[id].pc->type == SC_PERF_TYPE_Q_MAXIMUM) &&
            (x > pca->head[id].value)) {
        pca->head[id].value = x;
    } else if (pca->head[id].pc->type == SC_PERF_TYPE_Q_NORMAL) {
        pca->head[id].value = x;
    }

    pca->head[id].updates++;

    return;
}

static ConfNode *GetConfig(void) {
    ConfNode *stats = ConfGetNode("stats");
    if (stats != NULL)
        return stats;

    ConfNode *root = ConfGetNode("outputs");
    ConfNode *node = NULL;
    if (root != NULL) {
        TAILQ_FOREACH(node, &root->head, next) {
            if (strcmp(node->val, "stats") == 0) {
                return node->head.tqh_first;
            }
        }
    }
    return NULL;
}

/**
 * \brief Initializes the output interface context
 *
 * \todo Support multiple interfaces
 */
static void SCPerfInitOPCtx(void)
{
    SCEnter();
    if ( (sc_perf_op_ctx = SCMalloc(sizeof(SCPerfOPIfaceContext))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCPerfInitOPCtx. Exiting...");
        exit(EXIT_FAILURE);
    }
    memset(sc_perf_op_ctx, 0, sizeof(SCPerfOPIfaceContext));

    ConfNode *stats = GetConfig();
    if (stats != NULL) {
        const char *enabled = ConfNodeLookupChildValue(stats, "enabled");
        if (enabled != NULL && ConfValIsFalse(enabled)) {
            sc_counter_enabled = FALSE;
            SCLogDebug("Stats module has been disabled");
            SCReturn;
        }
        const char *interval = ConfNodeLookupChildValue(stats, "interval");
        if (interval != NULL)
            sc_counter_tts = (uint32_t) atoi(interval);
    }

    if (!OutputStatsLoggersRegistered()) {
        SCLogWarning(SC_WARN_NO_STATS_LOGGERS, "stats are enabled but no loggers are active");
        sc_counter_enabled = FALSE;
        SCReturn;
    }

    /* Store the engine start time */
    time(&sc_start_time);

    /* init the lock used by SCPerfClubTMInst */
    if (SCMutexInit(&sc_perf_op_ctx->pctmi_lock, NULL) != 0) {
        SCLogError(SC_ERR_INITIALIZATION, "error initializing pctmi mutex");
        exit(EXIT_FAILURE);
    }

    SCReturn;
}

/**
 * \brief Releases the resources alloted to the output context of the Perf
 *        Counter API
 */
static void SCPerfReleaseOPCtx()
{
    if (sc_perf_op_ctx == NULL) {
        SCLogDebug("Counter module has been disabled");
        return;
    }

    SCPerfClubTMInst *pctmi = NULL;
    SCPerfClubTMInst *temp = NULL;
    pctmi = sc_perf_op_ctx->pctmi;

    while (pctmi != NULL) {
        if (pctmi->tm_name != NULL)
            SCFree(pctmi->tm_name);

        if (pctmi->head != NULL)
            SCFree(pctmi->head);

        temp = pctmi->next;
        SCFree(pctmi);
        pctmi = temp;
    }

    SCFree(sc_perf_op_ctx);
    sc_perf_op_ctx = NULL;

    /* free stats table */
    if (stats_table.stats != NULL) {
        SCFree(stats_table.stats);
        memset(&stats_table, 0, sizeof(stats_table));
    }

    return;
}

/**
 * \brief The management thread. This thread is responsible for writing the
 *        performance stats information.
 *
 * \param arg is NULL always
 *
 * \retval NULL This is the value that is always returned
 */
static void *SCPerfMgmtThread(void *arg)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv_local = (ThreadVars *)arg;
    uint8_t run = 1;
    struct timespec cond_time;

    /* Set the thread name */
    if (SCSetThreadName(tv_local->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv_local->thread_setup_flags != 0)
        TmThreadSetupOptions(tv_local);

    /* Set the threads capability */
    tv_local->cap_flags = 0;

    SCDropCaps(tv_local);

    if (sc_perf_op_ctx == NULL) {
        SCLogError(SC_ERR_PERF_STATS_NOT_INIT, "Perf Counter API not init"
                   "SCPerfInitCounterApi() has to be called first");
        TmThreadsSetFlag(tv_local, THV_CLOSED | THV_RUNNING_DONE);
        return NULL;
    }

    TmModule *tm = &tmm_modules[TMM_STATSLOGGER];
    BUG_ON(tm->ThreadInit == NULL);
    int r = tm->ThreadInit(tv_local, NULL, &stats_thread_data);
    if (r != 0 || stats_thread_data == NULL) {
        SCLogError(SC_ERR_THREAD_INIT, "Perf Counter API "
                   "ThreadInit failed");
        TmThreadsSetFlag(tv_local, THV_CLOSED | THV_RUNNING_DONE);
        return NULL;
    }
    SCLogDebug("stats_thread_data %p", &stats_thread_data);

    TmThreadsSetFlag(tv_local, THV_INIT_DONE);
    while (run) {
        if (TmThreadsCheckFlag(tv_local, THV_PAUSE)) {
            TmThreadsSetFlag(tv_local, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv_local);
            TmThreadsUnsetFlag(tv_local, THV_PAUSED);
        }

        cond_time.tv_sec = time(NULL) + sc_counter_tts;
        cond_time.tv_nsec = 0;

        /* wait for the set time, or until we are woken up by
         * the shutdown procedure */
        SCCtrlMutexLock(tv_local->ctrl_mutex);
        SCCtrlCondTimedwait(tv_local->ctrl_cond, tv_local->ctrl_mutex, &cond_time);
        SCCtrlMutexUnlock(tv_local->ctrl_mutex);

        SCPerfOutputCounters(tv_local);

        if (TmThreadsCheckFlag(tv_local, THV_KILL)) {
            run = 0;
        }
    }

    TmThreadsSetFlag(tv_local, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv_local, THV_DEINIT);

    r = tm->ThreadDeinit(tv_local, stats_thread_data);
    if (r != TM_ECODE_OK) {
        SCLogError(SC_ERR_THREAD_DEINIT, "Perf Counter API "
                   "ThreadDeinit failed");
    }

    TmThreadsSetFlag(tv_local, THV_CLOSED);
    return NULL;
}

/**
 * \brief Wake up thread.  This thread wakes up every TTS(time to sleep) seconds
 *        and sets the flag for every ThreadVars' SCPerfPublicContext
 *
 * \param arg is NULL always
 *
 * \retval NULL This is the value that is always returned
 */
static void *SCPerfWakeupThread(void *arg)
{
    /* block usr2.  usr2 to be handled by the main thread only */
    UtilSignalBlock(SIGUSR2);

    ThreadVars *tv_local = (ThreadVars *)arg;
    uint8_t run = 1;
    ThreadVars *tv = NULL;
    PacketQueue *q = NULL;
    struct timespec cond_time;

    /* Set the thread name */
    if (SCSetThreadName(tv_local->name) < 0) {
        SCLogWarning(SC_ERR_THREAD_INIT, "Unable to set thread name");
    }

    if (tv_local->thread_setup_flags != 0)
        TmThreadSetupOptions(tv_local);

    /* Set the threads capability */
    tv_local->cap_flags = 0;

    SCDropCaps(tv_local);

    if (sc_perf_op_ctx == NULL) {
        SCLogError(SC_ERR_PERF_STATS_NOT_INIT, "Perf Counter API not init"
                   "SCPerfInitCounterApi() has to be called first");
        TmThreadsSetFlag(tv_local, THV_CLOSED | THV_RUNNING_DONE);
        return NULL;
    }

    TmThreadsSetFlag(tv_local, THV_INIT_DONE);
    while (run) {
        if (TmThreadsCheckFlag(tv_local, THV_PAUSE)) {
            TmThreadsSetFlag(tv_local, THV_PAUSED);
            TmThreadTestThreadUnPaused(tv_local);
            TmThreadsUnsetFlag(tv_local, THV_PAUSED);
        }

        cond_time.tv_sec = time(NULL) + SC_PERF_WUT_TTS;
        cond_time.tv_nsec = 0;

        /* wait for the set time, or until we are woken up by
         * the shutdown procedure */
        SCCtrlMutexLock(tv_local->ctrl_mutex);
        SCCtrlCondTimedwait(tv_local->ctrl_cond, tv_local->ctrl_mutex, &cond_time);
        SCCtrlMutexUnlock(tv_local->ctrl_mutex);

        tv = tv_root[TVT_PPT];
        while (tv != NULL) {
            if (tv->perf_public_ctx.head == NULL) {
                tv = tv->next;
                continue;
            }

            /* assuming the assignment of an int to be atomic, and even if it's
             * not, it should be okay */
            tv->perf_public_ctx.perf_flag = 1;

            if (tv->inq != NULL) {
                q = &trans_q[tv->inq->id];
                SCCondSignal(&q->cond_q);
            }

            tv = tv->next;
        }

        /* mgt threads for flow manager */
        tv = tv_root[TVT_MGMT];
        while (tv != NULL) {
            if (tv->perf_public_ctx.head == NULL) {
                tv = tv->next;
                continue;
            }

            /* assuming the assignment of an int to be atomic, and even if it's
             * not, it should be okay */
            tv->perf_public_ctx.perf_flag = 1;

            tv = tv->next;
        }

        if (TmThreadsCheckFlag(tv_local, THV_KILL)) {
            run = 0;
        }
    }

    TmThreadsSetFlag(tv_local, THV_RUNNING_DONE);
    TmThreadWaitForFlag(tv_local, THV_DEINIT);

    TmThreadsSetFlag(tv_local, THV_CLOSED);
    return NULL;
}

/**
 * \brief Releases a perf counter.  Used internally by
 *        SCPerfReleasePerfCounterS()
 *
 * \param pc Pointer to the SCPerfCounter to be freed
 */
static void SCPerfReleaseCounter(SCPerfCounter *pc)
{
    if (pc != NULL) {
        if (pc->cname != NULL)
            SCFree(pc->cname);

        if (pc->tm_name != NULL)
            SCFree(pc->tm_name);

        SCFree(pc);
    }

    return;
}

/**
 * \brief Registers a counter.  Used internally by the Perf Counter API
 *
 * \param cname    Name of the counter, to be registered
 * \param tm_name  Thread module to which this counter belongs
 * \param type     Datatype of this counter variable
 * \param pctx     SCPerfPublicContext for this tm-tv instance
 * \param type_q   Qualifier describing the type of counter to be registered
 *
 * \retval the counter id for the newly registered counter, or the already
 *         present counter on success
 * \retval 0 on failure
 */
static uint16_t SCPerfRegisterQualifiedCounter(char *cname, char *tm_name,
                                               int type, SCPerfPublicContext *pctx,
                                               int type_q, uint64_t (*Func)(void))
{
    SCPerfCounter **head = &pctx->head;
    SCPerfCounter *temp = NULL;
    SCPerfCounter *prev = NULL;
    SCPerfCounter *pc = NULL;

    if (cname == NULL || tm_name == NULL || pctx == NULL) {
        SCLogDebug("Counter name, tm name null or SCPerfPublicContext NULL");
        return 0;
    }

    if ((type >= SC_PERF_TYPE_MAX) || (type < 0)) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Counters of type %" PRId32 " can't "
                   "be registered", type);
        return 0;
    }

    temp = prev = *head;
    while (temp != NULL) {
        prev = temp;

        if (strcmp(cname, temp->cname) == 0 &&
            strcmp(tm_name, temp->tm_name) == 0) {
            break;
        }

        temp = temp->next;
    }

    /* We already have a counter registered by this name */
    if (temp != NULL)
        return(temp->id);

    /* if we reach this point we don't have a counter registered by this cname */
    if ( (pc = SCMalloc(sizeof(SCPerfCounter))) == NULL)
        return 0;
    memset(pc, 0, sizeof(SCPerfCounter));

    if ( (pc->cname = SCStrdup(cname)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    if ( (pc->tm_name = SCStrdup(tm_name)) == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "Error allocating memory");
        exit(EXIT_FAILURE);
    }

    /* assign a unique id to this SCPerfCounter.  The id is local to this
     * PerfContext.  Please note that the id start from 1, and not 0 */
    pc->id = ++(pctx->curr_id);

    pc->type = type_q;

    /* we now add the counter to the list */
    if (prev == NULL)
        *head = pc;
    else
        prev->next = pc;

    return pc->id;
}

/**
 * \brief Copies the SCPerfCounter value from the local counter present in the
 *        SCPerfPrivateContext to its corresponding global counterpart.  Used
 *        internally by SCPerfUpdateCounterArray()
 *
 * \param pcae     Pointer to the SCPerfPrivateContext which holds the local
 *                 versions of the counters
 */
static void SCPerfCopyCounterValue(SCPCAElem *pcae)
{
    SCPerfCounter *pc = pcae->pc;

    pc->value = pcae->value;
    pc->updates = pcae->updates;
    return;
}

/**
 * \brief Calculates counter value that should be sent as output
 *
 *        If we aren't dealing with timebased counters, we just return the
 *        the counter value.  In case of Timebased counters, if we haven't
 *        crossed the interval, we display the current value without any
 *        modifications.  If we have crossed the limit, we calculate the counter
 *        value for the time period and also return 1, to indicate that the
 *        counter value can be reset after use
 *
 * \param pc Pointer to the PerfCounter for which the timebased counter has to
 *           be calculated
 */
static uint64_t SCPerfOutputCalculateCounterValue(SCPerfCounter *pc)
{
    return pc->value;
}

/**
 * \brief The file output interface for the Perf Counter api
 */
static int SCPerfOutputCounterFileIface(ThreadVars *tv)
{
    const SCPerfClubTMInst *pctmi = NULL;
    const SCPerfCounter *pc = NULL;
    void *td = stats_thread_data;

    if (stats_table.nstats == 0) {
        uint32_t nstats = counters_global_id;

        stats_table.nstats = nstats;
        stats_table.stats = SCCalloc(stats_table.nstats, sizeof(StatsRecord));
        if (stats_table.stats == NULL) {
            stats_table.nstats = 0;
            SCLogError(SC_ERR_MEM_ALLOC, "could not alloc memory for stats");
            return -1;
        }

        stats_table.start_time = sc_start_time;
    }

    /** temporary local table to merge the per thread counters,
     *  especially needed for the average counters */
    struct CountersMergeTable {
        int type;
        uint64_t value;
        uint64_t updates;
    } merge_table[counters_global_id];
    memset(&merge_table, 0x00,
           counters_global_id * sizeof(struct CountersMergeTable));

    StatsRecord *table = stats_table.stats;
    pctmi = sc_perf_op_ctx->pctmi;
    SCLogDebug("pctmi %p", pctmi);
    while (pctmi != NULL) {
        SCLogDebug("Thread %s (%s) ctx %p", pctmi->name,
                pctmi->tm_name ? pctmi->tm_name : "none", pctmi->ctx);

        SCMutexLock(&pctmi->ctx->m);
        pc = pctmi->ctx->head;
        while (pc != NULL) {
            SCLogDebug("Counter %s (%u:%u) value %"PRIu64,
                    pc->cname, pc->id, pc->gid, pc->value);

            merge_table[pc->gid].type = pc->type;
            switch (pc->type) {
                case SC_PERF_TYPE_Q_MAXIMUM:
                    if (pc->value > merge_table[pc->gid].value)
                        merge_table[pc->gid].value = pc->value;
                    break;
                default:
                    merge_table[pc->gid].value += pc->value;
                    break;
            }
            merge_table[pc->gid].updates += pc->updates;

            table[pc->gid].name = pc->cname;
            table[pc->gid].tm_name = pctmi->tm_name;

            pc = pc->next;
        }
        SCMutexUnlock(&pctmi->ctx->m);
        pctmi = pctmi->next;
    }

    uint16_t x;
    for (x = 0; x < counters_global_id; x++) {
        /* xfer previous value to pvalue and reset value */
        table[x].pvalue = table[x].value;
        table[x].value = 0;

        struct CountersMergeTable *m = &merge_table[x];
        switch (m->type) {
            case SC_PERF_TYPE_Q_MAXIMUM:
                if (m->value > table[x].value)
                    table[x].value = m->value;
                break;
            case SC_PERF_TYPE_Q_AVERAGE:
                if (m->value > 0 && m->updates > 0) {
                    table[x].value = (uint64_t)(m->value / m->updates);
                }
                break;
            default:
                table[x].value += m->value;
                break;
        }
    }

    /* invoke logger(s) */
    OutputStatsLog(tv, td, &stats_table);
    return 1;
}

#ifdef BUILD_UNIX_SOCKET
/**
 * \brief The file output interface for the Perf Counter api
 */
TmEcode SCPerfOutputCounterSocket(json_t *cmd,
                               json_t *answer, void *data)
{
    SCPerfClubTMInst *pctmi = NULL;
    SCPerfCounter *pc = NULL;
    SCPerfCounter **pc_heads = NULL;

    uint64_t ui64_temp = 0;
    uint64_t ui64_result = 0;

    uint32_t u = 0;
    int flag = 0;

    if (sc_perf_op_ctx == NULL) {
        json_object_set_new(answer, "message",
                json_string("No performance counter context"));
        return TM_ECODE_FAILED;
    }

    json_t *tm_array;

    tm_array = json_object();
    if (tm_array == NULL) {
        json_object_set_new(answer, "message",
                json_string("internal error at json object creation"));
        return TM_ECODE_FAILED;
    }

    pctmi = sc_perf_op_ctx->pctmi;
    while (pctmi != NULL) {
        json_t *jdata;
        int filled = 0;
        jdata = json_object();
        if (jdata == NULL) {
            json_decref(tm_array);
            json_object_set_new(answer, "message",
                    json_string("internal error at json object creation"));
            return TM_ECODE_FAILED;
        }
        if ((pc_heads = SCMalloc(pctmi->size * sizeof(SCPerfCounter *))) == NULL) {
            json_decref(tm_array);
            json_object_set_new(answer, "message",
                    json_string("internal memory error"));
            return TM_ECODE_FAILED;
        }
        memset(pc_heads, 0, pctmi->size * sizeof(SCPerfCounter *));

        for (u = 0; u < pctmi->size; u++) {
            pc_heads[u] = pctmi->head[u]->head;

            SCMutexLock(&pctmi->head[u]->m);
        }

        flag = 1;
        while(flag) {
            ui64_result = 0;
            if (pc_heads[0] == NULL)
                break;
            pc = pc_heads[0];

            for (u = 0; u < pctmi->size; u++) {
                ui64_temp = SCPerfOutputCalculateCounterValue(pc_heads[u]);
                ui64_result += ui64_temp;

                if (pc_heads[u] != NULL)
                    pc_heads[u] = pc_heads[u]->next;
                if (pc_heads[u] == NULL)
                    flag = 0;
            }

            filled = 1;
            json_object_set_new(jdata, pc->cname, json_integer(ui64_result));
        }

        for (u = 0; u < pctmi->size; u++)
            SCMutexUnlock(&pctmi->head[u]->m);

        if (filled == 1) {
            json_object_set_new(tm_array, pctmi->tm_name, jdata);
        }
        pctmi = pctmi->next;

        SCFree(pc_heads);

    }

    json_object_set_new(answer, "message", tm_array);

    return TM_ECODE_OK;
}

#endif /* BUILD_UNIX_SOCKET */

/**
 * \brief Initializes the perf counter api.  Things are hard coded currently.
 *        More work to be done when we implement multiple interfaces
 */
void SCPerfInitCounterApi(void)
{
    SCPerfInitOPCtx();

    return;
}

/**
 * \brief Spawns the wakeup, and the management thread used by the perf
 *        counter api
 *
 *  The threads use the condition variable in the thread vars to control
 *  their wait loops to make sure the main thread can quickly kill them.
 */
void SCPerfSpawnThreads(void)
{
    SCEnter();

    if (!sc_counter_enabled) {
        SCReturn;
    }

    ThreadVars *tv_wakeup = NULL;
    ThreadVars *tv_mgmt = NULL;

    /* spawn the stats wakeup thread */
    tv_wakeup = TmThreadCreateMgmtThread("SCPerfWakeupThread",
                                         SCPerfWakeupThread, 1);
    if (tv_wakeup == NULL) {
        SCLogError(SC_ERR_THREAD_CREATE, "TmThreadCreateMgmtThread "
                   "failed");
        exit(EXIT_FAILURE);
    }

    if (TmThreadSpawn(tv_wakeup) != 0) {
        SCLogError(SC_ERR_THREAD_SPAWN, "TmThreadSpawn failed for "
                   "SCPerfWakeupThread");
        exit(EXIT_FAILURE);
    }

    /* spawn the stats mgmt thread */
    tv_mgmt = TmThreadCreateMgmtThread("SCPerfMgmtThread",
                                       SCPerfMgmtThread, 1);
    if (tv_mgmt == NULL) {
        SCLogError(SC_ERR_THREAD_CREATE,
                   "TmThreadCreateMgmtThread failed");
        exit(EXIT_FAILURE);
    }

    if (TmThreadSpawn(tv_mgmt) != 0) {
        SCLogError(SC_ERR_THREAD_SPAWN, "TmThreadSpawn failed for "
                   "SCPerfWakeupThread");
        exit(EXIT_FAILURE);
    }

    SCReturn;
}

/**
 * \brief Registers a normal, unqualified counter
 *
 * \param cname Name of the counter, to be registered
 * \param tv    Pointer to the ThreadVars instance for which the counter would
 *              be registered
 * \param type  Datatype of this counter variable
 *
 * \retval id Counter id for the newly registered counter, or the already
 *            present counter
 */
uint16_t SCPerfTVRegisterCounter(char *cname, struct ThreadVars_ *tv, int type)
{
    uint16_t id = SCPerfRegisterQualifiedCounter(cname,
                                                 (tv->thread_group_name != NULL) ? tv->thread_group_name : tv->name,
                                                 type,
                                                 &tv->perf_public_ctx,
                                                 SC_PERF_TYPE_Q_NORMAL, NULL);

    return id;
}

/**
 * \brief Registers a counter, whose value holds the average of all the values
 *        assigned to it.
 *
 * \param cname Name of the counter, to be registered
 * \param tv    Pointer to the ThreadVars instance for which the counter would
 *              be registered
 * \param type  Datatype of this counter variable
 *
 * \retval id Counter id for the newly registered counter, or the already
 *            present counter
 */
uint16_t SCPerfTVRegisterAvgCounter(char *cname, struct ThreadVars_ *tv,
                                    int type)
{
    uint16_t id = SCPerfRegisterQualifiedCounter(cname,
                                                 (tv->thread_group_name != NULL) ? tv->thread_group_name : tv->name,
                                                 type,
                                                 &tv->perf_public_ctx,
                                                 SC_PERF_TYPE_Q_AVERAGE, NULL);

    return id;
}

/**
 * \brief Registers a counter, whose value holds the maximum of all the values
 *        assigned to it.
 *
 * \param cname Name of the counter, to be registered
 * \param tv    Pointer to the ThreadVars instance for which the counter would
 *              be registered
 * \param type  Datatype of this counter variable
 *
 * \retval the counter id for the newly registered counter, or the already
 *         present counter
 */
uint16_t SCPerfTVRegisterMaxCounter(char *cname, struct ThreadVars_ *tv,
                                    int type)
{
    uint16_t id = SCPerfRegisterQualifiedCounter(cname,
                                                 (tv->thread_group_name != NULL) ? tv->thread_group_name : tv->name,
                                                 type,
                                                 &tv->perf_public_ctx,
                                                 SC_PERF_TYPE_Q_MAXIMUM, NULL);

    return id;
}

/**
 * \brief Registers a counter, which represents a global value
 *
 * \param cname Name of the counter, to be registered
 * \param Func  Function Pointer returning a uint64_t
 *
 * \retval id Counter id for the newly registered counter, or the already
 *            present counter
 */
uint16_t SCPerfTVRegisterGlobalCounter(char *cname, uint64_t (*Func)(void))
{
    uint16_t id = SCPerfRegisterQualifiedCounter(cname, NULL,
                                                 SC_PERF_TYPE_UINT64,
                                                 &(sc_perf_op_ctx->global_counter_ctx),
                                                 SC_PERF_TYPE_Q_FUNC,
                                                 Func);
    return id;
}

/**
 * \brief Registers a normal, unqualified counter
 *
 * \param cname   Name of the counter, to be registered
 * \param tm_name Name of the engine module under which the counter has to be
 *                registered
 * \param type    Datatype of this counter variable
 * \param pctx    SCPerfPublicContext corresponding to the tm_name key under which the
 *                key has to be registered
 *
 * \retval id Counter id for the newly registered counter, or the already
 *            present counter
 */
static uint16_t SCPerfRegisterCounter(char *cname, char *tm_name, int type,
                               SCPerfPublicContext *pctx)
{
    uint16_t id = SCPerfRegisterQualifiedCounter(cname, tm_name, type, pctx,
                                                 SC_PERF_TYPE_Q_NORMAL, NULL);

    return id;
}

typedef struct CountersIdType_ {
    uint16_t id;
    const char *string;
} CountersIdType;

uint32_t CountersIdHashFunc(HashTable *ht, void *data, uint16_t datalen)
{
    CountersIdType *t = (CountersIdType *)data;
    uint32_t hash = 0;
    int i = 0;

    int len = strlen(t->string);

    for (i = 0; i < len; i++)
        hash += tolower((unsigned char)t->string[i]);

    hash = hash % ht->array_size;

    return hash;
}

char CountersIdHashCompareFunc(void *data1, uint16_t datalen1,
                               void *data2, uint16_t datalen2)
{
    CountersIdType *t1 = (CountersIdType *)data1;
    CountersIdType *t2 = (CountersIdType *)data2;
    int len1 = 0;
    int len2 = 0;

    if (t1 == NULL || t2 == NULL)
        return 0;

    if (t1->string == NULL || t2->string == NULL)
        return 0;

    len1 = strlen(t1->string);
    len2 = strlen(t2->string);

    if (len1 == len2 && memcmp(t1->string, t2->string, len1) == 0) {
        return 1;
    }

    return 0;
}

void CountersIdHashFreeFunc(void *data)
{
    SCFree(data);
}


/** \internal
 *  \brief Adds a TM to the clubbed TM table.  Multiple instances of the same TM
 *         are stacked together in a PCTMI container.
 *
 *  \param tm_name Name of the tm to be added to the table
 *  \param pctx    SCPerfPublicContext associated with the TM tm_name
 *
 *  \retval 1 on success, 0 on failure
 */
static int SCPerfAddToClubbedTMTable(ThreadVars *tv, SCPerfPublicContext *pctx)
{
    if (sc_perf_op_ctx == NULL) {
        SCLogDebug("Counter module has been disabled");
        return 0;
    }

    SCPerfClubTMInst *temp = NULL;

    if (tv == NULL || pctx == NULL) {
        SCLogDebug("supplied argument(s) to SCPerfAddToClubbedTMTable NULL");
        return 0;
    }

    SCMutexLock(&sc_perf_op_ctx->pctmi_lock);
    if (sc_perf_op_ctx->counters_id_hash == NULL) {
        sc_perf_op_ctx->counters_id_hash = HashTableInit(256, CountersIdHashFunc,
                                                              CountersIdHashCompareFunc,
                                                              CountersIdHashFreeFunc);
        BUG_ON(sc_perf_op_ctx->counters_id_hash == NULL);
    }
    SCPerfCounter *pc = pctx->head;
    while (pc != NULL) {
        CountersIdType t = { 0, pc->cname }, *id = NULL;
        id = HashTableLookup(sc_perf_op_ctx->counters_id_hash, &t, sizeof(t));
        if (id == NULL) {
            id = SCCalloc(1, sizeof(*id));
            BUG_ON(id == NULL);
            id->id = counters_global_id++;
            id->string = pc->cname;
            BUG_ON(HashTableAdd(sc_perf_op_ctx->counters_id_hash, id, sizeof(*id)) < 0);
        }
        pc->gid = id->id;
        pc = pc->next;
    }


    if ( (temp = SCMalloc(sizeof(SCPerfClubTMInst))) == NULL) {
        SCMutexUnlock(&sc_perf_op_ctx->pctmi_lock);
        return 0;
    }
    memset(temp, 0, sizeof(SCPerfClubTMInst));

    temp->ctx = pctx;

    temp->name = SCStrdup(tv->name);
    if (unlikely(temp->name == NULL)) {
        SCFree(temp);
        SCMutexUnlock(&sc_perf_op_ctx->pctmi_lock);
        return 0;
    }

    if (tv->thread_group_name != NULL) {
        temp->tm_name = SCStrdup(tv->thread_group_name);
        if (unlikely(temp->tm_name == NULL)) {
            SCFree(temp->name);
            SCFree(temp);
            SCMutexUnlock(&sc_perf_op_ctx->pctmi_lock);
            return 0;
        }
    }

    temp->next = sc_perf_op_ctx->pctmi;
    sc_perf_op_ctx->pctmi = temp;
    SCLogInfo("sc_perf_op_ctx->pctmi %p", sc_perf_op_ctx->pctmi);

    SCMutexUnlock(&sc_perf_op_ctx->pctmi_lock);
    return 1;
}

/** \internal
 *  \brief Returns a counter array for counters in this id range(s_id - e_id)
 *
 *  \param s_id Counter id of the first counter to be added to the array
 *  \param e_id Counter id of the last counter to be added to the array
 *  \param pctx Pointer to the tv's SCPerfPublicContext
 *
 *  \retval a counter-array in this(s_id-e_id) range for this TM instance
 */
static int SCPerfGetCounterArrayRange(uint16_t s_id, uint16_t e_id,
                                      SCPerfPublicContext *pctx,
                                      SCPerfPrivateContext *pca)
{
    SCPerfCounter *pc = NULL;
    uint32_t i = 0;

    if (pctx == NULL || pca == NULL) {
        SCLogDebug("pctx/pca is NULL");
        return -1;
    }

    if (s_id < 1 || e_id < 1 || s_id > e_id) {
        SCLogDebug("error with the counter ids");
        return -1;
    }

    if (e_id > pctx->curr_id) {
        SCLogDebug("end id is greater than the max id for this tv");
        return -1;
    }

    if ( (pca->head = SCMalloc(sizeof(SCPCAElem) * (e_id - s_id  + 2))) == NULL) {
        return -1;
    }
    memset(pca->head, 0, sizeof(SCPCAElem) * (e_id - s_id  + 2));

    pc = pctx->head;
    while (pc->id != s_id)
        pc = pc->next;

    i = 1;
    while ((pc != NULL) && (pc->id <= e_id)) {
        pca->head[i].pc = pc;
        pca->head[i].id = pc->id;
        pc = pc->next;
        i++;
    }
    pca->size = i - 1;

    pca->initialized = 1;
    return 0;
}

/** \internal
 *  \brief Returns a counter array for all counters registered for this tm
 *         instance
 *
 *  \param pctx Pointer to the tv's SCPerfPublicContext
 *
 *  \retval pca Pointer to a counter-array for all counter of this tm instance
 *              on success; NULL on failure
 */
static int SCPerfGetAllCountersArray(SCPerfPublicContext *pctx, SCPerfPrivateContext *private)
{
    if (pctx == NULL || private == NULL)
        return -1;

    return SCPerfGetCounterArrayRange(1, pctx->curr_id, pctx, private);
}


int SCPerfSetupPrivate(ThreadVars *tv)
{
    SCPerfGetAllCountersArray(&(tv)->perf_public_ctx, &(tv)->perf_private_ctx);

    SCPerfAddToClubbedTMTable(tv, &(tv)->perf_public_ctx);
    return 0;
}

/**
 * \brief Syncs the counter array with the global counter variables
 *
 * \param pca      Pointer to the SCPerfPrivateContext
 * \param pctx     Pointer the the tv's SCPerfPublicContext
 *
 * \retval  0 on success
 * \retval -1 on error
 */
int SCPerfUpdateCounterArray(SCPerfPrivateContext *pca, SCPerfPublicContext *pctx)
{
    SCPerfCounter *pc = NULL;
    SCPCAElem *pcae = NULL;
    uint32_t i = 0;

    if (pca == NULL || pctx == NULL) {
        SCLogDebug("pca or pctx is NULL inside SCPerfUpdateCounterArray");
        return -1;
    }

    pcae = pca->head;

    SCMutexLock(&pctx->m);
    pc = pctx->head;

    for (i = 1; i <= pca->size; i++) {
        while (pc != NULL) {
            if (pc->id != pcae[i].id) {
                pc = pc->next;
                continue;
            }

            SCPerfCopyCounterValue(&pcae[i]);

            pc = pc->next;
            break;
        }
    }

    SCMutexUnlock(&pctx->m);

    pctx->perf_flag = 0;

    return 1;
}

/**
 * \brief Get the value of the local copy of the counter that hold this id.
 *
 * \param tv threadvars
 * \param id The counter id.
 *
 * \retval  0 on success.
 * \retval -1 on error.
 */
uint64_t SCPerfGetLocalCounterValue(ThreadVars *tv, uint16_t id)
{
    SCPerfPrivateContext *pca = &tv->perf_private_ctx;
#ifdef DEBUG
    BUG_ON ((id < 1) || (id > pca->size));
#endif
    return pca->head[id].value;
}

/**
 * \brief Releases the resources alloted by the Perf Counter API
 */
void SCPerfReleaseResources()
{
    SCPerfReleaseOPCtx();

    return;
}

/**
 * \brief Releases a list of perf counters
 *
 * \param head Pointer to the head of the list of perf counters that have to
 *             be freed
 */
void SCPerfReleasePerfCounterS(SCPerfCounter *head)
{
    SCPerfCounter *pc = NULL;

    while (head != NULL) {
        pc = head;
        head = head->next;
        SCPerfReleaseCounter(pc);
    }

    return;
}

/**
 * \brief Releases the SCPerfPrivateContext allocated by the user, for storing and
 *        updating local counter values
 *
 * \param pca Pointer to the SCPerfPrivateContext
 */
void SCPerfReleasePCA(SCPerfPrivateContext *pca)
{
    if (pca != NULL) {
        if (pca->head != NULL) {
            SCFree(pca->head);
            pca->head = NULL;
        }
        pca->initialized = 0;
    }

    return;
}

/*----------------------------------Unit_Tests--------------------------------*/

#ifdef UNITTESTS
static int SCPerfTestCounterReg01()
{
    SCPerfPublicContext pctx;

    memset(&pctx, 0, sizeof(SCPerfPublicContext));

    return SCPerfRegisterCounter("t1", "c1", 5, &pctx);
}

static int SCPerfTestCounterReg02()
{
    SCPerfPublicContext pctx;

    memset(&pctx, 0, sizeof(SCPerfPublicContext));

    return SCPerfRegisterCounter(NULL, NULL, SC_PERF_TYPE_UINT64, &pctx);
}

static int SCPerfTestCounterReg03()
{
    SCPerfPublicContext pctx;
    int result;

    memset(&pctx, 0, sizeof(SCPerfPublicContext));

    result = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64, &pctx);

    SCPerfReleasePerfCounterS(pctx.head);

    return result;
}

static int SCPerfTestCounterReg04()
{
    SCPerfPublicContext pctx;
    int result;

    memset(&pctx, 0, sizeof(SCPerfPublicContext));

    SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64, &pctx);
    SCPerfRegisterCounter("t2", "c2", SC_PERF_TYPE_UINT64, &pctx);
    SCPerfRegisterCounter("t3", "c3", SC_PERF_TYPE_UINT64, &pctx);

    result = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64, &pctx);

    SCPerfReleasePerfCounterS(pctx.head);

    return result;
}

static int SCPerfTestGetCntArray05()
{
    ThreadVars tv;
    int id;

    memset(&tv, 0, sizeof(ThreadVars));

    id = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                               &tv.perf_public_ctx);
    if (id != 1) {
        printf("id %d: ", id);
        return 0;
    }

    int r = SCPerfGetAllCountersArray(NULL, &tv.perf_private_ctx);
    return (r == -1) ? 1 : 0;
}

static int SCPerfTestGetCntArray06()
{
    ThreadVars tv;
    int id;
    int result;

    memset(&tv, 0, sizeof(ThreadVars));

    id = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                               &tv.perf_public_ctx);
    if (id != 1)
        return 0;

    int r = SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);

    result = (r == 0) ? 1  : 0;

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(&tv.perf_private_ctx);

    return result;
}

static int SCPerfTestCntArraySize07()
{
    ThreadVars tv;
    SCPerfPrivateContext *pca = NULL;
    int result;

    memset(&tv, 0, sizeof(ThreadVars));

    //pca = (SCPerfPrivateContext *)&tv.perf_private_ctx;

    SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                          &tv.perf_public_ctx);
    SCPerfRegisterCounter("t2", "c2", SC_PERF_TYPE_UINT64,
                          &tv.perf_public_ctx);

    SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);
    pca = &tv.perf_private_ctx;

    SCPerfCounterIncr(&tv, 1);
    SCPerfCounterIncr(&tv, 2);

    result = pca->size;

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(pca);

    return result;
}

static int SCPerfTestUpdateCounter08()
{
    ThreadVars tv;
    SCPerfPrivateContext *pca = NULL;
    int id;
    int result;

    memset(&tv, 0, sizeof(ThreadVars));

    id = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                               &tv.perf_public_ctx);

    SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);
    pca = &tv.perf_private_ctx;

    SCPerfCounterIncr(&tv, id);
    SCPerfCounterAddUI64(&tv, id, 100);

    result = pca->head[id].value;

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(pca);

    return result;
}

static int SCPerfTestUpdateCounter09()
{
    ThreadVars tv;
    SCPerfPrivateContext *pca = NULL;
    uint16_t id1, id2;
    int result;

    memset(&tv, 0, sizeof(ThreadVars));

    id1 = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    SCPerfRegisterCounter("t2", "c2", SC_PERF_TYPE_UINT64,
                          &tv.perf_public_ctx);
    SCPerfRegisterCounter("t3", "c3", SC_PERF_TYPE_UINT64,
                          &tv.perf_public_ctx);
    SCPerfRegisterCounter("t4", "c4", SC_PERF_TYPE_UINT64,
                          &tv.perf_public_ctx);
    id2 = SCPerfRegisterCounter("t5", "c5", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);

    SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);
    pca = &tv.perf_private_ctx;

    SCPerfCounterIncr(&tv, id2);
    SCPerfCounterAddUI64(&tv, id2, 100);

    result = (pca->head[id1].value == 0) && (pca->head[id2].value == 101);

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(pca);

    return result;
}

static int SCPerfTestUpdateGlobalCounter10()
{
    ThreadVars tv;
    SCPerfPrivateContext *pca = NULL;

    int result = 1;
    uint16_t id1, id2, id3;

    memset(&tv, 0, sizeof(ThreadVars));

    id1 = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    id2 = SCPerfRegisterCounter("t2", "c2", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    id3 = SCPerfRegisterCounter("t3", "c3", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);

    SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);
    pca = &tv.perf_private_ctx;

    SCPerfCounterIncr(&tv, id1);
    SCPerfCounterAddUI64(&tv, id2, 100);
    SCPerfCounterIncr(&tv, id3);
    SCPerfCounterAddUI64(&tv, id3, 100);

    SCPerfUpdateCounterArray(pca, &tv.perf_public_ctx);

    result = (1 == tv.perf_public_ctx.head->value);
    result &= (100 == tv.perf_public_ctx.head->next->value);
    result &= (101 == tv.perf_public_ctx.head->next->next->value);

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(pca);

    return result;
}

static int SCPerfTestCounterValues11()
{
    ThreadVars tv;
    SCPerfPrivateContext *pca = NULL;

    int result = 1;
    uint16_t id1, id2, id3, id4;

    memset(&tv, 0, sizeof(ThreadVars));

    id1 = SCPerfRegisterCounter("t1", "c1", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    id2 = SCPerfRegisterCounter("t2", "c2", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    id3 = SCPerfRegisterCounter("t3", "c3", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);
    id4 = SCPerfRegisterCounter("t4", "c4", SC_PERF_TYPE_UINT64,
                                &tv.perf_public_ctx);

    SCPerfGetAllCountersArray(&tv.perf_public_ctx, &tv.perf_private_ctx);
    pca = &tv.perf_private_ctx;

    SCPerfCounterIncr(&tv, id1);
    SCPerfCounterAddUI64(&tv, id2, 256);
    SCPerfCounterAddUI64(&tv, id3, 257);
    SCPerfCounterAddUI64(&tv, id4, 16843024);

    SCPerfUpdateCounterArray(pca, &tv.perf_public_ctx);

    result &= (1 == tv.perf_public_ctx.head->value);

    result &= (256 == tv.perf_public_ctx.head->next->value);

    result &= (257 == tv.perf_public_ctx.head->next->next->value);

    result &= (16843024 == tv.perf_public_ctx.head->next->next->next->value);

    SCPerfReleasePerfCounterS(tv.perf_public_ctx.head);
    SCPerfReleasePCA(pca);

    return result;
}

#endif

void SCPerfRegisterTests()
{
#ifdef UNITTESTS
    UtRegisterTest("SCPerfTestCounterReg01", SCPerfTestCounterReg01, 0);
    UtRegisterTest("SCPerfTestCounterReg02", SCPerfTestCounterReg02, 0);
    UtRegisterTest("SCPerfTestCounterReg03", SCPerfTestCounterReg03, 1);
    UtRegisterTest("SCPerfTestCounterReg04", SCPerfTestCounterReg04, 1);
    UtRegisterTest("SCPerfTestGetCntArray05", SCPerfTestGetCntArray05, 1);
    UtRegisterTest("SCPerfTestGetCntArray06", SCPerfTestGetCntArray06, 1);
    UtRegisterTest("SCPerfTestCntArraySize07", SCPerfTestCntArraySize07, 2);
    UtRegisterTest("SCPerfTestUpdateCounter08", SCPerfTestUpdateCounter08, 101);
    UtRegisterTest("SCPerfTestUpdateCounter09", SCPerfTestUpdateCounter09, 1);
    UtRegisterTest("SCPerfTestUpdateGlobalCounter10",
                   SCPerfTestUpdateGlobalCounter10, 1);
    UtRegisterTest("SCPerfTestCounterValues11", SCPerfTestCounterValues11, 1);
#endif
}
