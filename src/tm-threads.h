#ifndef __TM_THREADS_H__
#define __TM_THREADS_H__

extern ThreadVars *tv_root;

void Tm1SlotSetFunc(ThreadVars *, TmModule *, void *);
void Tm2SlotSetFunc1(ThreadVars *, TmModule *, void *);
void Tm2SlotSetFunc2(ThreadVars *, TmModule *, void *);
void Tm3SlotSetFunc1(ThreadVars *, TmModule *, void *);
void Tm3SlotSetFunc2(ThreadVars *, TmModule *, void *);
void Tm3SlotSetFunc3(ThreadVars *, TmModule *, void *);
void TmVarSlotSetFuncAppend(ThreadVars *, TmModule *, void *);
ThreadVars *TmThreadCreate(char *name, char *inq_name, char *inqh_name, char *outq_name, char *outqh_name, char *slots);
int TmThreadSpawn(ThreadVars *);
void TmThreadKillThreads(void);
void TmThreadAppend(ThreadVars *);
int TmThreadSetCPUAffinity(ThreadVars *, int);

#endif /* __TM_THREADS_H__ */

