/* API for stream handling */

#ifndef __STREAM_H__
#define __STREAM_H__

#include "flow.h"

#define STREAM_START        0x01
#define STREAM_EOF          0x02
#define STREAM_TOSERVER     0x04
#define STREAM_TOCLIENT     0x08
#define STREAM_GAP          0x10

#define MSG_DATA_SIZE       512

typedef struct _StreamMsg {
    u_int32_t id; /* unique stream id */
    u_int8_t flags; /* msg flags */
    Flow *flow; /* parent flow */

    union {
        /* case STREAM_START */
        struct {
            Address src_ip, dst_ip;
            Port src_port, dst_port;
            u_int8_t data[MSG_DATA_SIZE];
            u_int16_t data_len;
        } data;
        /* case STREAM_GAP */
        struct {
            u_int32_t gap_size;
        } gap;
    };

    struct _StreamMsg *next;
    struct _StreamMsg *prev;
} StreamMsg;

typedef struct _StreamMsgQueue {
    StreamMsg *top;
    StreamMsg *bot;
    u_int16_t len;
    pthread_mutex_t mutex_q;
    pthread_cond_t cond_q;
#ifdef DBG_PERF
    u_int16_t dbg_maxlen;
#endif /* DBG_PERF */
} StreamMsgQueue;

/* prototypes */
void StreamMsgQueuesInit(void);

StreamMsg *StreamMsgGetFromPool(void);
void StreamMsgReturnToPool(StreamMsg *);
StreamMsg *StreamMsgGetFromQueue(StreamMsgQueue *);
void StreamMsgPutInQueue(StreamMsg *);

StreamMsgQueue *StreamMsgQueueGetByPort(u_int16_t);

#endif /* __STREAM_H__ */

