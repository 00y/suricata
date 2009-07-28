#ifndef __DETECT_MPM_H__
#define __DETECT_MPM_H__

/* XXX remove once */
MpmCtx mpm_ctx[1];

u_int32_t PacketPatternMatch(ThreadVars *, PatternMatcherThread *, Packet *);
void PacketPatternCleanup(ThreadVars *, PatternMatcherThread *);
void PatternMatchPrepare(MpmCtx *);
int PatternMatchPrepareGroup(DetectEngineCtx *, SigGroupHead *);
void PatternMatcherThreadInfo(ThreadVars *, PatternMatcherThread *);
void PatternMatchDestroy(MpmCtx *);
void PatternMatchDestroyGroup(SigGroupHead *);

int PatternMatcherThreadInit(ThreadVars *, void **);
int PatternMatcherThreadDeinit(ThreadVars *, void *);

#endif /* __DETECT_MPM_H__ */

