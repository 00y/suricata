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
#include "source-pcap-file.h"
#include "util-time.h"

typedef struct PcapFileGlobalVars_ {
    pcap_t *pcap_handle;
    void (*Decoder)(ThreadVars *, Packet *, u_int8_t *, u_int16_t, PacketQueue *);
} PcapFileGlobalVars;

typedef struct PcapFileThreadVars_
{
    /* counters */
    u_int32_t pkts;
    u_int64_t bytes;
    u_int32_t errs;

    ThreadVars *tv;

    Packet *in_p;
} PcapFileThreadVars;

static PcapFileGlobalVars pcap_g = { NULL, NULL, };

int ReceivePcapFile(ThreadVars *, Packet *, void *, PacketQueue *);
int ReceivePcapFileThreadInit(ThreadVars *, void *, void **);
void ReceivePcapFileThreadExitStats(ThreadVars *, void *);
int ReceivePcapFileThreadDeinit(ThreadVars *, void *);

int DecodePcapFile(ThreadVars *, Packet *, void *, PacketQueue *);
int DecodePcapFileThreadInit(ThreadVars *, void *, void **);

void TmModuleReceivePcapFileRegister (void) {
    tmm_modules[TMM_RECEIVEPCAPFILE].name = "ReceivePcapFile";
    tmm_modules[TMM_RECEIVEPCAPFILE].Init = ReceivePcapFileThreadInit;
    tmm_modules[TMM_RECEIVEPCAPFILE].Func = ReceivePcapFile;
    tmm_modules[TMM_RECEIVEPCAPFILE].ExitPrintStats = ReceivePcapFileThreadExitStats;
    tmm_modules[TMM_RECEIVEPCAPFILE].Deinit = NULL;
    tmm_modules[TMM_RECEIVEPCAPFILE].RegisterTests = NULL;
}

void TmModuleDecodePcapFileRegister (void) {
    tmm_modules[TMM_DECODEPCAPFILE].name = "DecodePcapFile";
    tmm_modules[TMM_DECODEPCAPFILE].Init = DecodePcapFileThreadInit;
    tmm_modules[TMM_DECODEPCAPFILE].Func = DecodePcapFile;
    tmm_modules[TMM_DECODEPCAPFILE].ExitPrintStats = NULL;
    tmm_modules[TMM_DECODEPCAPFILE].Deinit = NULL;
    tmm_modules[TMM_DECODEPCAPFILE].RegisterTests = NULL;
}

void PcapFileCallback(char *user, struct pcap_pkthdr *h, u_char *pkt) {
    //printf("PcapFileCallback: user %p, h %p, pkt %p\n", user, h, pkt);
    PcapFileThreadVars *ptv = (PcapFileThreadVars *)user;
    //ThreadVars *tv = ptv->tv;

    mutex_lock(&mutex_pending);
    if (pending > MAX_PENDING) {
        pthread_cond_wait(&cond_pending, &mutex_pending);
    }
    mutex_unlock(&mutex_pending);

    Packet *p = ptv->in_p;

    p->ts.tv_sec = h->ts.tv_sec;
    p->ts.tv_usec = h->ts.tv_usec;
    TimeSet(&p->ts);

    ptv->pkts++;
    ptv->bytes += h->caplen;

    p->pktlen = h->caplen;
    memcpy(p->pkt, pkt, p->pktlen);
    //printf("PcapFileCallback: p->pktlen: %u (pkt %02x, p->pkt %02x)\n", p->pktlen, *pkt, *p->pkt);
}

int ReceivePcapFile(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq) {
    PcapFileThreadVars *ptv = (PcapFileThreadVars *)data;

    ptv->in_p = p;

    /// Right now we just support reading packets one at a time.
    int r = pcap_dispatch(pcap_g.pcap_handle, 1, (pcap_handler)PcapFileCallback, (u_char *)ptv);
    if (r <= 0) {
        printf("ReceivePcap: code %d error %s\n", r, pcap_geterr(pcap_g.pcap_handle));
        EngineStop();
        return 1;
    }

    return 0;
}

int ReceivePcapFileThreadInit(ThreadVars *tv, void *initdata, void **data) {
    if (initdata == NULL) {
        printf("ReceivePcapFileThreadInit error: initdata == NULL\n");
        return -1;
    }

    PcapFileThreadVars *ptv = malloc(sizeof(PcapFileThreadVars));
    if (ptv == NULL) {
        return -1;
    }
    memset(ptv, 0, sizeof(PcapFileThreadVars));

    char errbuf[PCAP_ERRBUF_SIZE] = "";
    pcap_g.pcap_handle = pcap_open_offline((char *)initdata, errbuf);
    if (pcap_g.pcap_handle == NULL) {
        printf("error %s\n", errbuf);
        exit(1);
    }

    int datalink = pcap_datalink(pcap_g.pcap_handle);
    printf("TmModuleReceivePcapFileRegister: datalink %d\n", datalink);
    switch(datalink)	{
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

    ptv->tv = tv;
    *data = (void *)ptv;
    return 0;
}

void ReceivePcapFileThreadExitStats(ThreadVars *tv, void *data) {
    PcapFileThreadVars *ptv = (PcapFileThreadVars *)data;

    printf(" - (%s) Packets %u, bytes %llu.\n", tv->name, ptv->pkts, ptv->bytes);
    return;
}

int ReceivePcapFileThreadDeinit(ThreadVars *tv, void *data) {
    return 0;
}

int DecodePcapFile(ThreadVars *t, Packet *p, void *data, PacketQueue *pq)
{
    PerfCounterIncr(COUNTER_DECODER_PKTS, t->pca);
    PerfCounterAdd(COUNTER_DECODER_BYTES, t->pca, p->pktlen);

    /* call the decoder */
    pcap_g.Decoder(t,p,p->pkt,p->pktlen,pq);
    return 0;
}

int DecodePcapFileThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    PerfRegisterCounter("decoder.pkts", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.bytes", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.ipv4", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.ipv6", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.ethernet", "DecodePcapFile", TYPE_UINT64,
                        "NULL", &tv->pctx);
    PerfRegisterCounter("decoder.sll", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.tcp", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.udp", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.icmpv4", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.icmpv6", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);
    PerfRegisterCounter("decoder.ppp", "DecodePcapFile", TYPE_UINT64, "NULL",
                        &tv->pctx);

    tv->pca = PerfGetAllCountersArray(&tv->pctx);

    PerfAddToClubbedTMTable("DecodePcapFile", &tv->pctx);

    return 0;
}

/* eof */

