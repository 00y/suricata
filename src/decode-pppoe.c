/**
 * \file Copyright (c) 2009 Open Information Security Foundation
 * \author James Riden <jamesr@europe.com>
 *
 * \brief PPPOE Decoder
 */

#include "eidps-common.h"

#include "packet-queue.h"

#include "decode.h"
#include "decode-ppp.h"
#include "decode-pppoe.h"
#include "decode-events.h"

#include "util-unittest.h"

/**
 * \brief Main decoding function for PPPOE Discovery packets
 */
void DecodePPPOEDiscovery(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint8_t *pkt, uint16_t len, PacketQueue *pq)
{
    PerfCounterIncr(dtv->counter_pppoe, tv->pca);

    if (len < PPPOE_DISCOVERY_HEADER_MIN_LEN) {
        DECODER_SET_EVENT(p, PPPOE_PKT_TOO_SMALL);
        return;
    }

    p->pppoedh = (PPPOEDiscoveryHdr *)pkt;
    if (p->pppoedh == NULL)
        return;

    /* parse the PPPOE code */
    switch (ntohs(p->pppoedh->pppoe_code))
    {
        case  PPPOE_CODE_PADI:
            break;
        case  PPPOE_CODE_PADO:
            break;
        case  PPPOE_CODE_PADR:
            break;
        case PPPOE_CODE_PADS:
            break;
        case PPPOE_CODE_PADT:
            break;

        default:
#ifdef	DEBUG
            printf("Unknown PPPOE code: %" PRIx32 "\n",ntohs(p->pppoedh->pppoe_code));
#endif
            DECODER_SET_EVENT(p,PPPOE_WRONG_CODE);
    }

    /* parse any tags we have in the packet */

    uint16_t tag_type, tag_length;
    PPPOEDiscoveryTag* pppoedt = (PPPOEDiscoveryTag*) (p->pppoedh +  PPPOE_DISCOVERY_HEADER_MIN_LEN);

    uint16_t pppoe_length = ntohs(p->pppoedh->pppoe_length);
    uint16_t packet_length = len - PPPOE_DISCOVERY_HEADER_MIN_LEN ;

    if (pppoe_length>packet_length) {
#ifdef	DEBUG
        printf("Malformed PPPOE tags\n");
#endif
        DECODER_SET_EVENT(p,PPPOE_MALFORMED_TAGS);
    }

    while (pppoe_length>=4 && packet_length>=4)
    {
        tag_type = ntohs(pppoedt->pppoe_tag_type);
        tag_length = ntohs(pppoedt->pppoe_tag_length);

#ifdef DEBUG
        printf ("PPPoE Tag type %x, length %u\n", tag_type, tag_length);
#endif

        if (pppoe_length >= 4+tag_length) {
            pppoe_length -= (4 + tag_length);
        } else {
            pppoe_length = 0; // don't want an underflow
        }

        if (packet_length >= 4+tag_length) {
            packet_length -= (4 + tag_length);
        } else {
            packet_length = 0; // don't want an underflow
        }

        pppoedt = pppoedt + (4 + tag_length);
    }

}

/**
 * \brief Main decoding function for PPPOE Session packets
 */
void DecodePPPOESession(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint8_t *pkt, uint16_t len, PacketQueue *pq)
{
    PerfCounterIncr(dtv->counter_pppoe, tv->pca);

    if (len < PPPOE_SESSION_HEADER_LEN) {
        DECODER_SET_EVENT(p, PPPOE_PKT_TOO_SMALL);
        return;
    }

    p->pppoesh = (PPPOESessionHdr *)pkt;
    if (p->pppoesh == NULL)
        return;

#ifdef DEBUG
    printf("PPPOE VERSION %" PRIu32 " TYPE %" PRIu32 " CODE %" PRIu32 " SESSIONID %" PRIu32 " LENGTH %" PRIu32 "\n",
           p->pppoesh->pppoe_version,  p->pppoesh->pppoe_type,  p->pppoesh->pppoe_code,  ntohs(p->pppoesh->session_id),  ntohs(p->pppoesh->pppoe_length));
#endif

    /* can't use DecodePPP() here because we only get a single 2-byte word to indicate protocol instead of the full PPP header */

    if (ntohs(p->pppoesh->pppoe_length) > 0) {
        /* decode contained PPP packet */

        switch (ntohs(p->pppoesh->protocol))
        {
            case PPP_VJ_COMP:
            case PPP_IPX:
            case PPP_OSI:
            case PPP_NS:
            case PPP_DECNET:
            case PPP_APPLE:
            case PPP_BRPDU:
            case PPP_STII:
            case PPP_VINES:
            case PPP_HELLO:
            case PPP_LUXCOM:
            case PPP_SNS:
            case PPP_MPLS_UCAST:
            case PPP_MPLS_MCAST:
            case PPP_IPCP:
            case PPP_OSICP:
            case PPP_NSCP:
            case PPP_DECNETCP:
            case PPP_APPLECP:
            case PPP_IPXCP:
            case PPP_STIICP:
            case PPP_VINESCP:
            case PPP_IPV6CP:
            case PPP_MPLSCP:
            case PPP_LCP:
            case PPP_PAP:
            case PPP_LQM:
            case PPP_CHAP:
                DECODER_SET_EVENT(p,PPP_UNSUP_PROTO);
                break;

            case PPP_VJ_UCOMP:

                if(len < (PPPOE_SESSION_HEADER_LEN + IPV4_HEADER_LEN))    {
                    DECODER_SET_EVENT(p,PPPVJU_PKT_TOO_SMALL);
                    return;
                }

                if(IPV4_GET_RAW_VER((IPV4Hdr *)(pkt + PPPOE_SESSION_HEADER_LEN)) == 4) {
                    DecodeIPV4(tv, dtv, p, pkt + PPPOE_SESSION_HEADER_LEN, len - PPPOE_SESSION_HEADER_LEN, pq );
                }
                break;

            case PPP_IP:
                if(len < (PPPOE_SESSION_HEADER_LEN + IPV4_HEADER_LEN))    {
                    DECODER_SET_EVENT(p,PPPIPV4_PKT_TOO_SMALL);
                    return;
                }

                DecodeIPV4(tv, dtv, p, pkt + PPPOE_SESSION_HEADER_LEN, len - PPPOE_SESSION_HEADER_LEN, pq );
            break;

            /* PPP IPv6 was not tested */
            case PPP_IPV6:
                if(len < (PPPOE_SESSION_HEADER_LEN + IPV6_HEADER_LEN))    {
                    DECODER_SET_EVENT(p,PPPIPV6_PKT_TOO_SMALL);
                    return;
                }

                DecodeIPV6(tv, dtv, p, pkt + PPPOE_SESSION_HEADER_LEN, len - PPPOE_SESSION_HEADER_LEN, pq );
                break;

            default:
#ifdef	DEBUG
                printf("Unknown PPP protocol: %" PRIx32 "\n",ntohs(p->ppph->protocol));
#endif
                DECODER_SET_EVENT(p,PPP_WRONG_TYPE);
                return;
        }
    }
}

/** DecodePPPOEtest01
 *  \brief Decode malformed PPPOE packet (too short)
 *  \retval 1 Expected test value
 */
static int DecodePPPOEtest01 (void)   {

    uint8_t raw_pppoe[] = { 0x11, 0x00, 0x00, 0x00, 0x00 };
    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodePPPOESession(&tv, &dtv, &p, raw_pppoe, sizeof(raw_pppoe), NULL);

    if (DECODER_ISSET_EVENT(&p,PPPOE_PKT_TOO_SMALL))  {
        return 1;
    }

    return 0;
}

/** DecodePPPOEtest02
 *  \brief Valid PPPOE packet
 *  \retval 0 Expected test value
 */
static int DecodePPPOEtest02 (void)   {

    uint8_t raw_pppoe[] = {
        0x11, 0x00, 0x00, 0x01, 0x00, 0x68, 0x00, 0x21,
        0x45, 0xc0, 0x00, 0x64, 0x00, 0x1e, 0x00, 0x00,
        0xff, 0x01, 0xa7, 0x78, 0x0a, 0x00, 0x00, 0x02,
        0x0a, 0x00, 0x00, 0x01, 0x00, 0x00, 0x4a, 0x61,
        0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x0f, 0x3b, 0xd4, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
        0xab, 0xcd, 0xab, 0xcd };

    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodePPPOESession(&tv, &dtv, &p, raw_pppoe, sizeof(raw_pppoe), NULL);

    if(DECODER_ISSET_EVENT(&p,PPPOE_PKT_TOO_SMALL))  {
        return 1;
    }

    return 0;
}


/** DecodePPPOEtest03
 *  \brief Valid example PADO packet PPPOE packet taken from RFC2516
 *  \retval 0 Expected test value
 */
static int DecodePPPOEtest03 (void)   {

    /* example PADO packet taken from RFC2516 */
    uint8_t raw_pppoe[] = {
        0x11, 0x07, 0x00, 0x00, 0x00, 0x20, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x02, 0x00, 0x18, 0x47, 0x6f,
        0x20, 0x52, 0x65, 0x64, 0x42, 0x61, 0x63, 0x6b,
        0x20, 0x2d, 0x20, 0x65, 0x73, 0x68, 0x73, 0x68,
        0x65, 0x73, 0x68, 0x6f, 0x6f, 0x74
    };

    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    DecodePPPOEDiscovery(&tv, &dtv, &p, raw_pppoe, sizeof(raw_pppoe), NULL);

    return 0; // TODO
}

/** DecodePPPOEtest04
 *  \brief Valid example PPPOE packet taken from RFC2516 - but with wrong PPPOE code
 *  \retval 1 Expected test value
 */
static int DecodePPPOEtest04 (void)   {

    /* example PADI packet taken from RFC2516, but with wrong code */
    uint8_t raw_pppoe[] = {
        0x11, 0xbb, 0x00, 0x00, 0x00, 0x04, 0x01, 0x01,
        0x00, 0x00
    };

    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    DecodePPPOEDiscovery(&tv, &dtv, &p, raw_pppoe, sizeof(raw_pppoe), NULL);

    if(DECODER_ISSET_EVENT(&p,PPPOE_WRONG_CODE))  {
        return 1;
    }

    return 0;
}

/** DecodePPPOEtest05
 *  \brief Valid exaple PADO PPPOE packet taken from RFC2516, but too short for given length
 *  \retval 0 Expected test value
 */
static int DecodePPPOEtest05 (void)   {

    /* example PADI packet taken from RFC2516 */
    uint8_t raw_pppoe[] = {
        0x11, 0x07, 0x00, 0x00, 0x00, 0x20, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x02, 0x00, 0x18, 0x47, 0x6f,
        0x20, 0x52, 0x65, 0x64, 0x42, 0x61, 0x63, 0x6b,
        0x20, 0x2d, 0x20, 0x65, 0x73, 0x68, 0x73, 0x68
    };

    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    DecodePPPOEDiscovery(&tv, &dtv, &p, raw_pppoe, sizeof(raw_pppoe), NULL);

    if(DECODER_ISSET_EVENT(&p,PPPOE_MALFORMED_TAGS))  {
        return 1;
    }

    return 0;
}



/**
 * \brief Registers PPPOE unit tests
 * \todo More PPPOE tests
 */
void DecodePPPOERegisterTests(void) {
    UtRegisterTest("DecodePPPOEtest01", DecodePPPOEtest01, 1);
    UtRegisterTest("DecodePPPOEtest02", DecodePPPOEtest02, 0);
    UtRegisterTest("DecodePPPOEtest03", DecodePPPOEtest03, 0);
    UtRegisterTest("DecodePPPOEtest04", DecodePPPOEtest04, 1);
    UtRegisterTest("DecodePPPOEtest05", DecodePPPOEtest05, 1);
}

