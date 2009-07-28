/* Copyright (c) 2008 Victor Julien <victor@inliniac.net> */

/* alert unified
 *
 * Logs alerts in a format compatible to Snort's unified1 format, so it should
 * be readable by Barnyard.
 *
 * TODO
 * - inspect error messages for threadsafety
 * - inspect gettimeofday for threadsafely
 * - implement configuration
 *
 */

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

#include "vips.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"

#include "threadvars.h"
#include "tm-modules.h"

#include "util-unittest.h"

int AlertUnifiedLog (ThreadVars *, Packet *, void *, PacketQueue *);
int AlertUnifiedLogThreadInit(ThreadVars *, void *, void **);
int AlertUnifiedLogThreadDeinit(ThreadVars *, void *);

void TmModuleAlertUnifiedLogRegister (void) {
    tmm_modules[TMM_ALERTUNIFIEDLOG].name = "AlertUnifiedLog";
    tmm_modules[TMM_ALERTUNIFIEDLOG].Init = AlertUnifiedLogThreadInit;
    tmm_modules[TMM_ALERTUNIFIEDLOG].Func = AlertUnifiedLog;
    tmm_modules[TMM_ALERTUNIFIEDLOG].Deinit = AlertUnifiedLogThreadDeinit;
    tmm_modules[TMM_ALERTUNIFIEDLOG].RegisterTests = NULL;
}

typedef struct _AlertUnifiedLogThread {
    FILE *fp;
    u_int32_t size_limit;
    u_int32_t size_current;
} AlertUnifiedLogThread;

#define ALERTUNIFIEDLOG_LOGMAGIC 0xDEAD1080 /* taken from Snort */
#define ALERTUNIFIEDLOG_VERMAJOR 1          /* taken from Snort */
#define ALERTUNIFIEDLOG_VERMINOR 2          /* taken from Snort */

typedef struct _AlertUnifiedLogFileHeader {
    u_int32_t magic;
    u_int16_t ver_major;
    u_int16_t ver_minor;
    u_int32_t timezone;
    u_int32_t pad1; /* Snort has something called sigfigs, dunno what it is. I do know it's always 0. */
    u_int32_t snaplen;
    u_int32_t linktype;
} AlertUnifiedLogFileHeader;

typedef struct _AlertUnifiedLogPacketHeader {
    /* Snort's 'Event' structure */
    u_int32_t sig_gen;
    u_int32_t sig_sid;
    u_int32_t sig_rev;
    u_int32_t sig_class;
    u_int32_t sig_prio;
    u_int32_t pad1; /* Snort's event_id */
    u_int32_t pad2; /* Snort's event_reference */
    u_int32_t tv_sec1; /* from Snort's struct pcap_timeval */
    u_int32_t tv_usec1; /* from Snort's struct pcap_timeval */

    /* 32 bit unsigned flags */
    u_int32_t pktflags;

    /* Snort's 'SnortPktHeader' structure */
    u_int32_t tv_sec2; /* from Snort's struct pcap_timeval */
    u_int32_t tv_usec2; /* from Snort's struct pcap_timeval */
    u_int32_t caplen;
    u_int32_t pktlen;
} AlertUnifiedLogPacketHeader;

int AlertUnifiedLogCreateFile(ThreadVars *t, AlertUnifiedLogThread *aun) {
    char filename[2048]; /* XXX some sane default? */
    int ret;

    /* get the time so we can have a filename with seconds since epoch
     * in it. XXX review if we can take this info from somewhere else.
     * This is used both during init and runtime, so it must be thread
     * safe. */
    struct timeval ts;
    memset (&ts, 0, sizeof(struct timeval));
    gettimeofday(&ts, NULL);

    /* create the filename to use */
    snprintf(filename, sizeof(filename), "%s/%s.%u", "/var/log/eips", "unified.log", (u_int32_t)ts.tv_sec);

    /* XXX filename & location */
    aun->fp = fopen(filename, "wb");
    if (aun->fp == NULL) {
        printf("Error: fopen failed: %s\n", strerror(errno)); /* XXX errno threadsafety? */
        return -1;
    }

    /* write the fileheader to the file so the reader can recognize it */
    AlertUnifiedLogFileHeader hdr;
    hdr.magic = ALERTUNIFIEDLOG_LOGMAGIC;    
    hdr.ver_major = ALERTUNIFIEDLOG_VERMAJOR;    
    hdr.ver_minor = ALERTUNIFIEDLOG_VERMINOR;    
    hdr.timezone = 0; /* XXX */
    hdr.pad1 = 0; /* XXX */
    hdr.snaplen = 65536; /* XXX */
    hdr.linktype = DLT_EN10MB; /* XXX */

    ret = fwrite(&hdr, sizeof(hdr), 1, aun->fp);
    if (ret != 1) {
        printf("Error: fwrite failed: ret = %d, %s\n", ret, strerror(errno));
        return -1;
    }

    aun->size_current = sizeof(hdr);
    return 0;
}

int AlertUnifiedLogCloseFile(ThreadVars *t, AlertUnifiedLogThread *aun) {
    if (aun->fp != NULL)
        fclose(aun->fp);
    return 0;
}

int AlertUnifiedLogRotateFile(ThreadVars *t, AlertUnifiedLogThread *aun) {
    if (AlertUnifiedLogCloseFile(t,aun) < 0) {
        printf("Error: AlertUnifiedLogCloseFile failed\n");
        return -1;
    }
    if (AlertUnifiedLogCreateFile(t, aun) < 0) {
        printf("Error: AlertUnifiedCreateFile failed\n");
        return -1;
    }
    return 0;
}

int AlertUnifiedLog (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq)
{
    AlertUnifiedLogThread *aun = (AlertUnifiedLogThread *)data;
    AlertUnifiedLogPacketHeader hdr;
    int ret;
    u_int8_t ethh_offset = 0;
    u_int8_t buf[80000];
    u_int32_t buflen = 0;

    /* the unified1 format only supports IPv4. */
    if (p->alerts.cnt == 0 || !PKT_IS_IPV4(p))
        return 0;

    /* if we have no ethernet header (e.g. when using nfq), we have to create
     * one ourselves. */
    if (p->ethh == NULL) {
        ethh_offset = sizeof(EthernetHdr);
    }

    /* check and enforce the filesize limit */
    if ((aun->size_current + sizeof(hdr) + p->pktlen + ethh_offset) > aun->size_limit) {
        if (AlertUnifiedLogRotateFile(tv,aun) < 0)
            return -1;
    }

    /* XXX which one to add to this alert? Lets see how Snort solves this.
     * For now just take last alert. */
    PacketAlert *pa = &p->alerts.alerts[p->alerts.cnt-1];

    /* fill the hdr structure */
    hdr.sig_gen = pa->gid;
    hdr.sig_sid = pa->sid;
    hdr.sig_rev = pa->rev;
    hdr.sig_class = pa->class;
    hdr.sig_prio = pa->prio;
    hdr.pad1 = 0;
    hdr.pad2 = 0;
    hdr.tv_sec1 = hdr.tv_sec2 = p->ts.tv_sec;
    hdr.tv_usec1 = hdr.tv_usec2 = p->ts.tv_usec;
    hdr.pktflags = 0; /* XXX */
    hdr.pktlen = hdr.caplen = p->pktlen + ethh_offset;

    memcpy(buf,&hdr,sizeof(hdr));
    buflen = sizeof(hdr);

    if (p->ethh == NULL) {
        EthernetHdr ethh;
        memset(&ethh, 0, sizeof(EthernetHdr));
        ethh.eth_type = htons(ETHERNET_TYPE_IP);

        memcpy(buf+buflen,&ethh,sizeof(ethh));
        buflen += sizeof(ethh);
    }

    memcpy(buf+buflen,&p->pkt,p->pktlen);
    buflen += p->pktlen;

    /* write and flush so it's written immediately */
    ret = fwrite(buf, buflen, 1, aun->fp);
    if (ret != 1) {
        printf("Error: fwrite failed: %s\n", strerror(errno));
        return -1;
    }
    /* force writing to disk so barnyard will not read half
     * written records and choke. */
    fflush(aun->fp);

    aun->size_current += buflen;
    return 0;
}

int AlertUnifiedLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    AlertUnifiedLogThread *aun = malloc(sizeof(AlertUnifiedLogThread));
    if (aun == NULL) {
        return -1;
    }
    memset(aun, 0, sizeof(AlertUnifiedLogThread));

    aun->fp = NULL;

    int ret = AlertUnifiedLogCreateFile(t, aun);
    if (ret != 0) {
        printf("Error: AlertUnifiedCreateFile failed.\n");
        return -1;
    }

    /* XXX make configurable */
    aun->size_limit = 1 * 1024 * 1024;

    *data = (void *)aun;
    return 0;
}

int AlertUnifiedLogThreadDeinit(ThreadVars *t, void *data)
{
    AlertUnifiedLogThread *aun = (AlertUnifiedLogThread *)data;
    if (aun == NULL) {
        goto error;
    }

    if (AlertUnifiedLogCloseFile(t, aun) < 0)
        goto error;

    /* clear memory */
    memset(aun, 0, sizeof(AlertUnifiedLogThread));
    free(aun);
    return 0;

error:
    /* clear memory */
    if (aun != NULL) {
        memset(aun, 0, sizeof(AlertUnifiedLogThread));
        free(aun);
    }
    return -1;
}

