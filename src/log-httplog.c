/* Copyright (c) 2008 Victor Julien <victor@inliniac.net> */

/* httplog
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threadvars.h"
#include "tm-modules.h"

#include "threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "log-httplog.h"

#define DEFAULT_LOG_FILENAME "http.log"

#define MODULE_NAME "LogHttpLog"

TmEcode LogHttpLog (ThreadVars *, Packet *, void *, PacketQueue *);
TmEcode LogHttpLogIPv4(ThreadVars *, Packet *, void *, PacketQueue *);
TmEcode LogHttpLogIPv6(ThreadVars *, Packet *, void *, PacketQueue *);
TmEcode LogHttpLogThreadInit(ThreadVars *, void *, void **);
TmEcode LogHttpLogThreadDeinit(ThreadVars *, void *);
void LogHttpLogExitPrintStats(ThreadVars *, void *);
int LogHttpLogOpenFileCtx(LogFileCtx* , const char *);

void TmModuleLogHttpLogRegister (void) {
    tmm_modules[TMM_LOGHTTPLOG].name = MODULE_NAME;
    tmm_modules[TMM_LOGHTTPLOG].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG].Func = LogHttpLog;
    tmm_modules[TMM_LOGHTTPLOG].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG].RegisterTests = NULL;

    OutputRegisterModule(MODULE_NAME, "http-log", LogHttpLogInitCtx);
}

void TmModuleLogHttpLogIPv4Register (void) {
    tmm_modules[TMM_LOGHTTPLOG4].name = "LogHttpLogIPv4";
    tmm_modules[TMM_LOGHTTPLOG4].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG4].Func = LogHttpLogIPv4;
    tmm_modules[TMM_LOGHTTPLOG4].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG4].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG4].RegisterTests = NULL;
}

void TmModuleLogHttpLogIPv6Register (void) {
    tmm_modules[TMM_LOGHTTPLOG6].name = "LogHttpLogIPv6";
    tmm_modules[TMM_LOGHTTPLOG6].ThreadInit = LogHttpLogThreadInit;
    tmm_modules[TMM_LOGHTTPLOG6].Func = LogHttpLogIPv6;
    tmm_modules[TMM_LOGHTTPLOG6].ThreadExitPrintStats = LogHttpLogExitPrintStats;
    tmm_modules[TMM_LOGHTTPLOG6].ThreadDeinit = LogHttpLogThreadDeinit;
    tmm_modules[TMM_LOGHTTPLOG6].RegisterTests = NULL;
}

typedef struct LogHttpLogThread_ {
    LogFileCtx *file_ctx;
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    uint32_t uri_cnt;
} LogHttpLogThread;

static void CreateTimeString (const struct timeval *ts, char *str, size_t size) {
    time_t time = ts->tv_sec;
    struct tm *t = gmtime(&time);
    uint32_t sec = ts->tv_sec % 86400;

    snprintf(str, size, "%02d/%02d/%02d-%02d:%02d:%02d.%06u",
        t->tm_mon + 1, t->tm_mday, t->tm_year - 100,
        sec / 3600, (sec % 3600) / 60, sec % 60,
        (uint32_t) ts->tv_usec);
}

TmEcode LogHttpLogIPv4(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq)
{
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    int i;
    char timebuf[64];

    /* XXX add a better check for this */
    if (p->http_uri.cnt == 0)
        return TM_ECODE_OK;

    PktVar *pv_hn = PktVarGet(p, "http_host");
    PktVar *pv_ua = PktVarGet(p, "http_ua");

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    char srcip[16], dstip[16];
    inet_ntop(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
    inet_ntop(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));

    SCMutexLock(&aft->file_ctx->fp_mutex);
    for (i = 0; i < p->http_uri.cnt; i++) {
        /* time */
        fprintf(aft->file_ctx->fp, "%s ", timebuf);
        /* hostname */
        if (pv_hn != NULL) PrintRawUriFp(aft->file_ctx->fp, pv_hn->value, pv_hn->value_len);
        else fprintf(aft->file_ctx->fp, "<hostname unknown>");
        fprintf(aft->file_ctx->fp, " [**] ");
        /* uri */
        PrintRawUriFp(aft->file_ctx->fp, p->http_uri.raw[i], p->http_uri.raw_size[i]);
        fprintf(aft->file_ctx->fp, " [**] ");
        /* user agent */
        if (pv_ua != NULL) PrintRawUriFp(aft->file_ctx->fp, pv_ua->value, pv_ua->value_len);
        else fprintf(aft->file_ctx->fp, "<useragent unknown>");
        /* ip/tcp header info */
        fprintf(aft->file_ctx->fp, " [**] %s:%" PRIu32 " -> %s:%" PRIu32 "\n", srcip, p->sp, dstip, p->dp);
    }
    fflush(aft->file_ctx->fp);
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    aft->uri_cnt += p->http_uri.cnt;
    return TM_ECODE_OK;
}

TmEcode LogHttpLogIPv6(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq)
{
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    int i;
    char timebuf[64];

    /* XXX add a better check for this */
    if (p->http_uri.cnt == 0)
        return TM_ECODE_OK;

    PktVar *pv_hn = PktVarGet(p, "http_host");
    PktVar *pv_ua = PktVarGet(p, "http_ua");

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    char srcip[46], dstip[46];
    inet_ntop(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
    inet_ntop(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));

    SCMutexLock(&aft->file_ctx->fp_mutex);
    for (i = 0; i < p->http_uri.cnt; i++) {
        /* time */
        fprintf(aft->file_ctx->fp, "%s ", timebuf);
        /* hostname */
        if (pv_hn != NULL) PrintRawUriFp(aft->file_ctx->fp, pv_hn->value, pv_hn->value_len);
        else fprintf(aft->file_ctx->fp, "<hostname unknown>");
        fprintf(aft->file_ctx->fp, " [**] ");
        /* uri */
        PrintRawUriFp(aft->file_ctx->fp, p->http_uri.raw[i], p->http_uri.raw_size[i]);
        fprintf(aft->file_ctx->fp, " [**] ");
        /* user agent */
        if (pv_ua != NULL) PrintRawUriFp(aft->file_ctx->fp, pv_ua->value, pv_ua->value_len);
        else fprintf(aft->file_ctx->fp, "<useragent unknown>");
        /* ip/tcp header info */
        fprintf(aft->file_ctx->fp, " [**] %s:%" PRIu32 " -> %s:%" PRIu32 "\n", srcip, p->sp, dstip, p->dp);
    }
    fflush(aft->file_ctx->fp);
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    aft->uri_cnt += p->http_uri.cnt;
    return TM_ECODE_OK;
}

TmEcode LogHttpLog (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq)
{
    if (!(PKT_IS_TCP(p)))
        return TM_ECODE_OK;

    if (PKT_IS_IPV4(p)) {
        return LogHttpLogIPv4(tv, p, data, pq);
    } else if (PKT_IS_IPV6(p)) {
        return LogHttpLogIPv6(tv, p, data, pq);
    }

    return TM_ECODE_OK;
}

TmEcode LogHttpLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    LogHttpLogThread *aft = malloc(sizeof(LogHttpLogThread));
    if (aft == NULL) {
        return TM_ECODE_FAILED;
    }
    memset(aft, 0, sizeof(LogHttpLogThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for HTTPLog.  \"initdata\" argument NULL");
        free(aft);
        return TM_ECODE_FAILED;
    }
    /** Use the Ouptut Context (file pointer and mutex) */
    aft->file_ctx=(LogFileCtx*) initdata;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode LogHttpLogThreadDeinit(ThreadVars *t, void *data)
{
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    /* clear memory */
    memset(aft, 0, sizeof(LogHttpLogThread));

    free(aft);
    return TM_ECODE_OK;
}

void LogHttpLogExitPrintStats(ThreadVars *tv, void *data) {
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    if (aft == NULL) {
        return;
    }

    SCLogInfo("(%s) HTTP requests %" PRIu32 "", tv->name, aft->uri_cnt);
}

/** \brief Create a new http log LogFileCtx.
 *  \param conf Pointer to ConfNode containing this loggers configuration.
 *  \return NULL if failure, LogFileCtx* to the file_ctx if succesful
 * */
LogFileCtx *LogHttpLogInitCtx(ConfNode *conf)
{
    int ret=0;
    LogFileCtx* file_ctx=LogFileNewCtx();

    if(file_ctx == NULL)
    {
        SCLogError(SC_ERR_HTTP_LOG_GENERIC, "LogHttpLogInitCtx: Couldn't "
                   "create new file_ctx");
        return NULL;
    }

    const char *filename = ConfNodeLookupChildValue(conf, "filename");
    if (filename == NULL)
        filename = DEFAULT_LOG_FILENAME;

    /** fill the new LogFileCtx with the specific LogHttpLog configuration */
    ret=LogHttpLogOpenFileCtx(file_ctx, filename);

    if(ret < 0)
        return NULL;

    return file_ctx;
}

/** \brief Read the config set the file pointer, open the file
 *  \param file_ctx pointer to a created LogFileCtx using LogFileNewCtx()
 *  \param config_file for loading separate configs
 *  \return -1 if failure, 0 if succesful
 * */
int LogHttpLogOpenFileCtx(LogFileCtx *file_ctx, const char *filename)
{
    char log_path[PATH_MAX], *log_dir;
    if (ConfGet("default-log-dir", &log_dir) != 1)
        log_dir = DEFAULT_LOG_DIR;
    snprintf(log_path, PATH_MAX, "%s/%s", log_dir, filename);

    file_ctx->fp = fopen(log_path, "w");

    if (file_ctx->fp == NULL) {
        SCLogError(SC_ERR_FOPEN, "ERROR: failed to open %s: %s", log_path,
            strerror(errno));
        return -1;
    }

    return 0;
}


