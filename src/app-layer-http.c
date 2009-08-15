/* Copyright (c) 2009 Victor Julien */

#include "eidps.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"

#include "stream-tcp-private.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"

enum {
    HTTP_FIELD_NONE = 0,

    HTTP_FIELD_REQUEST_LINE,
    HTTP_FIELD_REQUEST_HEADERS,
    HTTP_FIELD_REQUEST_BODY,

    HTTP_FIELD_REQUEST_METHOD,
    HTTP_FIELD_REQUEST_URI,
    HTTP_FIELD_REQUEST_VERSION,

    /* must be last */
    HTTP_FIELD_MAX,
};

typedef struct HttpState_ {

} HttpState;

/** \brief Mapping between HTTP_FIELD_* and AppLayerParsers
 *
 * Map the http fields identifiers to the parsers.
 */
typedef struct HTTPParser_ {
    u_int16_t parser_idx;
} HTTPParser;

static HTTPParser http_field_table[HTTP_FIELD_MAX];


int HTTPParseRequestLine(void *http_state, void *parser_state, u_int8_t *input, u_int32_t input_len, AppLayerParserResultElement **output, u_int16_t *output_num) {
    printf("HTTPParseRequestLine: http_state %p, parser_state %p, input %p, input_len %u\n",
        http_state, parser_state, input, input_len);
    PrintRawDataFp(stdout, input,input_len);

    return 0;
}

int HTTPParseRequest(void *http_state, void *parser_state, u_int8_t *input, u_int32_t input_len, AppLayerParserResultElement **output, u_int16_t *output_num) {
    printf("HTTPParseRequest: http_state %p, parser_state %p, input %p, input_len %u\n",
        http_state, parser_state, input, input_len);

    char done = FALSE;
    u_int32_t offset = 0;
    AppLayerParserState *pstate = (AppLayerParserState *)parser_state;

    printf("HTTPParseRequest: pstate->buflen %u\n", pstate->buflen);

    u_int32_t u32 = 0;
    for ( ; u32 < input_len && pstate->buflen < sizeof(pstate->buf); u32++) {
        pstate->buf[pstate->buflen] = input[u32];
        pstate->buflen++;

        if (pstate->buflen >= 2 && pstate->buf[pstate->buflen - 2] == '\r' && pstate->buf[pstate->buflen - 1] == '\n') {
            printf("HTTPParseRequest: request line done.\n");
            done = TRUE;
            break;
        }
    }

    if (done == TRUE) {
        AppLayerParserResultElement *e = AppLayerGetResultElmt();
        if (e == NULL)
            return -1;

        e->name_idx = HTTP_FIELD_REQUEST_LINE;
        e->data_ptr = input;
        e->data_len = pstate->buflen;
        output[*output_num] = e;

        (*output_num)++;

        printf("HTTPParseRequest: request line:\n");
        PrintRawDataFp(stdout, e->data_ptr,e->data_len);

        offset += pstate->buflen;
        pstate->buflen = 0;
        done = FALSE;
    } else {
        /* bail with state update */
        return 0;
    }

    printf("HTTPParseRequest: u32 %u, pstate->buflen %u\n", u32, pstate->buflen);
    for ( ; u32 < input_len && pstate->buflen < sizeof(pstate->buf); u32++) {
        pstate->buf[pstate->buflen] = input[u32];
        pstate->buflen++;


        if (pstate->buflen > 3 &&
            pstate->buf[pstate->buflen - 4] == '\r' && pstate->buf[pstate->buflen - 3] == '\n' &&
            pstate->buf[pstate->buflen - 2] == '\r' && pstate->buf[pstate->buflen - 1] == '\n') {
            printf("HTTPParseRequest: request headers done @ u32 %u, pstate->buflen %u\n", u32, pstate->buflen);
            done = TRUE;
            break;
        }
    }

    if (done == TRUE) {
        AppLayerParserResultElement *e = AppLayerGetResultElmt();
        if (e == NULL)
            return -1;

        e->name_idx = HTTP_FIELD_REQUEST_HEADERS;
        e->data_ptr = input + offset;
        e->data_len = pstate->buflen;
        output[*output_num] = e;

        (*output_num)++;

        printf("HTTPParseRequest: request headers:\n");
        PrintRawDataFp(stdout, e->data_ptr,e->data_len);

        offset += pstate->buflen;
        pstate->buflen = 0;
        done = FALSE;
    } else {
        /* bail with state update */
        return 0;
    }
    return 1;
}

int HTTPParseResponse(void *http_state, void *parser_state, u_int8_t *input, u_int32_t input_len, AppLayerParserResultElement **output, u_int16_t *output_num) {
    printf("HTTPParseResponse: http_state %p, parser_state %p, input %p, input_len %u\n",
        http_state, parser_state, input, input_len);
}

void RegisterHTTPParsers(void) {
    AppLayerRegisterProto("http", ALPROTO_HTTP, STREAM_TOSERVER, HTTPParseRequest);
    AppLayerRegisterProto("http", ALPROTO_HTTP, STREAM_TOCLIENT, HTTPParseResponse);

    AppLayerRegisterParser("http.request_line", ALPROTO_HTTP, HTTP_FIELD_REQUEST_LINE, HTTPParseRequestLine, "http");
}

