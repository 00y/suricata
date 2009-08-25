/* Copyright (c) 2008 Victor Julien <victor@inliniac.net> */

#include "eidps-common.h"
#include "decode.h"
#include "decode-tcp.h"
#include "decode-events.h"
#include "util-unittest.h"

#include "flow.h"

/**
 * \brief Calculates the checksum for the TCP packet
 *
 * \param shdr Pointer to source address field from the IP packet.  Used as a
 *             part of the psuedoheader for computing the checksum
 * \param pkt  Pointer to the start of the TCP packet
 * \param hlen Total length of the TCP packet(header + payload)
 *
 * \retval csum Checksum for the TCP packet
 */
static inline uint16_t TCPCalculateChecksum(uint16_t *shdr, uint16_t *pkt,
                                            uint16_t tlen)
{
    uint16_t pad = 0;
    uint32_t csum = shdr[0];

    csum += shdr[1] + shdr[2] + shdr[3] + htons(6 + tlen);

    csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
        pkt[7] + pkt[9];

    tlen -= 20;
    pkt += 10;

    while (tlen >= 32) {
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] +
            pkt[7] + pkt[8] + pkt[9] + pkt[10] + pkt[11] + pkt[12] + pkt[13] +
            pkt[14] + pkt[15];
        tlen -= 32;
        pkt += 16;
    }

    while(tlen >= 8) {
        csum += pkt[0] + pkt[1] + pkt[2] + pkt[3];
        tlen -= 8;
        pkt += 4;
    }

    while(tlen >= 4) {
        csum += pkt[0] + pkt[1];
        tlen -= 4;
        pkt += 2;
    }

    while (tlen > 1) {
        csum += pkt[0];
        pkt += 1;
        tlen -= 2;
    }

    if (tlen == 1) {
        *(uint8_t *)(&pad) = (*(uint8_t *)pkt);
        csum += pad;
    }

    csum = (csum >> 16) + (csum & 0x0000FFFF);

    return (uint16_t) ~csum;
}

static int DecodeTCPOptions(ThreadVars *tv, Packet *p, uint8_t *pkt, uint16_t len)
{
    uint16_t plen = len;
    while (plen)
    {
        /* single byte options */
        if (*pkt == TCP_OPT_EOL) {
            break;
        } else if (*pkt == TCP_OPT_NOP) {
            pkt++;
            plen--;

        /* multibyte options */
        } else {
            if (plen < 2) {
                break;
            }

            /* we already know that the total options len is valid,
             * so here the len of the specific option must be bad.
             * Also check for invalid lengths 0 and 1. */
            if (*(pkt+1) > plen || *(pkt+1) < 2) {
                DECODER_SET_EVENT(p,TCP_OPT_INVALID_LEN);
                return -1;
            }

            p->TCP_OPTS[p->TCP_OPTS_CNT].type = *pkt;
            p->TCP_OPTS[p->TCP_OPTS_CNT].len  = *(pkt+1);
            if (plen > 2)
                p->TCP_OPTS[p->TCP_OPTS_CNT].data = (pkt+2);
            else
                p->TCP_OPTS[p->TCP_OPTS_CNT].data = NULL;

            /* we are parsing the most commonly used opts to prevent
             * us from having to walk the opts list for these all the
             * time. */
            switch (p->TCP_OPTS[p->TCP_OPTS_CNT].type) {
                case TCP_OPT_WS:
                    if (p->TCP_OPTS[p->TCP_OPTS_CNT].len != TCP_OPT_WS_LEN) {
                        DECODER_SET_EVENT(p,TCP_OPT_INVALID_LEN);
                    } else {
                        if (p->tcpvars.ws != NULL) {
                            DECODER_SET_EVENT(p,TCP_OPT_DUPLICATE);
                        } else {
                            p->tcpvars.ws = &p->TCP_OPTS[p->TCP_OPTS_CNT];
                        }
                    }
                    break;
                case TCP_OPT_MSS:
                    if (p->TCP_OPTS[p->TCP_OPTS_CNT].len != TCP_OPT_MSS_LEN) {
                        DECODER_SET_EVENT(p,TCP_OPT_INVALID_LEN);
                    } else {
                        if (p->tcpvars.mss != NULL) {
                            DECODER_SET_EVENT(p,TCP_OPT_DUPLICATE);
                        } else {
                            p->tcpvars.mss = &p->TCP_OPTS[p->TCP_OPTS_CNT];
                        }
                    }
                    break;
                case TCP_OPT_SACKOK:
                    if (p->TCP_OPTS[p->TCP_OPTS_CNT].len != TCP_OPT_SACKOK_LEN) {
                        DECODER_SET_EVENT(p,TCP_OPT_INVALID_LEN);
                    } else {
                        if (p->tcpvars.sackok != NULL) {
                            DECODER_SET_EVENT(p,TCP_OPT_DUPLICATE);
                        } else {
                            p->tcpvars.sackok = &p->TCP_OPTS[p->TCP_OPTS_CNT];
                        }
                    }
                    break;
                case TCP_OPT_TS:
                    if (p->TCP_OPTS[p->TCP_OPTS_CNT].len != TCP_OPT_TS_LEN) {
                        DECODER_SET_EVENT(p,TCP_OPT_INVALID_LEN);
                    } else {
                        if (p->tcpvars.ts != NULL) {
                            DECODER_SET_EVENT(p,TCP_OPT_DUPLICATE);
                        } else {
                            p->tcpvars.ts = &p->TCP_OPTS[p->TCP_OPTS_CNT];
                        }
                    }
                    break;
            }

            pkt += p->TCP_OPTS[p->TCP_OPTS_CNT].len;
            plen -= (p->TCP_OPTS[p->TCP_OPTS_CNT].len);
            p->TCP_OPTS_CNT++;
        }
    }
    return 0;
}

static int DecodeTCPPacket(ThreadVars *tv, Packet *p, uint8_t *pkt, uint16_t len)
{
    if (len < TCP_HEADER_LEN) {
        DECODER_SET_EVENT(p, TCP_PKT_TOO_SMALL);
        return -1;
    }

    p->tcph = (TCPHdr *)pkt;

    p->tcpvars.hlen = TCP_GET_HLEN(p);
    if (len < p->tcpvars.hlen) {
        DECODER_SET_EVENT(p, TCP_HLEN_TOO_SMALL);
        return -1;
    }

    SET_TCP_SRC_PORT(p,&p->sp);
    SET_TCP_DST_PORT(p,&p->dp);

    p->tcpvars.tcp_opt_len = p->tcpvars.hlen - TCP_HEADER_LEN;
    if (p->tcpvars.tcp_opt_len > TCP_OPTLENMAX) {
        DECODER_SET_EVENT(p, TCP_INVALID_OPTLEN);
        return -1;
    }

    if (p->tcpvars.tcp_opt_len > 0) {
        DecodeTCPOptions(tv, p, pkt + TCP_HEADER_LEN, p->tcpvars.tcp_opt_len);
    }

    p->payload = pkt + p->tcpvars.hlen;
    p->payload_len = len - p->tcpvars.hlen;

    p->proto = IPPROTO_TCP;

    return 0;
}

void DecodeTCP(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint8_t *pkt, uint16_t len, PacketQueue *pq)
{
    PerfCounterIncr(dtv->counter_tcp, tv->pca);

    if (DecodeTCPPacket(tv, p,pkt,len) < 0) {
        p->tcph = NULL;
        return;
    }

#ifdef DEBUG
    printf("TCP sp: %" PRIu32 " -> dp: %" PRIu32 " - HLEN: %" PRIu32 " LEN: %" PRIu32 " %s%s%s%s\n",
        GET_TCP_SRC_PORT(p), GET_TCP_DST_PORT(p), p->tcpvars.hlen, len,
        p->tcpvars.sackok ? "SACKOK " : "",
        p->tcpvars.ws ? "WS " : "",
        p->tcpvars.ts ? "TS " : "",
        p->tcpvars.mss ? "MSS " : "");
#endif

    /* Flow is an integral part of us */
    FlowHandlePacket(tv, p);

    return;
}

static int TCPCalculateValidChecksumtest01(void)
{
    uint16_t csum = 0;

    uint8_t raw_ipshdr[] = {
        0x40, 0x8e, 0x7e, 0xb2, 0xc0, 0xa8, 0x01, 0x03};

    uint8_t raw_tcp[] = {
        0x00, 0x50, 0x8e, 0x16, 0x0d, 0x59, 0xcd, 0x3c,
        0xcf, 0x0d, 0x21, 0x80, 0xa0, 0x12, 0x16, 0xa0,
        0xfa, 0x03, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4,
        0x04, 0x02, 0x08, 0x0a, 0x6e, 0x18, 0x78, 0x73,
        0x01, 0x71, 0x74, 0xde, 0x01, 0x03, 0x03, 02};

    csum = *( ((uint16_t *)raw_tcp) + 8);

    return (csum == TCPCalculateChecksum((uint16_t *) raw_ipshdr,
                                         (uint16_t *)raw_tcp, sizeof(raw_tcp)));
}

static int TCPCalculateInvalidChecksumtest02(void)
{
    uint16_t csum = 0;

    uint8_t raw_ipshdr[] = {
        0x40, 0x8e, 0x7e, 0xb2, 0xc0, 0xa8, 0x01, 0x03};

    uint8_t raw_tcp[] = {
        0x00, 0x50, 0x8e, 0x16, 0x0d, 0x59, 0xcd, 0x3c,
        0xcf, 0x0d, 0x21, 0x80, 0xa0, 0x12, 0x16, 0xa0,
        0xfa, 0x03, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4,
        0x04, 0x02, 0x08, 0x0a, 0x6e, 0x18, 0x78, 0x73,
        0x01, 0x71, 0x74, 0xde, 0x01, 0x03, 0x03, 03};

    csum = *( ((uint16_t *)raw_tcp) + 8);

    return (csum == TCPCalculateChecksum((uint16_t *) raw_ipshdr,
                                         (uint16_t *)raw_tcp, sizeof(raw_tcp)));
}

void DecodeTCPRegisterTests(void)
{
    UtRegisterTest("TCPCalculateValidChecksumtest01",
                   TCPCalculateValidChecksumtest01, 1);
    UtRegisterTest("TCPCalculateInvalidChecksumtest02",
                   TCPCalculateInvalidChecksumtest02, 0);
}
