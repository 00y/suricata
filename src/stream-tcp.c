/* Copyright (c) 2008 Victor Julien <victor@inliniac.net> */
/*  2009 Gurvinder Singh <gurvindersinghdahiya@gmail.com>*/


#include "decode.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "eidps-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "threads.h"

#include "threadvars.h"
#include "tm-modules.h"

#include "util-pool.h"
#include "util-unittest.h"
#include "util-print.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"
#include "stream-tcp.h"

int StreamTcp (ThreadVars *, Packet *, void *, PacketQueue *);
int StreamTcpThreadInit(ThreadVars *, void *, void **);
int StreamTcpThreadDeinit(ThreadVars *, void *);
void StreamTcpExitPrintStats(ThreadVars *, void *);
static int ValidReset(TcpSession * , Packet *);
static int StreamTcpHandleFin(TcpSession *, Packet *);
void StreamTcpSessionPoolFree (Packet *);
void StreamTcpRegisterTests (void);
void StreamTcpReturnStreamSegments (TcpStream *);
void StreamTcpInitConfig(char);
extern void StreamTcpSegmentReturntoPool(TcpSegment *);

#define StreamTcpSessionFree  free
#define STREAMTCP_DEFAULT_SESSIONS  262144
#define STREAMTCP_DEFAULT_PREALLOC  32768

static Pool *ssn_pool;
static pthread_mutex_t ssn_pool_mutex;

typedef struct StreamTcpThread_ {
    u_int64_t pkts;
} StreamTcpThread;


void *StreamTcpSessionPoolAlloc(void *null) {
    void *ptr = malloc(sizeof(TcpSession));
    if (ptr == NULL)
        return NULL;

    memset(ptr, 0, sizeof(TcpSession));
    return ptr;
}


void TmModuleStreamTcpRegister (void) {
    StreamTcpReassembleInit();

    tmm_modules[TMM_STREAMTCP].name = "StreamTcp";
    tmm_modules[TMM_STREAMTCP].Init = StreamTcpThreadInit;
    tmm_modules[TMM_STREAMTCP].Func = StreamTcp;
    tmm_modules[TMM_STREAMTCP].ExitPrintStats = StreamTcpExitPrintStats;
    tmm_modules[TMM_STREAMTCP].Deinit = StreamTcpThreadDeinit;
    tmm_modules[TMM_STREAMTCP].RegisterTests = StreamTcpRegisterTests;
    StreamTcpInitConfig(STREAM_VERBOSE);
}

/** \brief          To initialize the stream global configuration data
 *
 *  \param  quiet   It tells the mode of operation, if it is TRUE nothing will
 *                  be get printed.
 */

void StreamTcpInitConfig(char quiet) {

    if (quiet == FALSE)
        printf("Initializing Stream:\n");

    memset(&stream_config,  0, sizeof(stream_config));
     /*set defaults*/
    stream_config.max_sessions = STREAMTCP_DEFAULT_SESSIONS;
    stream_config.prealloc_sessions = STREAMTCP_DEFAULT_PREALLOC;
    stream_config.midstream = TRUE;
    /*XXX GS should we need different function as last argument here not StreamTcpSessionPoolFree(),
     * this function will free () the memory not retrun to the pool !!*/
    ssn_pool = PoolInit(stream_config.max_sessions, stream_config.prealloc_sessions, StreamTcpSessionPoolAlloc, NULL, StreamTcpSessionFree);
    if (ssn_pool == NULL) {
        exit(1);
    }
    pthread_mutex_init(&ssn_pool_mutex, NULL);
}

/** \brief          The function is used to to fetch a TCP session from the
 *                  ssn_pool, when a TCP SYN is received.
 *
 *  \param  quiet   Packet P, which has been recieved for the new TCP session.
 *
 *  \retval TcpSession A new TCP session with field initilaized to 0/NULL.
 */

TcpSession *StreamTcpNewSession (Packet *p) {
    TcpSession *ssn = (TcpSession *)p->flow->stream;

    if (ssn == NULL) {
        mutex_lock(&ssn_pool_mutex);
        p->flow->stream = PoolGet(ssn_pool);
        mutex_unlock(&ssn_pool_mutex);
        ssn = (TcpSession *)p->flow->stream;
        if (ssn == NULL)
            return NULL;
        ssn->state = 0;
        ssn->l7data = NULL;
    }

    return ssn;
}

/**
 *  \brief  Function to handle the TCP_CLOSED or NONE state. The function handles
 *          packets while the session state is None which means a newly
 *          initialized structure, or a fully closed session.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateNone(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    switch (p->tcph->th_flags) {
        case TH_SYN:
        {
            if (ssn == NULL) {
                ssn = StreamTcpNewSession(p);
                if (ssn == NULL)
                    return -1;
            }

            /* set the state */
            ssn->state = TCP_SYN_SENT;
            printf("StreamTcpPacketStateNone (%p): =~ ssn state is now TCP_SYN_SENT\n", ssn);

            /* set the sequence numbers and window */
            ssn->client.isn = TCP_GET_SEQ(p);
            ssn->client.ra_base_seq = ssn->client.isn;
            ssn->client.next_seq = ssn->client.isn + 1;
            ssn->client.window = TCP_GET_WINDOW(p);

            //ssn->server.last_ack = ssn->client.isn + 1;
            //ssn->server.last_ack = TCP_GET_ACK(p);

            printf("StreamTcpPacketStateNone (%p): ssn->client.isn %" PRIu32 ", ssn->client.next_seq %" PRIu32 "\n",
                    ssn, ssn->client.isn, ssn->client.next_seq);

            if (p->tcpvars.ws != NULL) {
                printf("StreamTcpPacketStateNone (%p): p->tcpvars.ws %p, %02x\n", ssn, p->tcpvars.ws, *p->tcpvars.ws->data);
                ssn->client.wscale = *p->tcpvars.ws->data;
            }
            break;
        }
        case TH_SYN|TH_ACK:
            if (stream_config.midstream == FALSE)
                break;
            if (ssn == NULL) {
                ssn = StreamTcpNewSession(p);
                if (ssn == NULL)
                    return -1;
            }
            /* set the state */
            ssn->state = TCP_SYN_RECV;
            printf("StreamTcpPacketStateNone (%p): =~ midstream picked ssn state is now TCP_SYN_RECV\n", ssn);
            ssn->flags = STREAMTCP_FLAG_MIDSTREAM;

            /* sequence number & window */
            ssn->server.isn = TCP_GET_SEQ(p);
            ssn->server.ra_base_seq = ssn->server.isn;
            ssn->server.next_seq = ssn->server.isn + 1;
            ssn->server.window = TCP_GET_WINDOW(p);
            printf("StreamTcpPacketStateNone: (%p): server window %u\n", ssn, ssn->server.window);

            ssn->client.isn = TCP_GET_ACK(p) - 1;
            ssn->client.ra_base_seq = ssn->client.isn;
            ssn->client.next_seq = ssn->client.isn + 1;


            ssn->client.last_ack = TCP_GET_ACK(p);

            printf("StreamTcpPacketStateNone (%p): ssn->client.isn %u, ssn->client.next_seq %u\n",
                    ssn, ssn->client.isn, ssn->client.next_seq);

            if (p->tcpvars.ws != NULL) {
                printf("StreamTcpPacketStateNone (%p): p->tcpvars.ws %p, %02x\n", ssn, p->tcpvars.ws, *p->tcpvars.ws->data);
                ssn->server.wscale = *p->tcpvars.ws->data;
            }

            printf("StreamTcpPacketStateNone (%p): ssn->server.isn %u, ssn->server.next_seq %u, ssn->CLIENT.last_ack %u\n",
                    ssn, ssn->server.isn, ssn->server.next_seq, ssn->client.last_ack);

            break;
        /*Handle SYN/ACK and 3WHS shake missed together as it is almost similar. ryt ?*/
        case TH_ACK:
        case TH_ACK|TH_PUSH:
            if (stream_config.midstream == FALSE)
                break;
            if (ssn == NULL) {
                ssn = StreamTcpNewSession(p);
                if (ssn == NULL)
                    return -1;
            }
            /* set the state */
            ssn->state = TCP_ESTABLISHED;
            printf("StreamTcpPacketStateNone (%p): =~ midstream picked ssn state is now TCP_ESTABLISHED\n", ssn);
            ssn->flags = STREAMTCP_FLAG_MIDSTREAM;
            ssn->flags |= STREAMTCP_FLAG_MIDSTREAM_ESTABLISHED;

            /* set the sequence numbers and window */
            ssn->client.isn = TCP_GET_SEQ(p) - 1;
            ssn->client.ra_base_seq = ssn->client.isn;
            ssn->client.next_seq = TCP_GET_SEQ(p) + p->payload_len;
            ssn->client.window = TCP_GET_WINDOW(p);
            ssn->client.last_ack = TCP_GET_SEQ(p);
            ssn->client.next_win = ssn->client.last_ack + ssn->client.window;
             printf("StreamTcpPacketStateNone (%p): ssn->client.isn %u, ssn->client.next_seq %u\n",
                    ssn, ssn->client.isn, ssn->client.next_seq);

            ssn->server.isn = TCP_GET_ACK(p) - 1;
            ssn->server.ra_base_seq = ssn->server.isn;
            ssn->server.next_seq = ssn->server.isn + 1;
            ssn->server.last_ack = TCP_GET_ACK(p);

            /*XXX GS window scaling*/
            ssn->client.wscale = 0;
            ssn->server.wscale = 0;

            StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
        case TH_RST|TH_ACK|TH_PUSH:
        case TH_FIN:
        case TH_FIN|TH_ACK:
        case TH_FIN|TH_ACK|TH_PUSH:
            p->flow->stream = NULL;
            printf ("StreamTcpPacketStateNone: FIN or RST packet received, no session setup\n");
            break;
        default:
            printf("StreamTcpPacketStateNone: default case\n");
            break;
    }
    return 0;
}

/**
 *  \brief  Function to handle the TCP_SYN_SENT state. The function handles
 *          SYN, SYN/ACK, RSTpackets and correspondingly changes the connection
 *          state.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateSynSent(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch (p->tcph->th_flags) {
        case TH_SYN:
            printf("StreamTcpPacketStateSynSent (%p): SYN packet on state SYN_SENT... resent\n", ssn);
            break;
        case TH_SYN|TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateSynSent (%p): SYN/ACK received in the wrong direction\n", ssn);
                return -1;
            }

            /* Check if the SYN/ACK packet ack's the earlier
             * received SYN packet. */
            if (!(SEQ_EQ(TCP_GET_ACK(p), ssn->client.isn + 1))) {
                printf("StreamTcpPacketStateSynSent (%p): ACK mismatch, packet ACK %" PRIu32 " != %" PRIu32 " from stream\n",
                        ssn, TCP_GET_ACK(p), ssn->client.isn + 1);
                return -1;
            }

            /* update state */
            ssn->state = TCP_SYN_RECV;
            printf("StreamTcpPacketStateSynSent (%p): =~ ssn state is now TCP_SYN_RECV\n", ssn);

            /* sequence number & window */
            ssn->server.isn = TCP_GET_SEQ(p);
            ssn->server.ra_base_seq = ssn->server.isn;
            ssn->server.next_seq = ssn->server.isn + 1;
            ssn->server.window = TCP_GET_WINDOW(p);
            printf("StreamTcpPacketStateSynSent: (%p): window %" PRIu32 "\n", ssn, ssn->server.window);

            ssn->client.last_ack = TCP_GET_ACK(p);
            ssn->server.last_ack = ssn->server.isn + 1;

            if (ssn->client.wscale != 0 && p->tcpvars.ws != NULL) {
                printf("StreamTcpPacketStateSynSent (%p): p->tcpvars.ws %p, %02x\n", ssn, p->tcpvars.ws, *p->tcpvars.ws->data);
                ssn->server.wscale = *p->tcpvars.ws->data;
            } else {
                ssn->client.wscale = 0;
            }

            ssn->server.next_win = ssn->server.last_ack + ssn->server.window;
            printf("StreamTcpPacketStateSynSent (%p): next_win %" PRIu32 "\n", ssn, ssn->server.next_win);

            printf("StreamTcpPacketStateSynSent (%p): ssn->server.isn %" PRIu32 ", ssn->server.next_seq %" PRIu32 ", ssn->CLIENT.last_ack %" PRIu32 "\n",
                    ssn, ssn->server.isn, ssn->server.next_seq, ssn->client.last_ack);
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
            /* seq should be 0, win should be 0, ack should be isn +1.
             * check Snort's stream4/5 for more security*/
            if(ValidReset(ssn, p)){
                if(SEQ_EQ(TCP_GET_SEQ(p), ssn->client.isn) && SEQ_EQ(TCP_GET_WINDOW(p), 0) && SEQ_EQ(TCP_GET_ACK(p), (ssn->client.isn + 1))) {
                    ssn->state = TCP_CLOSED;
                    StreamTcpSessionPoolFree(p);
                }
            } else
                return -1;
            break;
        default:
            printf("StreamTcpPacketStateSynSent (%p): default case\n", ssn);
            break;
    }

    return 0;
}

/**
 *  \brief  Function to handle the TCP_SYN_RECV state. The function handles
 *          SYN, SYN/ACK, ACK, FIN, RST packets and correspondingly changes
 *          the connection state.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateSynRecv(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch (p->tcph->th_flags) {
        case TH_SYN:
            printf("StreamTcpPacketStateSynRecv (%p): SYN packet on state SYN_RECV... resent\n", ssn);
            break;
        case TH_SYN|TH_ACK:
            printf("StreamTcpPacketStateSynRecv (%p): SYN/ACK packet on state SYN_RECV... resent\n", ssn);
            break;
        case TH_ACK:
            if (PKT_IS_TOCLIENT(p)) {
                printf("StreamTcpPacketStateSynRecv (%p): ACK received in the wrong direction\n", ssn);
                return -1;
            }

            if (!(SEQ_EQ(TCP_GET_SEQ(p), ssn->client.next_seq))) {
                printf("StreamTcpPacketStateSynRecv (%p): ACK received in the wrong direction\n", ssn);
                return -1;
            }
            ssn->server.last_ack = TCP_GET_ACK(p);

            if (ssn->flags & STREAMTCP_FLAG_MIDSTREAM) {
                ssn->client.window = TCP_GET_WINDOW(p);
                ssn->server.next_win = ssn->server.last_ack + ssn->server.window;
                /*XXX GS window scaling need to be addressed!!*/
                ssn->server.wscale = 0;
                ssn->client.wscale = 0;
            }

            printf("StreamTcpPacketStateSynRecv (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                    ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

            ssn->state = TCP_ESTABLISHED;
            printf("StreamTcpPacketStateSynRecv (%p): =~ ssn state is now TCP_ESTABLISHED\n", ssn);

            ssn->client.next_seq += p->payload_len;
            ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

            ssn->client.next_win = ssn->client.last_ack + ssn->client.window;
            printf("StreamTcpPacketStateSynRecv (%p): next_win %" PRIu32 "\n", ssn, ssn->client.next_win);
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
            if(ValidReset(ssn, p)) {
                ssn->state = TCP_CLOSED;
                StreamTcpSessionPoolFree(p);
            } else
                return -1;
            break;
        case TH_FIN:
            /*FIN is handled in the same way as in TCP_ESTABLISHED case */;
            if((StreamTcpHandleFin(ssn, p)) == -1)
                return -1;
            break;
        default:
            printf("StreamTcpPacketStateSynRecv (%p): default case\n", ssn);
            break;
    }

    return 0;
}

/**
 *  \brief  Function to handle the TCP_ESTABLISHED state. The function handles
 *          ACK, FIN, RST packets and correspondingly changes the connection
 *          state. The function handles the data inside packets and call
 *          StreamTcpReassembleHandleSegment() to handle the reassembling.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateEstablished(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch (p->tcph->th_flags) {
        case TH_SYN:
            printf("StreamTcpPacketStateEstablished (%p): SYN packet on state ESTABLISED... resent\n", ssn);
            break;
        case TH_SYN|TH_ACK:
            printf("StreamTcpPacketStateEstablished (%p): SYN/ACK packet on state ESTABLISHED... resent\n", ssn);
            break;
        case TH_ACK:
        case TH_ACK|TH_PUSH:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateEstablished (%p): =+ pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_EQ(ssn->client.next_seq, TCP_GET_SEQ(p))) {
                    ssn->client.next_seq += p->payload_len;
                    printf("StreamTcpPacketStateEstablished (%p): ssn->client.next_seq %" PRIu32 "\n", ssn, ssn->client.next_seq);
                }

                if (SEQ_GEQ(TCP_GET_SEQ(p), ssn->client.last_ack) &&
                    SEQ_LEQ(TCP_GET_SEQ(p) + p->payload_len, ssn->client.next_win)) {
    
                    ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                    if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                        ssn->server.last_ack = TCP_GET_ACK(p);

                    if (SEQ_GT(ssn->client.last_ack + ssn->client.window, ssn->client.next_win)) {
                        ssn->client.next_win = ssn->client.last_ack + ssn->client.window;
                        printf("StreamTcpPacketStateEstablished (%p): ssn->client.next_win %" PRIu32 "\n", ssn, ssn->client.next_win);
                    }

                    StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);
                } else {
                    printf("StreamTcpPacketStateEstablished (%p): server !!!!! => SEQ mismatch, packet SEQ %" PRIu32 ", payload size %" PRIu32 " (%" PRIu32 "), last_ack %" PRIu32 ", next_win %" PRIu32 "\n",
                            ssn, TCP_GET_SEQ(p), p->payload_len, TCP_GET_SEQ(p) + p->payload_len, ssn->client.last_ack, ssn->client.next_win);
                }

                printf("StreamTcpPacketStateEstablished (%p): next SEQ %" PRIu32 ", last ACK %" PRIu32 ", next win %" PRIu32 ", win %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack, ssn->client.next_win, ssn->client.window);
            } else { /* implied to client */
                printf("StreamTcpPacketStateEstablished (%p): =+ pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                /*To get the server window value from the servers packet, when connection
                 is picked up as midstream*/
                if ((ssn->flags & STREAMTCP_FLAG_MIDSTREAM) && (ssn->flags & STREAMTCP_FLAG_MIDSTREAM_ESTABLISHED)) {
                    ssn->server.window = TCP_GET_WINDOW(p);
                    ssn->server.next_win = ssn->server.last_ack + ssn->server.window;
                    ssn->flags = STREAMTCP_FLAG_MIDSTREAM;
                }

                if (SEQ_EQ(ssn->server.next_seq, TCP_GET_SEQ(p))) {
                    ssn->server.next_seq += p->payload_len;
                    printf("StreamTcpPacketStateEstablished (%p): ssn->server.next_seq %" PRIu32 "\n", ssn, ssn->server.next_seq);
                }

                if (SEQ_GEQ(TCP_GET_SEQ(p), ssn->server.last_ack) &&
                    SEQ_LEQ(TCP_GET_SEQ(p) + p->payload_len, ssn->server.next_win)) {

                    ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                    if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                        ssn->client.last_ack = TCP_GET_ACK(p);

                    if (SEQ_GT(ssn->server.last_ack + ssn->server.window, ssn->server.next_win)) {
                        ssn->server.next_win = ssn->server.last_ack + ssn->server.window;
                        printf("StreamTcpPacketStateEstablished (%p): ssn->server.next_win %" PRIu32 "\n", ssn, ssn->server.next_win);
                    }

                    StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);
                } else {
                    printf("StreamTcpPacketStateEstablished (%p): client !!!!! => SEQ mismatch, packet SEQ %" PRIu32 ", payload size %" PRIu32 " (%" PRIu32 "), last_ack %" PRIu32 ", next_win %" PRIu32 "\n",
                            ssn, TCP_GET_SEQ(p), p->payload_len, TCP_GET_SEQ(p) + p->payload_len, ssn->server.last_ack, ssn->server.next_win);
                }

                printf("StreamTcpPacketStateEstablished (%p): next SEQ %" PRIu32 ", last ACK %" PRIu32 ", next win %" PRIu32 ", win %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack, ssn->server.next_win, ssn->server.window);
            }
            break;
        case TH_FIN:
        case TH_FIN|TH_ACK:
        case TH_FIN|TH_ACK|TH_PUSH:
            if((StreamTcpHandleFin(ssn, p)) == -1)
                return -1;
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
            if(ValidReset(ssn, p)) {
                if(PKT_IS_TOSERVER(p)) {
                    printf("StreamTcpPacketStateEstablished (%p): Reset received and state changed to TCP_CLOSED\n", ssn);
                    ssn->state = TCP_CLOSED;
                    /*Similar remote application is closed, so jump to CLOSE_WAIT*/
                    ssn->client.next_seq = TCP_GET_ACK(p);
                    ssn->server.next_seq = TCP_GET_SEQ(p) + p->payload_len + 1;
                    printf("StreamTcpPacketStateEstablished (%p): ssn->server.next_seq %" PRIu32 "\n", ssn, ssn->server.next_seq);
                    ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                    if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                        ssn->server.last_ack = TCP_GET_ACK(p);

                    StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                    printf("StreamTcpPacketStateEstablished (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                            ssn, ssn->client.next_seq, ssn->server.last_ack);
                    StreamTcpSessionPoolFree(p);
                } else {
                   printf("StreamTcpPacketStateEstablished (%p): Reset received and state changed to TCP_CLOSED\n", ssn);
                    ssn->state = TCP_CLOSED;
                    /*Similar remote application is closed, so jump to CLOSE_WAIT*/
                    ssn->server.next_seq = TCP_GET_SEQ(p) + p->payload_len + 1;
                    ssn->client.next_seq = TCP_GET_ACK(p);
                    printf("StreamTcpPacketStateEstablished (%p): ssn->server.next_seq %" PRIu32 "\n", ssn, ssn->server.next_seq);
                    ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                    if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                        ssn->client.last_ack = TCP_GET_ACK(p);

                    StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                    printf("StreamTcpPacketStateEstablished (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                            ssn, ssn->server.next_seq, ssn->client.last_ack);
                    StreamTcpSessionPoolFree(p);
                }
            } else
                return -1;
            break;
         default:
            printf("StreamTcpPacketStateEstablished (%p): default case\n", ssn);
            break;
    }
    return 0;
}

/**
 *  \brief  Function to handle the FIN packets for states TCP_SYN_RECV and
 *          TCP_ESTABLISHED and changes to another TCP state as required.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpHandleFin(TcpSession *ssn, Packet *p) {

    if (PKT_IS_TOSERVER(p)) {
        printf("StreamTcpPacket (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

        if (SEQ_LT(TCP_GET_SEQ(p), ssn->client.next_seq) || SEQ_GT(TCP_GET_SEQ(p), (ssn->client.last_ack + ssn->client.window))) {
              printf("StreamTcpPacket (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                      ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
              return -1;
        }

        printf("StreamTcpPacket (%p): state changed to TCP_CLOSE_WAIT\n", ssn);
        ssn->state = TCP_CLOSE_WAIT;
        ssn->client.next_seq = TCP_GET_ACK(p);
        ssn->server.next_seq = TCP_GET_SEQ(p) + p->payload_len + 1;
        printf("StreamTcpPacket (%p): ssn->server.next_seq %" PRIu32 "\n", ssn, ssn->server.next_seq);
        ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

        if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
            ssn->server.last_ack = TCP_GET_ACK(p);

        StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

        printf("StreamTcpPacket (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                ssn, ssn->client.next_seq, ssn->server.last_ack);
    } else { /* implied to client */
        printf("StreamTcpPacket (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

        if (SEQ_LT(TCP_GET_SEQ(p), ssn->server.next_seq) || SEQ_GT(TCP_GET_SEQ(p), (ssn->server.last_ack + ssn->server.window))) {
            printf("StreamTcpPacket (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                    ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
            return -1;
        }

        printf("StreamTcpPacket (%p): state changed to TCP_FIN_WAIT1\n", ssn);
        ssn->state = TCP_FIN_WAIT1;
        ssn->server.next_seq = TCP_GET_SEQ(p) + p->payload_len + 1;
        ssn->client.next_seq = TCP_GET_ACK(p);
        printf("StreamTcpPacket (%p): ssn->server.next_seq %" PRIu32 "\n", ssn, ssn->server.next_seq);
        ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

        if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
        ssn->client.last_ack = TCP_GET_ACK(p);

        StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

        printf("StreamTcpPacket (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                ssn, ssn->server.next_seq, ssn->client.last_ack);
    }
    return 0;
}

/**
 *  \brief  Function to handle the TCP_FIN_WAIT1 state. The function handles
 *          ACK, FIN, RST packets and correspondingly changes the connection
 *          state.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateFinWait1(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch (p->tcph->th_flags) {
        case TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateFinWait1 (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));


                printf("StreamTcpPacketStateFinWait1 (%p): state changed to TCP_FIN_WAIT2\n", ssn);
                ssn->state = TCP_FIN_WAIT2;

                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateFinWait1 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
            } else { /* implied to client */
                printf("StreamTcpPacketStateFinWait1 (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                printf("StreamTcpPacketStateFinWait1 (%p): state changed to TCP_FIN_WAIT2\n", ssn);
                ssn->state = TCP_FIN_WAIT2;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateFinWait1 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        case TH_FIN:
        case TH_FIN|TH_ACK:
        case TH_FIN|TH_ACK|TH_PUSH:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateFinWait1 (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_LT(TCP_GET_SEQ(p), ssn->client.next_seq || SEQ_GT(TCP_GET_SEQ(p), (ssn->client.last_ack + ssn->client.window)))) {
                    printf("StreamTcpPacketStateFinWait1 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait1 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;

                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateFinWait1 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
            } else { /* implied to client */
                printf("StreamTcpPacketStateFinWait1 (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_LT(TCP_GET_SEQ(p), ssn->server.next_seq) || SEQ_GT(TCP_GET_SEQ(p), (ssn->server.last_ack + ssn->server.window))) {
                    printf("StreamTcpPacketStateFinWait1 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait1 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateFinWait1 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
            if(ValidReset(ssn, p)) {
                printf("StreamTcpPacketStateFinWait1 (%p): Reset received state changed to TCP_CLOSED\n", ssn);
                ssn->state = TCP_CLOSED;
                StreamTcpSessionPoolFree(p);
            }
            else
                return -1;
            break;
        default:
            printf("StreamTcpPacketStateFinWait1 (%p): default case\n", ssn);
            break;
    }

    return 0;
}

/**
 *  \brief  Function to handle the TCP_FIN_WAIT2 state. The function handles
 *          ACK, RST, FIN packets and correspondingly changes the connection
 *          state.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateFinWait2(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch (p->tcph->th_flags) {
        case TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateFinWait2 (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->client.next_seq) {
                    printf("StreamTcpPacketStateFinWait2 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait2 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateFinWait2 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
            } else { /* implied to client */
                printf("StreamTcpPacketStateFinWait2 (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->server.next_seq) {
                    printf("StreamTcpPacketStateFinWait2 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait2 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateFinWait2 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        case TH_RST:
        case TH_RST|TH_ACK:
            if(ValidReset(ssn, p)) {
                printf("StreamTcpPacketStateFinWait2 (%p): Reset received state changed to TCP_CLOSED\n", ssn);
                ssn->state = TCP_CLOSED;
                StreamTcpSessionPoolFree(p);
            }
            else
                return -1;
            break;
        case TH_FIN:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateFinWait2 (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_LT(TCP_GET_SEQ(p), ssn->client.next_seq || SEQ_GT(TCP_GET_SEQ(p), (ssn->client.last_ack + ssn->client.window)))) {
                    printf("StreamTcpPacketStateFinWait2 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait2 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;

                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateFinWait2 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
            } else { /* implied to client */
                printf("StreamTcpPacketStateFinWait2 (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_LT(TCP_GET_SEQ(p), ssn->server.next_seq) || SEQ_GT(TCP_GET_SEQ(p), (ssn->server.last_ack + ssn->server.window))) {
                    printf("StreamTcpPacketStateFinWait2 (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateFinWait2 (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateFinWait2 (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        default:
            printf("StreamTcpPacketStateFinWait2 (%p): default case\n", ssn);
            break;
    }

    return 0;
}

/**
 *  \brief  Function to handle the TCP_CLOSING state. Upon arrival of ACK
 *          the connection goes to TCP_TIME_WAIT state. The state has been
 *          reached as both end application has been closed.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateClosing(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch(p->tcph->th_flags) {
        case TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateClosing (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->client.next_seq) {
                    printf("StreamTcpPacketStateClosing (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateClosing (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateClosing (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
            } else { /* implied to client */
                printf("StreamTcpPacketStateClosing (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->server.next_seq) {
                    printf("StreamTcpPacketStateClosing (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateClosing (%p): state changed to TCP_TIME_WAIT\n", ssn);
                ssn->state = TCP_TIME_WAIT;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateClosing (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        default:
            printf("StreamTcpPacketStateClosing (%p): default case\n", ssn);
            break;
    }
    return 0;
}

/**
 *  \brief  Function to handle the TCP_CLOSE_WAIT state. Upon arrival of FIN
 *          packet from server the connection goes to TCP_LAST_ACK state.
 *          The state is possible only for server host.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateCloseWait(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch(p->tcph->th_flags) {
        case TH_FIN:
            if (PKT_IS_TOCLIENT(p)) {
                printf("StreamTcpPacketStateCloseWait (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (SEQ_LT(TCP_GET_SEQ(p), ssn->server.next_seq) || SEQ_GT(TCP_GET_SEQ(p), (ssn->server.last_ack + ssn->server.window))) {
                    printf("StreamTcpPacketStateCloseWait (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateCloseWait (%p): state changed to TCP_LAST_ACK\n", ssn);
                ssn->state = TCP_LAST_ACK;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateCloseWait (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
            }
            break;
        default:
            printf("StreamTcpPacketStateCloseWait (%p): default case\n", ssn);
            break;
    }
    return 0;
}

/**
 *  \brief  Function to handle the TCP_LAST_ACK state. Upon arrival of ACK
 *          the connection goes to TCP_CLOSED state and stream memory is
 *          returned back to pool. The state is possible only for server host.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPakcetStateLastAck(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch(p->tcph->th_flags) {
        case TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateLastAck (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->client.next_seq) {
                    printf("StreamTcpPacketStateLastAck (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateLastAck (%p): state changed to TCP_CLOSED\n", ssn);
                ssn->state = TCP_CLOSED;
                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateLastAck (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
                StreamTcpSessionPoolFree(p);
            }
            break;
        default:
            printf("StreamTcpPacketStateLastAck (%p): default case\n", ssn);
            break;
    }
    return 0;
}

/**
 *  \brief  Function to handle the TCP_TIME_WAIT state. Upon arrival of ACK
 *          the connection goes to TCP_CLOSED state and stream memory is
 *          returned back to pool.
 *
 *  \param  tv      Thread Variable containig  input/output queue, cpu affinity etc.
 *  \param  p       Packet which has to be handled in this TCP state.
 *  \param  stt     Strean Thread module registered to handle the stream handling
 */

static int StreamTcpPacketStateTimeWait(ThreadVars *tv, Packet *p, StreamTcpThread *stt, TcpSession *ssn) {
    if (ssn == NULL)
        return -1;

    switch(p->tcph->th_flags) {
        case TH_ACK:
            if (PKT_IS_TOSERVER(p)) {
                printf("StreamTcpPacketStateTimeWait (%p): pkt (%" PRIu32 ") is to server: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->client.next_seq) {
                    printf("StreamTcpPacketStateTimeWait (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->client.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateTimeWait (%p): state changed to TCP_CLOSED\n", ssn);
                ssn->state = TCP_CLOSED;
                ssn->client.window = TCP_GET_WINDOW(p) << ssn->client.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->server.last_ack))
                    ssn->server.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->client, p);

                printf("StreamTcpPacketStateTimeWait (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->client.next_seq, ssn->server.last_ack);
                StreamTcpSessionPoolFree(p);
            } else {
                printf("StreamTcpPacketStateTimeWait (%p): pkt (%" PRIu32 ") is to client: SEQ %" PRIu32 ", ACK %" PRIu32 "\n",
                        ssn, p->payload_len, TCP_GET_SEQ(p), TCP_GET_ACK(p));

                if (TCP_GET_SEQ(p) != ssn->server.next_seq) {
                    printf("StreamTcpPacketStateTimeWait (%p): -> SEQ mismatch, packet SEQ %" PRIu32 " != %" PRIu32 " from stream\n",
                            ssn, TCP_GET_SEQ(p), ssn->server.next_seq);
                    return -1;
                }

                printf("StreamTcpPacketStateTimeWait (%p): state changed to TCP_CLOSED\n", ssn);
                ssn->state = TCP_CLOSED;
                ssn->server.window = TCP_GET_WINDOW(p) << ssn->server.wscale;

                if (SEQ_GT(TCP_GET_ACK(p),ssn->client.last_ack))
                    ssn->client.last_ack = TCP_GET_ACK(p);

                StreamTcpReassembleHandleSegment(ssn, &ssn->server, p);

                printf("StreamTcpPacketStateTimeWait (%p): =+ next SEQ %" PRIu32 ", last ACK %" PRIu32 "\n",
                        ssn, ssn->server.next_seq, ssn->client.last_ack);
                StreamTcpSessionPoolFree(p);
            }
            break;
        default:
            printf("StreamTcpPacketStateTimeWait (%p): default case\n", ssn);
            break;

    }
    return 0;
}

/* flow is and stays locked */
static int StreamTcpPacket (ThreadVars *tv, Packet *p, StreamTcpThread *stt) {
    TcpSession *ssn = (TcpSession *)p->flow->stream;

    if (ssn == NULL || ssn->state == 0 || ssn->state == TCP_CLOSED) {
        if (StreamTcpPacketStateNone(tv, p, stt, ssn) == -1)
            return -1;
    } else {
        switch (ssn->state) {
            case TCP_SYN_SENT:
                if(StreamTcpPacketStateSynSent(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_SYN_RECV:
                if(StreamTcpPacketStateSynRecv(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_ESTABLISHED:
                if(StreamTcpPacketStateEstablished(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_FIN_WAIT1:
                if(StreamTcpPacketStateFinWait1(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_FIN_WAIT2:
                if(StreamTcpPacketStateFinWait2(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_CLOSING:
                if(StreamTcpPacketStateClosing(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_CLOSE_WAIT:
                if(StreamTcpPacketStateCloseWait(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_LAST_ACK:
                if(StreamTcpPakcetStateLastAck(tv, p, stt, ssn))
                    return -1;
                break;
            case TCP_TIME_WAIT:
                if(StreamTcpPacketStateTimeWait(tv, p, stt, ssn))
                    return -1;
                break;
        }
    }

    return 0;
}

int StreamTcp (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq)
{
    StreamTcpThread *stt = (StreamTcpThread *)data;

    //PerfCounterAddUI64(COUNTER_STREAMTCP_STREAMS, tv->pca, a);

    if (!(PKT_IS_TCP(p)))
        return 0;

    if (p->flow == NULL)
        return 0;

#if 0
    printf("StreamTcp: seq %" PRIu32 ", ack %" PRIu32 ", %s%s%s%s%s%s%s%s: ", TCP_GET_SEQ(p), TCP_GET_ACK(p),
        TCP_ISSET_FLAG_FIN(p) ? "FIN " :"",
        TCP_ISSET_FLAG_SYN(p) ? "SYN " :"",
        TCP_ISSET_FLAG_RST(p) ? "RST " :"",
        TCP_ISSET_FLAG_PUSH(p)? "PUSH ":"",
        TCP_ISSET_FLAG_ACK(p) ? "ACK " :"",
        TCP_ISSET_FLAG_URG(p) ? "URG " :"",
        TCP_ISSET_FLAG_RES2(p)? "RES2 ":"",
        TCP_ISSET_FLAG_RES1(p)? "RES1 ":"");
#endif

    mutex_lock(&p->flow->m);
    StreamTcpPacket(tv, p, stt);
    mutex_unlock(&p->flow->m);

    stt->pkts++;
    return 0;
}

int StreamTcpThreadInit(ThreadVars *t, void *initdata, void **data)
{
    StreamTcpThread *stt = malloc(sizeof(StreamTcpThread));
    if (stt == NULL) {
        return -1;
    }
    memset(stt, 0, sizeof(StreamTcpThread));

    /* XXX */

    *data = (void *)stt;

    PerfRegisterCounter("streamTcp.tcp_streams", "StreamTcp", TYPE_DOUBLE, "NULL",
                        &t->pctx, TYPE_Q_AVERAGE, 1);

    t->pca = PerfGetAllCountersArray(&t->pctx);

    PerfAddToClubbedTMTable("StreamTcp", &t->pctx);

    return 0;
}

int StreamTcpThreadDeinit(ThreadVars *t, void *data)
{
    StreamTcpThread *stt = (StreamTcpThread *)data;
    if (stt == NULL) {
        return 0;
    }

    /* XXX */

    /* clear memory */
    memset(stt, 0, sizeof(StreamTcpThread));

    free(stt);
    return 0;
}

void StreamTcpExitPrintStats(ThreadVars *tv, void *data) {
    StreamTcpThread *stt = (StreamTcpThread *)data;
    if (stt == NULL) {
        return;
    }

    printf(" - (%s) Packets %" PRIu64 ".\n", tv->name, stt->pkts);
}

/**
 *  \brief   Function to check the validity of the RST packets based on the target
 *          OS of the given packet.
 *
 *  \param   ssn    TCP session to which the given packet belongs
 *  \param   p      Packet which has to be checked for its validity
 */

static int ValidReset(TcpSession *ssn, Packet *p) {

    uint8_t os_policy;
    if (PKT_IS_TOSERVER(p))
        os_policy = ssn->server.os_policy;
    else
        os_policy = ssn->client.os_policy;
    switch(os_policy) {
        case OS_POLICY_BSD:
        case OS_POLICY_FIRST:
        case OS_POLICY_HPUX10:
        case OS_POLICY_IRIX:
        case OS_POLICY_MACOS:
        case OS_POLICY_LAST:
        case OS_POLICY_WINDOWS:
        case OS_POLICY_WINDOWS2K3:
        case OS_POLICY_VISTA:
            if(PKT_IS_TOSERVER(p)){
                if(SEQ_EQ(TCP_GET_SEQ(p), ssn->client.next_seq)) {
                    printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                    return 1;
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and server SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->client.next_seq);
                    return 0;
                }
            } else { /* implied to client */
                if(SEQ_EQ(TCP_GET_SEQ(p), ssn->server.next_seq)) {
                    printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                    return 1;
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and client SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->server.next_seq);
                    return 0;
                }
            }
            break;
        case OS_POLICY_HPUX11:
            if(PKT_IS_TOSERVER(p)){
                if(SEQ_GEQ(TCP_GET_SEQ(p), ssn->client.next_seq)) {
                    printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                    return 1;
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and server SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->client.next_seq);
                    return 0;
                }
            } else { /* implied to client */
                if(SEQ_GEQ(TCP_GET_SEQ(p), ssn->server.next_seq)) {
                    printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                    return 1;
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and client SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->server.next_seq);
                    return 0;
                }
            }
            break;
        case OS_POLICY_OLD_LINUX:
        case OS_POLICY_LINUX:
        case OS_POLICY_SOLARIS:
            if(PKT_IS_TOSERVER(p)){
                if(SEQ_GEQ((TCP_GET_SEQ(p)+p->payload_len), ssn->client.last_ack)) { /*window base is needed !!*/
                    if(SEQ_LT(TCP_GET_SEQ(p), (ssn->client.next_seq + ssn->client.window))) {
                        printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                        return 1;
                    }
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and server SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->client.next_seq);
                    return 0;
                }
            } else { /* implied to client */
                if(SEQ_GEQ((TCP_GET_SEQ(p) + p->payload_len), ssn->server.last_ack)) { /*window base is needed !!*/
                    if(SEQ_LT(TCP_GET_SEQ(p), (ssn->server.next_seq + ssn->server.window))) {
                        printf("Reset is Valid! Pakcet SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p));
                        return 1;
                    }
                } else {
                    printf("Reset is not Valid! Packet SEQ: %" PRIu32 " and client SEQ: %" PRIu32 "\n", TCP_GET_SEQ(p), ssn->server.next_seq);
                    return 0;
                }
            }
            break;
        default:
            printf("Reset is not Valid! Packet SEQ: %" PRIu32 " & os_policy default case\n", TCP_GET_SEQ(p));
            break;
    }
    return 0;
}

/**
 *  \brief      Function to return the stream back to the pool. It returns the
 *              segments in the stream to the segment pool.
 *
 *  \param   p  Packet used to identify the stream.
 */

void StreamTcpSessionPoolFree (Packet *p) {
    TcpSession *ssn = (TcpSession *)p->flow->stream;
    if (ssn == NULL)
        return;
    StreamTcpReturnStreamSegments(&ssn->client);
    StreamTcpReturnStreamSegments(&ssn->server);
    mutex_lock(&ssn_pool_mutex);
    PoolReturn(ssn_pool, p->flow->stream);
    mutex_unlock(&ssn_pool_mutex);
    /*XXX GS i think this will suffice then as when ssn_pool is initialize
     StreamTcpSessionPoolAlloc () sets the ssn to 0 also. I think this will keep
     the session in the pool as clean. your thoughts ??*/
    memset(ssn, 0, sizeof(TcpSession));
    p->flow->stream = NULL;
}

void StreamTcpReturnStreamSegments (TcpStream *stream) {
    TcpSegment *temp = stream->seg_list;
    TcpSegment *prev;
    for (; temp!=NULL; ) {
        if (temp->next != NULL) {
            prev = temp;
            temp = temp->next;
            StreamTcpSegmentReturntoPool(prev);
        } else {
            StreamTcpSegmentReturntoPool(temp);
            temp = temp->next;
        }
    }
}
/**
 *  \test   Test the allocation of TCP session for a given packet from the
 *          ssn_pool.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest01 (void) {
    Packet p;
    Flow f;
    TcpSession ssn1;
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn1, 0, sizeof (TcpSession));
    f.stream = &ssn1;
    p.flow = &f;

    TcpSession *ssn = StreamTcpNewSession(&p);
    if (ssn == NULL) {
        printf("Session can not be allocated \n");
        return 0;
    }
    if (ssn->l7data != NULL) {
        printf("Layer 7 field not set to NULL \n");
        return 0;
    }
    if (ssn->state != 0) {
        printf("TCP state field not set to 0 \n");
        return 0;
    }

    StreamTcpSessionPoolFree(&p);
    return 1;
}

/**
 *  \test   Test the deallocation of TCP session for a given packet and return
 *          the memory back to ssn_pool and corresponding segments to segment
 *          pool.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest02 (void) {
    Packet p;
    Flow f;
    TcpSession ssn;
    ThreadVars tv;
    StreamTcpThread stt;
    u_int8_t payload[4];
    TCPHdr tcph;
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));
    memset(&stt, 0, sizeof (StreamTcpThread));
    memset(&tcph, 0, sizeof (TCPHdr));
    f.stream = &ssn;
    p.flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_flags = TH_SYN;
    p.tcph = &tcph;
    p.flowflags = FLOW_PKT_TOSERVER;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_ack = htonl(1);
    p.tcph->th_flags = TH_SYN | TH_ACK;
    p.flowflags = FLOW_PKT_TOCLIENT;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_ack = htonl(1);
    p.tcph->th_seq = htonl(1);
    p.tcph->th_flags = TH_ACK;
    p.flowflags = FLOW_PKT_TOSERVER;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_ack = htonl(1);
    p.tcph->th_seq = htonl(2);
    p.tcph->th_flags = TH_PUSH | TH_ACK;
    p.flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x41, 3); /*AAA*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.flowflags = FLOW_PKT_TOCLIENT;
    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_ack = htonl(1);
    p.tcph->th_seq = htonl(6);
    p.tcph->th_flags = TH_PUSH | TH_ACK;
    p.flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x42, 3); /*BBB*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.flowflags = FLOW_PKT_TOCLIENT;
    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    StreamTcpSessionPoolFree(&p);
    if (p.flow->stream != NULL)
        return 0;
    return 1;
}

/**
 *  \test   Test the setting up a TCP session when we missed the intial
 *          SYN packet of the session. The session is setup only if midstream
 *          sessions are allowed to setup.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest03 (void) {
    Packet p;
    Flow f;
    TcpSession ssn;
    ThreadVars tv;
    StreamTcpThread stt;
    TCPHdr tcph;
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));
    memset(&stt, 0, sizeof (StreamTcpThread));
    memset(&tcph, 0, sizeof (TCPHdr));
    f.stream = &ssn;
    p.flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_SYN|TH_ACK;
    p.tcph = &tcph;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(20);
    p.tcph->th_ack = htonl(11);
    p.tcph->th_flags = TH_ACK;
    p.flowflags = FLOW_PKT_TOSERVER;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(19);
    p.tcph->th_ack = htonl(11);
    p.tcph->th_flags = TH_ACK|TH_PUSH;
    p.flowflags = FLOW_PKT_TOSERVER;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    if (stream_config.midstream != TRUE)
        return 1;
    if (((TcpSession *)(p.flow->stream))->state != TCP_ESTABLISHED)
        return 0;

    if (((TcpSession *)(p.flow->stream))->client.next_seq != 20 ||
            ((TcpSession *)(p.flow->stream))->server.next_seq != 11)
        return 0;

    StreamTcpSessionPoolFree(&p);
    return 1;
}

/**
 *  \test   Test the setting up a TCP session when we missed the intial
 *          SYN/ACK packet of the session. The session is setup only if
 *          midstream sessions are allowed to setup.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest04 (void) {
    Packet p;
    Flow f;
    TcpSession ssn;
    ThreadVars tv;
    StreamTcpThread stt;
    TCPHdr tcph;
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));
    memset(&stt, 0, sizeof (StreamTcpThread));
    memset(&tcph, 0, sizeof (TCPHdr));
    f.stream = &ssn;
    p.flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK;
    p.tcph = &tcph;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(9);
    p.tcph->th_ack = htonl(19);
    p.tcph->th_flags = TH_ACK|TH_PUSH;
    p.flowflags = FLOW_PKT_TOSERVER;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    if (stream_config.midstream != TRUE)
        return 1;
    if (((TcpSession *)(p.flow->stream))->state != TCP_ESTABLISHED)
        return 0;

    if (((TcpSession *)(p.flow->stream))->client.next_seq != 10 ||
            ((TcpSession *)(p.flow->stream))->server.next_seq != 20)
        return 0;

    StreamTcpSessionPoolFree(&p);
    return 1;
}

/**
 *  \test   Test the setting up a TCP session when we missed the intial
 *          3WHS packet of the session. The session is setup only if
 *          midstream sessions are allowed to setup.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest05 (void) {
    Packet p;
    Flow f;
    TcpSession ssn;
    ThreadVars tv;
    StreamTcpThread stt;
    TCPHdr tcph;
    u_int8_t payload[4];
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));
    memset(&stt, 0, sizeof (StreamTcpThread));
    memset(&tcph, 0, sizeof (TCPHdr));
    f.stream = &ssn;
    p.flow = &f;

    tcph.th_win = htons(5480);
    tcph.th_seq = htonl(10);
    tcph.th_ack = htonl(20);
    tcph.th_flags = TH_ACK|TH_PUSH;
    p.tcph = &tcph;

    StreamTcpCreateTestPacket(payload, 0x41, 3); /*AAA*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(20);
    p.tcph->th_ack = htonl(13);
    p.tcph->th_flags = TH_ACK|TH_PUSH;
    p.flowflags = FLOW_PKT_TOCLIENT;

    StreamTcpCreateTestPacket(payload, 0x42, 3); /*BBB*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(13);
    p.tcph->th_ack = htonl(23);
    p.tcph->th_flags = TH_ACK|TH_PUSH;
    p.flowflags = FLOW_PKT_TOSERVER;

    StreamTcpCreateTestPacket(payload, 0x43, 3); /*CCC*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    p.tcph->th_seq = htonl(19);
    p.tcph->th_ack = htonl(16);
    p.tcph->th_flags = TH_ACK|TH_PUSH;
    p.flowflags = FLOW_PKT_TOCLIENT;

    StreamTcpCreateTestPacket(payload, 0x44, 3); /*DDD*/
    p.payload = payload;
    p.payload_len = 3;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    if (stream_config.midstream != TRUE)
        return 1;
    if (((TcpSession *)(p.flow->stream))->state != TCP_ESTABLISHED)
        return 0;

    if (((TcpSession *)(p.flow->stream))->client.next_seq != 16 ||
            ((TcpSession *)(p.flow->stream))->server.next_seq != 23)
        return 0;
    StreamTcpSessionPoolFree(&p);
    return 1;
}

/**
 *  \test   Test the setting up a TCP session when we have seen only the
 *          FIN, RST packets packet of the session. The session is setup only if
 *          midstream sessions are allowed to setup.
 *
 *  \retval On success it returns 1 and on failure 0.
 */

static int StreamTcpTest06 (void) {
    Packet p;
    Flow f;
    TcpSession ssn;
    ThreadVars tv;
    StreamTcpThread stt;
    TCPHdr tcph;
    memset (&p, 0, sizeof(Packet));
    memset (&f, 0, sizeof(Flow));
    memset(&ssn, 0, sizeof (TcpSession));
    memset(&tv, 0, sizeof (ThreadVars));
    memset(&stt, 0, sizeof (StreamTcpThread));
    memset(&tcph, 0, sizeof (TCPHdr));
    f.stream = &ssn;
    p.flow = &f;
    tcph.th_flags = TH_FIN;
    p.tcph = &tcph;

    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    if (((TcpSession *)(p.flow->stream)) != NULL)
        return 0;

    p.tcph->th_flags = TH_RST;
    if (StreamTcpPacket(&tv, &p, &stt) == -1)
        return 0;

    if (((TcpSession *)(p.flow->stream)) != NULL)
        return 0;
    return 1;
}

void StreamTcpRegisterTests (void) {
#ifdef UNITTESTS
    UtRegisterTest("StreamTcpTest01 -- TCP session allocation", StreamTcpTest01, 1);
    UtRegisterTest("StreamTcpTest02 -- TCP session deallocation", StreamTcpTest02, 1);
    UtRegisterTest("StreamTcpTest03 -- SYN missed MidStream session", StreamTcpTest03, 1);
    UtRegisterTest("StreamTcpTest04 -- SYN/ACK missed MidStream session", StreamTcpTest04, 1);
    UtRegisterTest("StreamTcpTest05 -- 3WHS missed MidStream session", StreamTcpTest05, 1);
    UtRegisterTest("StreamTcpTest06 -- FIN, RST message MidStream session", StreamTcpTest06, 1);
    /* set up the reassembly tests as well */
    StreamTcpReassembleRegisterTests();
#endif /* UNITTESTS */
}

