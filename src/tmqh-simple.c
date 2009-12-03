
#include "eidps.h"
#include "packet-queue.h"
#include "decode.h"
#include "threads.h"
#include "threadvars.h"

#include "tm-queuehandlers.h"

Packet *TmqhInputSimple(ThreadVars *t);
void TmqhOutputSimple(ThreadVars *t, Packet *p);

void TmqhSimpleRegister (void) {
    tmqh_table[TMQH_SIMPLE].name = "simple";
    tmqh_table[TMQH_SIMPLE].InHandler = TmqhInputSimple;
    tmqh_table[TMQH_SIMPLE].OutHandler = TmqhOutputSimple;
}

Packet *TmqhInputSimple(ThreadVars *t)
{
    PacketQueue *q = &trans_q[t->inq->id];

    sc_mutex_lock(&q->mutex_q);
    if (q->len == 0) {
        /* if we have no packets in queue, wait... */
        sc_cond_wait(&q->cond_q, &q->mutex_q);
    }

    if (t->sc_perf_pctx.perf_flag == 1)
        SCPerfUpdateCounterArray(t->sc_perf_pca, &t->sc_perf_pctx, 0);

    if (q->len > 0) {
        Packet *p = PacketDequeue(q);
        sc_mutex_unlock(&q->mutex_q);
        return p;
    } else {
        /* return NULL if we have no pkt. Should only happen on signals. */
        sc_mutex_unlock(&q->mutex_q);
        return NULL;
    }
}

void TmqhOutputSimple(ThreadVars *t, Packet *p)
{
    PacketQueue *q = &trans_q[t->outq->id];

    sc_mutex_lock(&q->mutex_q);
    PacketEnqueue(q, p);
    sc_cond_signal(&q->cond_q);
    sc_mutex_unlock(&q->mutex_q);
}

