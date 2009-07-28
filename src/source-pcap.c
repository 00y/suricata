/* Copyright (c) 2009 Victor Julien <victor@inliniac.net> */

/* TODO
 *
 *
 *
 */

#include <pthread.h>
#include <sys/signal.h>

#if LIBPCAP_VERSION_MAJOR == 1
#include <pcap/pcap.h>
#else
#include <pcap.h>
#endif

#include "eidps.h"
#include "decode.h"
#include "packet-queue.h"
#include "threads.h"
#include "threadvars.h"
#include "tm-queuehandlers.h"
#include "tm-modules.h"
#include "source-pcap.h"

typedef struct PcapGlobalVars_ {
    pcap_t *pcap_handle;
    void (*Decoder)(ThreadVars *, Packet *, u_int8_t *, u_int16_t, PacketQueue *);
} PcapGlobalVars;

typedef struct PcapThreadVars_
{
    /* counters */
    u_int32_t pkts;
    u_int64_t bytes;
    u_int32_t errs;

    ThreadVars *tv;
} PcapThreadVars;

static PcapGlobalVars pcap_g = { NULL, NULL, };

int ReceivePcap(ThreadVars *, Packet *, void *, PacketQueue *);
int ReceivePcapThreadInit(ThreadVars *, void *, void **);
void ReceivePcapThreadExitStats(ThreadVars *, void *);
int ReceivePcapThreadDeinit(ThreadVars *, void *);

int DecodePcap(ThreadVars *, Packet *, void *, PacketQueue *);

void TmModuleReceivePcapRegister (void) {
    tmm_modules[TMM_RECEIVEPCAP].name = "ReceivePcap";
    tmm_modules[TMM_RECEIVEPCAP].Init = ReceivePcapThreadInit;
    tmm_modules[TMM_RECEIVEPCAP].Func = ReceivePcap;
    tmm_modules[TMM_RECEIVEPCAP].ExitPrintStats = ReceivePcapThreadExitStats;
    tmm_modules[TMM_RECEIVEPCAP].Deinit = NULL;
    tmm_modules[TMM_RECEIVEPCAP].RegisterTests = NULL;
}

void TmModuleDecodePcapRegister (void) {
    tmm_modules[TMM_DECODEPCAP].name = "DecodePcap";
    tmm_modules[TMM_DECODEPCAP].Init = NULL;
    tmm_modules[TMM_DECODEPCAP].Func = DecodePcap;
    tmm_modules[TMM_DECODEPCAP].ExitPrintStats = NULL;
    tmm_modules[TMM_DECODEPCAP].Deinit = NULL;
    tmm_modules[TMM_DECODEPCAP].RegisterTests = NULL;
}

void PcapCallback(char *user, struct pcap_pkthdr *h, u_char *pkt) {
    //printf("PcapCallback: user %p, h %p, pkt %p\n", user, h, pkt);
    PcapThreadVars *ptv = (PcapThreadVars *)user;
    ThreadVars *tv = ptv->tv;

    mutex_lock(&mutex_pending);
    if (pending > MAX_PENDING) {
        pthread_cond_wait(&cond_pending, &mutex_pending);
    }
    mutex_unlock(&mutex_pending);

    Packet *p = tv->tmqh_in(tv);
    p->ts.tv_sec = h->ts.tv_sec;
    p->ts.tv_usec = h->ts.tv_usec;

    ptv->pkts++;
    ptv->bytes += h->caplen;

    p->pktlen = h->caplen;
    memcpy(p->pkt, pkt, p->pktlen);
    //printf("PcapCallback: p->pktlen: %u (pkt %02x, p->pkt %02x)\n", p->pktlen, *pkt, *p->pkt);

    /* pass on... */
    tv->tmqh_out(tv, p);
}

int ReceivePcap(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq) {
    PcapThreadVars *ptv = (PcapThreadVars *)data;

    //printf("ReceivePcap: tv %p\n", tv);
    int r = pcap_dispatch(pcap_g.pcap_handle, 1, (pcap_handler)PcapCallback, (u_char *)ptv);
    if (r <= 0) {
        //printf("ReceivePcap: error %s\n", pcap_geterr(pcap_g.pcap_handle));
    }

    return 0;
}

#if LIBPCAP_VERSION_MAJOR == 1
int ReceivePcapThreadInit(ThreadVars *tv, void *initdata, void **data) {
    if (initdata == NULL) {
        printf("ReceivePcapThreadInit error: initdata == NULL\n");
        return -1;
    }

    PcapThreadVars *ptv = malloc(sizeof(PcapThreadVars));
    if (ptv == NULL) {
        return -1;
    }
    memset(ptv, 0, sizeof(PcapThreadVars));

    ptv->tv = tv;

    printf("ReceivePcapThreadInit: using interface %s\n", (char *)initdata);

    /* XXX create a general pcap setup function */
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_g.pcap_handle = pcap_create((char *)initdata, errbuf);
    if (pcap_g.pcap_handle == NULL) {
        printf("error %s\n", pcap_geterr(pcap_g.pcap_handle));
        exit(1);
    }
    
    /* set Snaplen, Promisc, and Timeout. Must be called before pcap_activate */
    int pcap_set_snaplen_r = pcap_set_snaplen(pcap_g.pcap_handle,LIBPCAP_SNAPLEN);
    //printf("ReceivePcapThreadInit: pcap_set_snaplen(%p) returned %d\n", pcap_g.pcap_handle, pcap_set_snaplen_r);
    if (pcap_set_snaplen_r != 0) {
        printf("ReceivePcapThreadInit: error is %s\n", pcap_geterr(pcap_g.pcap_handle));
        exit(1);
    }

    int pcap_set_promisc_r = pcap_set_promisc(pcap_g.pcap_handle,LIBPCAP_PROMISC);
    //printf("ReceivePcapThreadInit: pcap_set_promisc(%p) returned %d\n", pcap_g.pcap_handle, pcap_set_promisc_r);
    if (pcap_set_promisc_r != 0) {
        printf("ReceivePcapThreadInit: error is %s\n", pcap_geterr(pcap_g.pcap_handle));
        exit(1);
    }

    int pcap_set_timeout_r = pcap_set_timeout(pcap_g.pcap_handle,LIBPCAP_COPYWAIT);
    //printf("ReceivePcapThreadInit: pcap_set_timeout(%p) returned %d\n", pcap_g.pcap_handle, pcap_set_timeout_r);
    if (pcap_set_timeout_r != 0) {
        printf("ReceivePcapThreadInit: error is %s\n", pcap_geterr(pcap_g.pcap_handle));
        exit(1);
    }
 
    /* activate the handle */
    int pcap_activate_r = pcap_activate(pcap_g.pcap_handle);
    //printf("ReceivePcapThreadInit: pcap_activate(%p) returned %d\n", pcap_g.pcap_handle, pcap_activate_r);
    if (pcap_activate_r != 0) {
        printf("ReceivePcapThreadInit: error is %s\n", pcap_geterr(pcap_g.pcap_handle));
        exit(1);
    }

    int datalink = pcap_datalink(pcap_g.pcap_handle);
    //printf("TmModuleReceivePcapFileRegister: datalink %d\n", datalink);
    switch(datalink)    {
        case LINKTYPE_LINUX_SLL:
            pcap_g.Decoder = DecodeSll;
            break;
        case LINKTYPE_ETHERNET:
            pcap_g.Decoder = DecodeEthernet;
            break;
        case LINKTYPE_PPP:
            pcap_g.Decoder = DecodePPP;
            break;
        default:
            printf("Error: datalink type %d not yet supported in module PcapFile.\n", datalink);
            break;
    }


    *data = (void *)ptv;
    return 0;
}
#else /* implied LIBPCAP_VERSION_MAJOR == 0 */
int ReceivePcapThreadInit(ThreadVars *tv, void *initdata, void **data) {
    if (initdata == NULL) {
        printf("ReceivePcapThreadInit error: initdata == NULL\n");
        return -1;
    }

    PcapThreadVars *ptv = malloc(sizeof(PcapThreadVars));
    if (ptv == NULL) {
        return -1;
    }
    memset(ptv, 0, sizeof(PcapThreadVars));

    ptv->tv = tv;

    printf("ReceivePcapThreadInit: using interface %s\n", (char *)initdata);

    /* XXX create a general pcap setup function */
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_g.pcap_handle = pcap_open_live((char *)initdata, LIBPCAP_SNAPLEN, LIBPCAP_PROMISC, LIBPCAP_COPYWAIT, errbuf);
    /* Cannot use pcap_geterror with pcap_open_live have to use errbuf */
    if (pcap_g.pcap_handle == NULL) {
        printf("error %s\n", errbuf);
        exit(1);
    }

    int datalink = pcap_datalink(pcap_g.pcap_handle);
    printf("TmModuleReceivePcapFileRegister: datalink %d\n", datalink);
    switch(datalink)    {
        case LINKTYPE_LINUX_SLL:
            pcap_g.Decoder = DecodeSll;
            break;
        case LINKTYPE_ETHERNET:
            pcap_g.Decoder = DecodeEthernet;
            break;
        case LINKTYPE_PPP:
            pcap_g.Decoder = DecodePPP;
            break;
        default:
            printf("Error: datalink type %d not yet supported in module PcapFile.\n", datalink);
            break;
    }


    *data = (void *)ptv;
    return 0;
}
#endif /* LIBPCAP_VERSION_MAJOR */

void ReceivePcapThreadExitStats(ThreadVars *tv, void *data) {
    PcapThreadVars *ptv = (PcapThreadVars *)data;

    printf(" - (%s) Packets %u, bytes %llu.\n", tv->name, ptv->pkts, ptv->bytes);
    return;
}

int ReceivePcapThreadDeinit(ThreadVars *tv, void *data) {
    return 0;
}

int DecodePcap(ThreadVars *t, Packet *p, void *data, PacketQueue *pq) {
    /* call the decoder */
    pcap_g.Decoder(t,p,p->pkt,p->pktlen,pq);
    return 0;
}

/* eof */

