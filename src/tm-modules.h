#ifndef __TM_MODULES_H__
#define __TM_MODULES_H__

#include "threadvars.h"

typedef struct TmModule_ {
    char *name;

    /** thread handling */
    int (*ThreadInit)(ThreadVars *, void *, void **);
    void (*ThreadExitPrintStats)(ThreadVars *, void *);
    int (*ThreadDeinit)(ThreadVars *, void *);

    /** the packet processing function */
    int (*Func)(ThreadVars *, Packet *, void *, PacketQueue *);

    void (*RegisterTests)(void);
} TmModule;

enum {
    TMM_DECODENFQ,
    TMM_VERDICTNFQ,
    TMM_RECEIVENFQ,
    TMM_RECEIVEPCAP,
    TMM_RECEIVEPCAPFILE,
    TMM_DECODEPCAP,
    TMM_DECODEPCAPFILE,
    TMM_RECEIVEPFRING,
    TMM_DECODEPFRING,
    TMM_DETECT,
    TMM_ALERTFASTLOG,
    TMM_ALERTFASTLOG4,
    TMM_ALERTFASTLOG6,
    TMM_ALERTUNIFIEDLOG,
    TMM_ALERTUNIFIEDALERT,
    TMM_ALERTUNIFIED2ALERT,
    TMM_ALERTDEBUGLOG,
    TMM_RESPONDREJECT,
    TMM_LOGHTTPLOG,
    TMM_LOGHTTPLOG4,
    TMM_LOGHTTPLOG6,
    TMM_STREAMTCP,
    TMM_SIZE,
};

TmModule tmm_modules[TMM_SIZE];

/** Global structure for Output Context */
typedef struct LogFileCtx_ {
    FILE *fp;
    pthread_mutex_t fp_mutex;
    /** It will be locked if the log/alert
     * record cannot be written to the file in one call */

    char *config_file;
    /** To know where did we read this config */

} LogFileCtx;

LogFileCtx *LogFileNewCtx();
int LogFileFreeCtx(LogFileCtx *);

TmModule *TmModuleGetByName(char *name);
int TmModuleRegister(char *name, int (*module_func)(ThreadVars *, Packet *, void *));
void TmModuleDebugList(void);
void TmModuleRegisterTests(void);

#endif /* __TM_MODULES_H__ */

