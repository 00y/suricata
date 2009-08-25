/* Copyright (c) 2008 Victor Julien <victor@inliniac.net> */

#include "eidps-common.h"
#include "decode.h"
#include "decode-icmpv4.h"
#include "util-unittest.h"

/**
 * \brief Calculates the checksum for the ICMP packet
 *
 * \param pkt  Pointer to the start of the ICMP packet
 * \param hlen Total length of the ICMP packet(header + payload)
 *
 * \retval csum Checksum for the ICMP packet
 */
static inline uint16_t ICMPV4CalculateChecksum(uint16_t *pkt, uint16_t tlen)
{
    uint16_t pad = 0;
    uint32_t csum = pkt[0];

    tlen -= 4;
    pkt += 2;

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
        tlen -= 2;
        pkt += 1;
    }

    if (tlen == 1) {
        *(uint8_t *)(&pad) = (*(uint8_t *)pkt);
        csum += pad;
    }

    csum = (csum >> 16) + (csum & 0x0000FFFF);

    return (uint16_t) ~csum;
}

/** DecodeICMPV4
 *  \brief Main ICMPv4 decoding function
 */
void DecodeICMPV4(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint8_t *pkt, uint16_t len, PacketQueue *pq)
{
    PerfCounterIncr(dtv->counter_icmpv4, tv->pca);

    if (len < ICMPV4_HEADER_LEN) {
        /** \todo decode event */
        return;
    }

    p->icmpv4h = (ICMPV4Hdr *)pkt;

#ifdef DEBUG
    printf("ICMPV4 TYPE %" PRIu32 " CODE %" PRIu32 "\n", p->icmpv4h->type, p->icmpv4h->code);
#endif

    p->proto = IPPROTO_ICMP;
    return;
}

#ifdef UNITTESTS

/** DecodeICMPV4test01
 *  \brief
 *  \retval 0 Expected test value
 */
static int DecodeICMPV4test01(void) {
    uint8_t raw_icmpv4[] = {
        0x08, 0x00, 0x78, 0x47, 0xfc, 0x55, 0x00, 0x04,
        0x52, 0xab, 0x86, 0x4a, 0x84, 0x50, 0x0e, 0x00,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
        0xab };
    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodeICMPV4(&tv, &dtv, &p, raw_icmpv4, sizeof(raw_icmpv4), NULL);
    return 0;
}

/** DecodeICMPV4test02
 *  \brief
 *  \retval 0 Expected test value
 */
static int DecodeICMPV4test02(void) {
    uint8_t raw_icmpv4[] = {
        0x00, 0x00, 0x57, 0x64, 0xfb, 0x55, 0x00, 0x03,
        0x43, 0xab, 0x86, 0x4a, 0xf6, 0x49, 0x02, 0x00,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f };
    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodeICMPV4(&tv, &dtv, &p, raw_icmpv4, sizeof(raw_icmpv4), NULL);
    return 0;
}

/** DecodeICMPV4test03
 *  \brief  TTL exceeded
 *  \retval Expected test value: 0
 */
static int DecodeICMPV4test03(void) {
    uint8_t raw_icmpv4[] = {
        0x0b, 0x00, 0x6a, 0x3d, 0x00, 0x00, 0x00, 0x00,
        0x45, 0x00, 0x00, 0x3c, 0x64, 0x15, 0x00, 0x00,
        0x01, 0x11, 0xde, 0xfd, 0xc0, 0xa8, 0x01, 0x0d,
        0xd1, 0x55, 0xe3, 0x93, 0x8b, 0x12, 0x82, 0xaa,
        0x00, 0x28, 0x7c, 0xdd };
    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodeICMPV4(&tv, &dtv, &p, raw_icmpv4, sizeof(raw_icmpv4), NULL);
    return 0;
}

/** DecodeICMPV4test04
 *  \brief dest. unreachable, administratively prohibited
 *  \retval 0 Expected test value
 */
static int DecodeICMPV4test04(void) {
    uint8_t raw_icmpv4[] = {
        0x03, 0x0a, 0x36, 0xc3, 0x00, 0x00, 0x00, 0x00,
        0x45, 0x00, 0x00, 0x3c, 0x62, 0xee, 0x40, 0x00,
        0x33, 0x06, 0xb4, 0x8f, 0xc0, 0xa8, 0x01, 0x0d,
        0x58, 0x60, 0x16, 0x29, 0xb1, 0x0a, 0x00, 0x32,
        0x3e, 0x36, 0x38, 0x7c, 0x00, 0x00, 0x00, 0x00,
        0xa0, 0x02, 0x16, 0xd0, 0x72, 0x04, 0x00, 0x00,
        0x02, 0x04, 0x05, 0x8a, 0x04, 0x02, 0x08, 0x0a };
    Packet p;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    DecodeICMPV4(&tv, &dtv, &p, raw_icmpv4, sizeof(raw_icmpv4), NULL);
    return 0;
}

static int ICMPV4CalculateValidChecksumtest05(void) {
    uint16_t csum = 0;

    uint8_t raw_icmpv4[] = {
        0x08, 0x00, 0xab, 0x9b, 0x7f, 0x2b, 0x05, 0x2c,
        0x3f, 0x72, 0x93, 0x4a, 0x00, 0x4d, 0x0a, 0x00,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37};

    csum = *( ((uint16_t *)raw_icmpv4) + 1);
    return (csum == ICMPV4CalculateChecksum((uint16_t *)raw_icmpv4, sizeof(raw_icmpv4)));
}

static int ICMPV4CalculateInvalidChecksumtest06(void) {
    uint16_t csum = 0;

    uint8_t raw_icmpv4[] = {
        0x08, 0x00, 0xab, 0x9b, 0x7f, 0x2b, 0x05, 0x2c,
        0x3f, 0x72, 0x93, 0x4a, 0x00, 0x4d, 0x0a, 0x00,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x38};

    csum = *( ((uint16_t *)raw_icmpv4) + 1);
    return (csum == ICMPV4CalculateChecksum((uint16_t *)raw_icmpv4, sizeof(raw_icmpv4)));
}

/**
 * \brief Registers ICMPV4 unit test
 * \todo More ICMPv4 tests
 */
void DecodeICMPV4RegisterTests(void) {
    UtRegisterTest("DecodeICMPV4ttest01", DecodeICMPV4test01, 0);
    UtRegisterTest("DecodeICMPV4ttest02", DecodeICMPV4test02, 0);
    UtRegisterTest("DecodeICMPV4ttest03", DecodeICMPV4test03, 0);
    UtRegisterTest("DecodeICMPV4ttest04", DecodeICMPV4test04, 0);
    UtRegisterTest("ICMPV4CalculateValidChecksumtest05",
                   ICMPV4CalculateValidChecksumtest05, 1);
    UtRegisterTest("ICMPV4CalculateInvalidChecksumtest06",
                   ICMPV4CalculateInvalidChecksumtest06, 0);
}

#endif /* UNITTESTS */

