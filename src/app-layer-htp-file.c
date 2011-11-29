/* Copyright (C) 2007-2011 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * This file provides HTTP protocol file handling support for the engine
 * using HTP library.
 */

#include "suricata.h"
#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-radix-tree.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"

#include "util-spm.h"
#include "util-debug.h"
#include "app-layer-htp.h"
#include "util-time.h"
#include <htp/htp.h>

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "flow-util.h"
#include "flow-file.h"

#include "detect-engine.h"
#include "detect-engine-state.h"
#include "detect-parse.h"

#include "conf.h"

#include "util-memcmp.h"

/**
 *  \brief Open the file with "filename" and pass the first chunk
 *         of data if any.
 *
 *  \param f flow to store the file in
 *  \param filename name of the file
 *  \param filename_len length of the name
 *  \param data data chunk (if any)
 *  \param data_len length of the data portion
 *
 *  \retval 0 ok
 *  \retval -1 error
 */
int HTPFileOpen(Flow *f, uint8_t *filename, uint16_t filename_len,
        uint8_t *data, uint32_t data_len)
{
    int retval = 0;

    if (f == NULL) {
        return -1;
    }

    SCMutexLock(&f->files_m);
    {
        if (f->files == NULL) {
            f->files = FlowFileContainerAlloc();
            if (f->files == NULL) {
                retval = -1;
                goto end;
            }
        }

        if (FlowFileOpenFile(f->files, filename, filename_len,
                    data, data_len) == NULL)
        {
            retval = -1;
        }
    }

end:
    SCMutexUnlock(&f->files_m);
    return retval;
}

/**
 *  \brief Store a chunk of data in the flow
 *
 *  \param f flow to store the file in
 *  \param data data chunk (if any)
 *  \param data_len length of the data portion
 *
 *  \retval 0 ok
 *  \retval -1 error
 */
int HTPFileStoreChunk(Flow *f, uint8_t *data, uint32_t data_len) {
    SCEnter();

    int retval = 0;

    if (f == NULL) {
        SCReturnInt(-1);
    }

    SCMutexLock(&f->files_m);
    {
        if (f->files == NULL) {
            SCLogDebug("no files in flow");
            retval = -1;
            goto end;
        }

        if (FlowFileAppendData(f->files, data, data_len) == -1)
        {
            SCLogDebug("appending data failed");
            retval = -1;
        }
    }

end:
    SCMutexUnlock(&f->files_m);
    SCReturnInt(retval);
}

/**
 *  \brief Close the file in the flow
 *
 *  \param f flow to store in
 *  \param data data chunk if any
 *  \param data_len length of the data portion
 *  \param flags flags to indicate events
 *
 *  Currently on the FLOW_FILE_TRUNCATED flag is implemented, indicating
 *  that the file isn't complete but we're stopping storing it.
 *
 *  \retval 0 ok
 *  \retval -1 error
 */
int HTPFileClose(Flow *f, uint8_t *data, uint32_t data_len, uint8_t flags) {
    int retval = 0;

    if (f == NULL) {
        return -1;
    }

    SCMutexLock(&f->files_m);
    {
        if (f->files == NULL) {
            retval = -1;
            goto end;
        }

        if (FlowFileCloseFile(f->files, data, data_len, flags) == -1)
        {
            retval = -1;
        }
    }

end:
    SCMutexUnlock(&f->files_m);
    return retval;
}

#ifdef UNITTESTS
static int HTPFileParserTest01(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;
    f.src.family = AF_INET;
    f.dst.family = AF_INET;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    http_state = f.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(http_state->connp->conn->transactions, 0);
    if (tx == NULL) {
        goto end;
    }

    if (tx->request_method == NULL || memcmp(bstr_tocstr(tx->request_method), "POST", 4) != 0)
    {
        printf("expected method POST, got %s \n", bstr_tocstr(tx->request_method));
        goto end;
    }

    result = 1;
end:
    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    if (http_state != NULL)
        HTPStateFree(http_state);
    return result;
}

static int HTPFileParserTest02(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;
    f.src.family = AF_INET;
    f.dst.family = AF_INET;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf3, httplen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf4, httplen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    http_state = f.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(http_state->connp->conn->transactions, 0);
    if (tx == NULL) {
        goto end;
    }

    if (tx->request_method == NULL || memcmp(bstr_tocstr(tx->request_method), "POST", 4) != 0)
    {
        printf("expected method POST, got %s \n", bstr_tocstr(tx->request_method));
        goto end;
    }

    if (f.files == NULL || f.files->tail == NULL || f.files->tail->state != FLOWFILE_STATE_CLOSED) {
        goto end;
    }

    result = 1;
end:
    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    if (http_state != NULL)
        HTPStateFree(http_state);
    return result;
}

static int HTPFileParserTest03(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 337\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "file";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    uint8_t httpbuf5[] = "content\r\n";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */

    uint8_t httpbuf6[] = "-----------------------------277531038314945--";
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;
    f.src.family = AF_INET;
    f.dst.family = AF_INET;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf3, httplen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf4, httplen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 5 size %u <<<<\n", httplen5);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf5, httplen5);
    if (r != 0) {
        printf("toserver chunk 5 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 6 size %u <<<<\n", httplen6);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf6, httplen6);
    if (r != 0) {
        printf("toserver chunk 6 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    http_state = f.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(http_state->connp->conn->transactions, 0);
    if (tx == NULL) {
        goto end;
    }

    if (tx->request_method == NULL || memcmp(bstr_tocstr(tx->request_method), "POST", 4) != 0)
    {
        printf("expected method POST, got %s \n", bstr_tocstr(tx->request_method));
        goto end;
    }

    if (f.files == NULL || f.files->tail == NULL || f.files->tail->state != FLOWFILE_STATE_CLOSED) {
        goto end;
    }

    if (f.files->head->chunks_head->len != 11) {
        goto end;
    }

    result = 1;
end:
    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    if (http_state != NULL)
        HTPStateFree(http_state);
    return result;
}

static int HTPFileParserTest04(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 373\r\n"
                         "\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */

    uint8_t httpbuf2[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"email\"\r\n"
                         "\r\n"
                         "someaddress@somedomain.lan\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    uint8_t httpbuf3[] = "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */

    uint8_t httpbuf4[] = "file0123456789abcdefghijklmnopqrstuvwxyz";
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */

    uint8_t httpbuf5[] = "content\r\n";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */

    uint8_t httpbuf6[] = "-----------------------------277531038314945--";
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;
    f.src.family = AF_INET;
    f.dst.family = AF_INET;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 3 size %u <<<<\n", httplen3);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf3, httplen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 4 size %u <<<<\n", httplen4);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf4, httplen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 5 size %u <<<<\n", httplen5);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf5, httplen5);
    if (r != 0) {
        printf("toserver chunk 5 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 6 size %u <<<<\n", httplen6);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf6, httplen6);
    if (r != 0) {
        printf("toserver chunk 6 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    http_state = f.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(http_state->connp->conn->transactions, 0);
    if (tx == NULL) {
        goto end;
    }

    if (tx->request_method == NULL || memcmp(bstr_tocstr(tx->request_method), "POST", 4) != 0)
    {
        printf("expected method POST, got %s: ", bstr_tocstr(tx->request_method));
        goto end;
    }

    if (f.files == NULL || f.files->tail == NULL || f.files->tail->state != FLOWFILE_STATE_CLOSED) {
        goto end;
    }

    result = 1;
end:
    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    if (http_state != NULL)
        HTPStateFree(http_state);
    return result;
}

static int HTPFileParserTest05(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 544\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "Content-Disposition: form-data; name=\"uploadfile_1\"; filename=\"somepicture2.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "FILECONTENT\r\n"
        "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */

    TcpSession ssn;
    HtpState *http_state = NULL;

    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    f.protoctx = (void *)&ssn;
    f.src.family = AF_INET;
    f.dst.family = AF_INET;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    SCLogDebug("\n>>>> processing chunk 1 size %u <<<<\n", httplen1);
    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    http_state = f.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(http_state->connp->conn->transactions, 0);
    if (tx == NULL) {
        goto end;
    }

    if (tx->request_method == NULL || memcmp(bstr_tocstr(tx->request_method), "POST", 4) != 0)
    {
        printf("expected method POST, got %s \n", bstr_tocstr(tx->request_method));
        goto end;
    }

    if (f.files == NULL || f.files->tail == NULL || f.files->tail->state != FLOWFILE_STATE_CLOSED) {
        goto end;
    }

    if (f.files->head == f.files->tail)
        goto end;

    if (f.files->head->next != f.files->tail)
        goto end;

    if (f.files->head->chunks_head->len != 11) {
        printf("expected 11 but file is %u bytes instead\n",
                f.files->head->chunks_head->len);
        PrintRawDataFp(stdout, f.files->head->chunks_head->data,
                f.files->head->chunks_head->len);
        goto end;
    }

    if (memcmp("filecontent", f.files->head->chunks_head->data,
                f.files->head->chunks_head->len) != 0) {
        goto end;
    }

    if (f.files->tail->chunks_head->len != 11) {
        printf("expected 11 but file is %u bytes instead\n",
                f.files->tail->chunks_head->len);
        PrintRawDataFp(stdout, f.files->tail->chunks_head->data,
                f.files->tail->chunks_head->len);
        goto end;
    }

    if (memcmp("FILECONTENT", f.files->tail->chunks_head->data,
                f.files->tail->chunks_head->len) != 0) {
        goto end;
    }
    result = 1;
end:
    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    if (http_state != NULL)
        HTPStateFree(http_state);
    return result;
}

#endif /* UNITTESTS */

void HTPFileParserRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("HTPFileParserTest01", HTPFileParserTest01, 1);
    UtRegisterTest("HTPFileParserTest02", HTPFileParserTest02, 1);
    UtRegisterTest("HTPFileParserTest03", HTPFileParserTest03, 1);
    UtRegisterTest("HTPFileParserTest04", HTPFileParserTest04, 1);
    UtRegisterTest("HTPFileParserTest05", HTPFileParserTest05, 1);
#endif /* UNITTESTS */
}
