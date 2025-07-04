/* Copyright (C) 2007-2013 Open Information Security Foundation
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
 * \author Ignacio Sanchez <sanchezmartin.ji@gmail.com>
 *
 * Implements http logging portion of the engine.
 */

#include "suricata-common.h"
#include "detect.h"
#include "pkt-var.h"
#include "conf.h"

#include "threads.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "util-print.h"
#include "util-unittest.h"

#include "util-debug.h"

#include "output.h"
#include "log-httplog.h"
#include "app-layer-htp.h"
#include "app-layer.h"
#include "app-layer-parser.h"
#include "util-privs.h"
#include "util-buffer.h"

#include "util-logopenfile.h"
#include "util-time.h"
#include "log-cf-common.h"

#define DEFAULT_LOG_FILENAME "http.log"

#define MODULE_NAME "LogHttpLog"

#define OUTPUT_BUFFER_SIZE 65535

TmEcode LogHttpLogThreadInit(ThreadVars *, const void *, void **);
TmEcode LogHttpLogThreadDeinit(ThreadVars *, void *);
static void LogHttpLogDeInitCtx(OutputCtx *);

int LogHttpLogger(ThreadVars *tv, void *thread_data, const Packet *, Flow *f, void *state, void *tx, uint64_t tx_id);

void LogHttpLogRegister (void)
{
    OutputRegisterTxModule(LOGGER_HTTP, MODULE_NAME, "http-log", LogHttpLogInitCtx, ALPROTO_HTTP1,
            LogHttpLogger, LogHttpLogThreadInit, LogHttpLogThreadDeinit);
}

#define LOG_HTTP_CF_REQUEST_HOST 'h'
#define LOG_HTTP_CF_REQUEST_PROTOCOL 'H'
#define LOG_HTTP_CF_REQUEST_METHOD 'm'
#define LOG_HTTP_CF_REQUEST_URI      'u'
#define LOG_HTTP_CF_REQUEST_HEADER 'i'
#define LOG_HTTP_CF_REQUEST_COOKIE 'C'
#define LOG_HTTP_CF_REQUEST_LEN 'b'
#define LOG_HTTP_CF_RESPONSE_STATUS 's'
#define LOG_HTTP_CF_RESPONSE_HEADER 'o'
#define LOG_HTTP_CF_RESPONSE_LEN 'B'


typedef struct LogHttpFileCtx_ {
    LogFileCtx *file_ctx;
    uint32_t flags; /** Store mode */
    LogCustomFormat *cf;
} LogHttpFileCtx;

#define LOG_HTTP_DEFAULT 0
#define LOG_HTTP_EXTENDED 1
#define LOG_HTTP_CUSTOM 2

typedef struct LogHttpLogThread_ {
    LogHttpFileCtx *httplog_ctx;
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    uint32_t uri_cnt;

    MemBuffer *buffer;
} LogHttpLogThread;

/* Retrieves the selected cookie value */
static uint32_t GetCookieValue(const uint8_t *rawcookies, uint32_t rawcookies_len, char *cookiename,
        const uint8_t **cookievalue)
{
    const uint8_t *p = rawcookies;
    const uint8_t *cn = p;    /* ptr to cookie name start */
    const uint8_t *cv = NULL; /* ptr to cookie value start */
    while (p < rawcookies + rawcookies_len) {
        if (cv == NULL && *p == '=') {
            cv = p + 1;
        } else if (cv != NULL && (*p == ';' || p == rawcookies + rawcookies_len - 1) ) {
            /* Found end of cookie */
            p++;
            if (strlen(cookiename) == (unsigned int) (cv-cn-1) &&
                        strncmp(cookiename, (char *) cn, cv-cn-1) == 0) {
                *cookievalue = cv;
                return (uint32_t) (p-cv);
            }
            cv = NULL;
            cn = p + 1;
        }
        p++;
    }
    return 0;
}

/* Custom format logging */
static void LogHttpLogCustom(LogHttpLogThread *aft, htp_tx_t *tx, const SCTime_t ts, char *srcip,
        Port sp, char *dstip, Port dp)
{
    LogHttpFileCtx *httplog_ctx = aft->httplog_ctx;
    uint32_t i;
    size_t datalen;
    char buf[128];

    const uint8_t *cvalue = NULL;
    uint32_t cvalue_len = 0;

    const htp_header_t *h_request_hdr;
    const htp_header_t *h_response_hdr;

    for (i = 0; i < httplog_ctx->cf->cf_n; i++) {
        h_request_hdr = NULL;
        h_response_hdr = NULL;

        LogCustomFormatNode * node = httplog_ctx->cf->cf_nodes[i];
        if (! node) /* Should never happen */
            continue;

        switch (node->type){
            case LOG_CF_LITERAL:
            /* LITERAL */
                MemBufferWriteString(aft->buffer, "%s", node->data);
                break;
            case LOG_CF_TIMESTAMP:
            /* TIMESTAMP */
            LogCustomFormatWriteTimestamp(aft->buffer, node->data, ts);
            break;
            case LOG_CF_TIMESTAMP_U:
            /* TIMESTAMP USECONDS */
            snprintf(buf, sizeof(buf), "%06u", (unsigned int)SCTIME_USECS(ts));
            PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                    (uint8_t *)buf, MIN(strlen(buf), 6));
            break;
            case LOG_CF_CLIENT_IP:
            /* CLIENT IP ADDRESS */
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset,
                            aft->buffer->size, (uint8_t *)srcip,strlen(srcip));
                break;
            case LOG_CF_SERVER_IP:
            /* SERVER IP ADDRESS */
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset,
                            aft->buffer->size, (uint8_t *)dstip,strlen(dstip));
                break;
            case LOG_CF_CLIENT_PORT:
            /* CLIENT PORT */
                MemBufferWriteString(aft->buffer, "%" PRIu16 "", sp);
                break;
            case LOG_CF_SERVER_PORT:
            /* SERVER PORT */
                MemBufferWriteString(aft->buffer, "%" PRIu16 "", dp);
                break;
            case LOG_HTTP_CF_REQUEST_METHOD:
            /* METHOD */
            if (htp_tx_request_method(tx) != NULL) {
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        (uint8_t *)bstr_ptr(htp_tx_request_method(tx)),
                        bstr_len(htp_tx_request_method(tx)));
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_REQUEST_URI:
            /* URI */
            if (htp_tx_request_uri(tx) != NULL) {
                datalen = node->maxlen;
                if (datalen == 0 || datalen > bstr_len(htp_tx_request_uri(tx))) {
                    datalen = bstr_len(htp_tx_request_uri(tx));
                }
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        (uint8_t *)bstr_ptr(htp_tx_request_uri(tx)), datalen);
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_REQUEST_HOST:
            /* HOSTNAME */
            if (htp_tx_request_hostname(tx) != NULL) {
                datalen = node->maxlen;
                if (datalen == 0 || datalen > bstr_len(htp_tx_request_hostname(tx))) {
                    datalen = bstr_len(htp_tx_request_hostname(tx));
                }
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        (uint8_t *)bstr_ptr(htp_tx_request_hostname(tx)), datalen);
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_REQUEST_PROTOCOL:
            /* PROTOCOL */
            if (htp_tx_request_protocol(tx) != NULL) {
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        (uint8_t *)bstr_ptr(htp_tx_request_protocol(tx)),
                        bstr_len(htp_tx_request_protocol(tx)));
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_REQUEST_HEADER:
            /* REQUEST HEADER */
            h_request_hdr = htp_tx_request_header(tx, node->data);
            if (h_request_hdr != NULL) {
                datalen = node->maxlen;
                if (datalen == 0 || datalen > htp_header_value_len(h_request_hdr)) {
                    datalen = htp_header_value_len(h_request_hdr);
                }
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        htp_header_value_ptr(h_request_hdr), datalen);
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_REQUEST_COOKIE:
            /* REQUEST COOKIE */
            if (htp_tx_request_headers(tx) != NULL) {
                h_request_hdr = htp_tx_request_header(tx, "Cookie");
                if (h_request_hdr != NULL) {
                    cvalue_len = GetCookieValue(htp_header_value_ptr(h_request_hdr),
                            (uint32_t)htp_header_value_len(h_request_hdr), (char *)node->data,
                            &cvalue);
                }
            }
                if (cvalue_len > 0 && cvalue != NULL) {
                    datalen = node->maxlen;
                    if (datalen == 0 || datalen > cvalue_len) {
                        datalen = cvalue_len;
                    }
                    PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset,
                                    aft->buffer->size, cvalue, datalen);
                } else {
                    MemBufferWriteString(aft->buffer, LOG_CF_NONE);
                }
                break;
            case LOG_HTTP_CF_REQUEST_LEN:
            /* REQUEST LEN */
            MemBufferWriteString(
                    aft->buffer, "%" PRIuMAX "", (uintmax_t)htp_tx_request_message_len(tx));
            break;
            case LOG_HTTP_CF_RESPONSE_STATUS:
            /* RESPONSE STATUS */
            if (htp_tx_response_status(tx) != NULL) {
                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        (uint8_t *)bstr_ptr(htp_tx_response_status(tx)),
                        bstr_len(htp_tx_response_status(tx)));
            } else {
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
            }
                break;
            case LOG_HTTP_CF_RESPONSE_HEADER:
            /* RESPONSE HEADER */
            if (htp_tx_response_headers(tx) != NULL) {
                h_response_hdr = htp_tx_response_header(tx, node->data);
            }
                if (h_response_hdr != NULL) {
                    datalen = node->maxlen;
                    if (datalen == 0 || datalen > htp_header_value_len(h_response_hdr)) {
                        datalen = htp_header_value_len(h_response_hdr);
                    }
                    PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset,
                            aft->buffer->size, htp_header_value_ptr(h_response_hdr), datalen);
                } else {
                    MemBufferWriteString(aft->buffer, LOG_CF_NONE);
                }
                break;
            case LOG_HTTP_CF_RESPONSE_LEN:
            /* RESPONSE LEN */
            MemBufferWriteString(
                    aft->buffer, "%" PRIuMAX "", (uintmax_t)htp_tx_response_message_len(tx));
            break;
            default:
            /* NO MATCH */
                MemBufferWriteString(aft->buffer, LOG_CF_NONE);
                SCLogDebug("No matching parameter %%%c for custom http log.", node->type);
                break;
        }
    }
    MemBufferWriteString(aft->buffer, "\n");
}

static void LogHttpLogExtended(LogHttpLogThread *aft, htp_tx_t *tx)
{
    LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

    /* referer */
    const htp_header_t *h_referer = htp_tx_request_header(tx, "referer");

    if (h_referer != NULL) {
        PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                htp_header_value_ptr(h_referer), htp_header_value_len(h_referer));
    } else {
        MemBufferWriteString(aft->buffer, "<no referer>");
    }

    LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

    /* method */
    if (htp_tx_request_method(tx) != NULL) {
        PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                (uint8_t *)bstr_ptr(htp_tx_request_method(tx)),
                bstr_len(htp_tx_request_method(tx)));
    }
    LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

    /* protocol */
    if (htp_tx_request_protocol(tx) != NULL) {
        PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                (uint8_t *)bstr_ptr(htp_tx_request_protocol(tx)),
                bstr_len(htp_tx_request_protocol(tx)));
    } else {
        MemBufferWriteString(aft->buffer, "<no protocol>");
    }
    LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

    /* response status */
    if (htp_tx_response_status(tx) != NULL) {
        PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                (uint8_t *)bstr_ptr(htp_tx_response_status(tx)),
                bstr_len(htp_tx_response_status(tx)));
        /* Redirect? */
        if ((htp_tx_response_status_number(tx) > 300) &&
                ((htp_tx_response_status_number(tx)) < 303)) {
            const htp_header_t *h_location = htp_tx_response_header(tx, "location");
            if (h_location != NULL) {
                MemBufferWriteString(aft->buffer, " => ");

                PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                        htp_header_value_ptr(h_location), htp_header_value_len(h_location));
            }
        }
    } else {
        MemBufferWriteString(aft->buffer, "<no status>");
    }

    /* length */
    LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);
    MemBufferWriteString(
            aft->buffer, "%" PRIuMAX " bytes", (uintmax_t)htp_tx_response_message_len(tx));
}

static TmEcode LogHttpLogIPWrapper(ThreadVars *tv, void *data, const Packet *p, Flow *f, HtpState *htp_state, htp_tx_t *tx, uint64_t tx_id, int ipproto)
{
    SCEnter();

    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    LogHttpFileCtx *hlog = aft->httplog_ctx;
    char timebuf[64];

    /* check if we have HTTP state or not */
    CreateTimeString(p->ts, timebuf, sizeof(timebuf));

    char srcip[46], dstip[46];
    Port sp, dp;
    if ((PKT_IS_TOSERVER(p))) {
        switch (ipproto) {
            case AF_INET:
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
                break;
            case AF_INET6:
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
                break;
            default:
                goto end;
        }
        sp = p->sp;
        dp = p->dp;
    } else {
        switch (ipproto) {
            case AF_INET:
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), dstip, sizeof(dstip));
                break;
            case AF_INET6:
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), dstip, sizeof(dstip));
                break;
            default:
                goto end;
        }
        sp = p->dp;
        dp = p->sp;
    }

    SCLogDebug("got a HTTP request and now logging !!");

    /* reset */
    MemBufferReset(aft->buffer);

    if (hlog->flags & LOG_HTTP_CUSTOM) {
        LogHttpLogCustom(aft, tx, p->ts, srcip, sp, dstip, dp);
    } else {
        /* time */
        MemBufferWriteString(aft->buffer, "%s ", timebuf);

        /* hostname */
        if (htp_tx_request_hostname(tx) != NULL) {
            PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                    (uint8_t *)bstr_ptr(htp_tx_request_hostname(tx)),
                    bstr_len(htp_tx_request_hostname(tx)));
        } else {
            MemBufferWriteString(aft->buffer, "<hostname unknown>");
        }
        LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

        /* uri */
        if (htp_tx_request_uri(tx) != NULL) {
            PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                    (uint8_t *)bstr_ptr(htp_tx_request_uri(tx)), bstr_len(htp_tx_request_uri(tx)));
        }
        LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);

        /* user agent */
        const htp_header_t *h_user_agent = htp_tx_request_header(tx, "user-agent");
        if (h_user_agent != NULL) {
            PrintRawUriBuf((char *)aft->buffer->buffer, &aft->buffer->offset, aft->buffer->size,
                    htp_header_value_ptr(h_user_agent), htp_header_value_len(h_user_agent));
        } else {
            MemBufferWriteString(aft->buffer, "<useragent unknown>");
        }
        if (hlog->flags & LOG_HTTP_EXTENDED) {
            LogHttpLogExtended(aft, tx);
        }

        /* ip/tcp header info */
        LOG_CF_WRITE_STAR_SEPARATOR(aft->buffer);
        MemBufferWriteString(aft->buffer,
                "%s:%" PRIu16 " -> %s:%" PRIu16 "\n",
                srcip, sp, dstip, dp);
    }

    aft->uri_cnt ++;

    hlog->file_ctx->Write((const char *)MEMBUFFER_BUFFER(aft->buffer),
        MEMBUFFER_OFFSET(aft->buffer), hlog->file_ctx);

end:
    SCReturnInt(0);

}

int LogHttpLogger(ThreadVars *tv, void *thread_data, const Packet *p, Flow *f, void *state, void *tx, uint64_t tx_id)
{
    SCEnter();

    if (!(PacketIsTCP(p))) {
        SCReturnInt(TM_ECODE_OK);
    }

    int r = 0;
    if (PacketIsIPv4(p)) {
        r = LogHttpLogIPWrapper(tv, thread_data, p, f, (HtpState *)state, (htp_tx_t *)tx, tx_id, AF_INET);
    } else if (PacketIsIPv6(p)) {
        r = LogHttpLogIPWrapper(tv, thread_data, p, f, (HtpState *)state, (htp_tx_t *)tx, tx_id, AF_INET6);
    }

    SCReturnInt(r);
}

TmEcode LogHttpLogThreadInit(ThreadVars *t, const void *initdata, void **data)
{
    LogHttpLogThread *aft = SCCalloc(1, sizeof(LogHttpLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for LogHTTPLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /* Use the Output Context (file pointer and mutex) */
    aft->httplog_ctx= ((OutputCtx *)initdata)->data;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode LogHttpLogThreadDeinit(ThreadVars *t, void *data)
{
    LogHttpLogThread *aft = (LogHttpLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);
    /* clear memory */
    memset(aft, 0, sizeof(LogHttpLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

/** \brief Create a new http log LogFileCtx.
 *  \param conf Pointer to ConfNode containing this loggers configuration.
 *  \return NULL if failure, LogFileCtx* to the file_ctx if succesful
 * */
OutputInitResult LogHttpLogInitCtx(SCConfNode *conf)
{
    SCLogWarning("The http-log output has been deprecated and will be removed in Suricata 9.0.");
    OutputInitResult result = { NULL, false };
    LogFileCtx* file_ctx = LogFileNewCtx();
    if(file_ctx == NULL) {
        SCLogError("couldn't create new file_ctx");
        return result;
    }

    if (SCConfLogOpenGeneric(conf, file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
        LogFileFreeCtx(file_ctx);
        return result;
    }

    LogHttpFileCtx *httplog_ctx = SCCalloc(1, sizeof(LogHttpFileCtx));
    if (unlikely(httplog_ctx == NULL)) {
        LogFileFreeCtx(file_ctx);
        return result;
    }

    httplog_ctx->file_ctx = file_ctx;

    const char *extended = SCConfNodeLookupChildValue(conf, "extended");
    const char *custom = SCConfNodeLookupChildValue(conf, "custom");
    const char *customformat = SCConfNodeLookupChildValue(conf, "customformat");

    /* If custom logging format is selected, lets parse it */
    if (custom != NULL && customformat != NULL && SCConfValIsTrue(custom)) {

        httplog_ctx->cf = LogCustomFormatAlloc();
        if (!httplog_ctx->cf) {
            goto errorfree;
        }

        httplog_ctx->flags |= LOG_HTTP_CUSTOM;

        /* Parsing */
        if ( ! LogCustomFormatParse(httplog_ctx->cf, customformat)) {
            goto parsererror;
        }
    } else {
        if (extended == NULL) {
            httplog_ctx->flags |= LOG_HTTP_DEFAULT;
        } else {
            if (SCConfValIsTrue(extended)) {
                httplog_ctx->flags |= LOG_HTTP_EXTENDED;
            }
        }
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        goto parsererror;
    }

    output_ctx->data = httplog_ctx;
    output_ctx->DeInit = LogHttpLogDeInitCtx;

    SCLogDebug("HTTP log output initialized");

    /* enable the logger for the app layer */
    SCAppLayerParserRegisterLogger(IPPROTO_TCP, ALPROTO_HTTP1);

    result.ctx = output_ctx;
    result.ok = true;
    return result;

parsererror:
    SCLogError("Syntax error in custom http log format string.");
errorfree:
    LogCustomFormatFree(httplog_ctx->cf);
    LogFileFreeCtx(file_ctx);
    SCFree(httplog_ctx);
    return result;

}

static void LogHttpLogDeInitCtx(OutputCtx *output_ctx)
{
    LogHttpFileCtx *httplog_ctx = (LogHttpFileCtx *)output_ctx->data;
    LogCustomFormatFree(httplog_ctx->cf);
    LogFileFreeCtx(httplog_ctx->file_ctx);
    SCFree(httplog_ctx);
    SCFree(output_ctx);
}
