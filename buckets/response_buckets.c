/* Copyright 2002-2004 Justin Erenkrantz and Greg Stein
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_date.h>

#include "serf.h"
#include "serf_bucket_util.h"


typedef struct {
    serf_bucket_t *stream;
    serf_bucket_t *dechunk;
    serf_bucket_t *headers;     /* holds parsed headers */

    enum {
        STATE_STATUS_LINE,      /* reading status line */
        STATE_HEADERS,          /* reading headers */
        STATE_BODY,             /* reading body */
        STATE_TRAILERS,         /* reading trailers */
        STATE_DONE              /* we've sent EOF */
    } state;

    /* Buffer for accumulating a line from the response. */
    serf_linebuf_t linebuf;

    serf_status_line sl;

    int chunked;
    apr_int64_t body_left;
} response_context_t;


SERF_DECLARE(serf_bucket_t *) serf_bucket_response_create(
    serf_bucket_t *stream,
    serf_bucket_alloc_t *allocator)
{
    response_context_t *ctx;

    ctx = serf_bucket_mem_alloc(allocator, sizeof(*ctx));
    ctx->stream = stream;
    ctx->dechunk = NULL;
    ctx->headers = serf_bucket_headers_create(allocator);
    ctx->state = STATE_STATUS_LINE;

    serf_linebuf_init(&ctx->linebuf);

    return serf_bucket_create(&serf_bucket_type_response, allocator, ctx);
}

static void serf_response_destroy_and_data(serf_bucket_t *bucket)
{
    response_context_t *ctx = bucket->data;

    if (ctx->state != STATE_STATUS_LINE) {
        serf_bucket_mem_free(bucket->allocator, (void*)ctx->sl.reason);
    }

    serf_bucket_destroy(ctx->stream);
    if (ctx->dechunk != NULL)
        serf_bucket_destroy(ctx->dechunk);
    serf_bucket_destroy(ctx->headers);

    serf_default_destroy_and_data(bucket);
}

static apr_status_t fetch_line(response_context_t *ctx, int acceptable)
{
    return serf_linebuf_fetch(&ctx->linebuf, ctx->stream, acceptable);
}

static apr_status_t parse_status_line(response_context_t *ctx,
                                      serf_bucket_alloc_t *allocator)
{
    int res;
    char *reason; /* ### stupid APR interface makes this non-const */

    /* ctx->linebuf.line should be of form: HTTP/1.1 200 OK */
    res = apr_date_checkmask(ctx->linebuf.line, "HTTP/#.# ###*");
    if (!res) {
        /* Not an HTTP response?  Well, at least we won't understand it. */
        return APR_EGENERAL;
    }

    ctx->sl.version = SERF_HTTP_VERSION(ctx->linebuf.line[5] - '0',
                                        ctx->linebuf.line[7] - '0');
    ctx->sl.code = apr_strtoi64(ctx->linebuf.line + 8, &reason, 10);

    /* Skip leading spaces for the reason string. */
    if (apr_isspace(*reason)) {
        reason++;
    }

    /* Copy the reason value out of the line buffer. */
    ctx->sl.reason = serf_bstrmemdup(allocator, reason,
                                     ctx->linebuf.used
                                     - (reason - ctx->linebuf.line));

    return APR_SUCCESS;
}

/* This code should be replaced with header buckets. */
static apr_status_t fetch_headers(serf_bucket_t *bkt, response_context_t *ctx)
{
    apr_status_t status;

    /* RFC 2616 says that CRLF is the only line ending, but we can easily
     * accept any kind of line ending.
     */
    status = fetch_line(ctx, SERF_NEWLINE_ANY);
    if (SERF_BUCKET_READ_ERROR(status)) {
        return status;
    }
    /* Something was read. Process it. */

    if (ctx->linebuf.state == SERF_LINEBUF_READY && ctx->linebuf.used) {
        const char *end_key;
        const char *c;

        end_key = c = memchr(ctx->linebuf.line, ':', ctx->linebuf.used);
        if (!c) {
            /* Bad headers? */
            return APR_EGENERAL;
        }

        /* Skip over initial : and spaces. */
        while (apr_isspace(*++c))
            continue;

        /* Always copy the headers (from the linebuf into new mem). */
        /* ### we should be able to optimize some mem copies */
        serf_bucket_headers_setx(
            ctx->headers,
            ctx->linebuf.line, end_key - ctx->linebuf.line, 1,
            c, ctx->linebuf.line + ctx->linebuf.used - c, 1);
    }

    return status;
}

/* Perform one iteration of the state machine.
 *
 * Will return when one the following conditions occurred:
 *  1) a state change
 *  2) an error
 *  3) the stream is not ready or at EOF
 *  4) APR_SUCCESS, meaning the machine can be run again immediately
 */
static apr_status_t run_machine(serf_bucket_t *bkt, response_context_t *ctx)
{
    apr_status_t status = APR_SUCCESS; /* initialize to avoid gcc warnings */

    switch (ctx->state) {
    case STATE_STATUS_LINE:
        /* RFC 2616 says that CRLF is the only line ending, but we can easily
         * accept any kind of line ending.
         */
        status = fetch_line(ctx, SERF_NEWLINE_ANY);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        if (ctx->linebuf.state == SERF_LINEBUF_READY) {
            /* The Status-Line is in the line buffer. Process it. */
            status = parse_status_line(ctx, bkt->allocator);
            if (status)
                return status;

            /* Okay... move on to reading the headers. */
            ctx->state = STATE_HEADERS;
        }
        break;
    case STATE_HEADERS:
        status = fetch_headers(bkt, ctx);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        /* If an empty line was read, then we hit the end of the headers.
         * Move on to the body.
         */
        if (ctx->linebuf.state == SERF_LINEBUF_READY && !ctx->linebuf.used) {
            const void *v;

            /* Are we C-L, chunked, or conn close? */
            v = serf_bucket_headers_get(ctx->headers, "Content-Length");
            if (v) {
                ctx->chunked = 0;
                ctx->body_left = apr_strtoi64(v, NULL, 10);
                if (errno == ERANGE) {
                    return APR_FROM_OS_ERROR(ERANGE);
                }
            }
            else {
                v = serf_bucket_headers_get(ctx->headers, "Transfer-Encoding");

                /* Need to handle multiple transfer-encoding. */
                if (v && strcasecmp("chunked", v) == 0) {
                    ctx->chunked = 1;
                    ctx->body_left = 0;
                    ctx->dechunk = serf_bucket_dechunk_create(ctx->stream,
                                                              bkt->allocator);
                }
            }
            ctx->state = STATE_BODY;
        }
        break;
    case STATE_BODY:
        /* Don't do anything. */
        break;
    case STATE_TRAILERS:
        status = fetch_headers(bkt, ctx);
        if (SERF_BUCKET_READ_ERROR(status))
            return status;

        /* If an empty line was read, then we're done. */
        if (ctx->linebuf.state == SERF_LINEBUF_READY && !ctx->linebuf.used) {
            ctx->state = STATE_DONE;
            return APR_EOF;
        }
        break;
    case STATE_DONE:
        return APR_EOF;
    default:
        abort();
    }

    return status;
}

static apr_status_t wait_for_body(serf_bucket_t *bkt, response_context_t *ctx)
{
    apr_status_t status;

    /* Keep reading and moving through states if we aren't at the BODY */
    while (ctx->state != STATE_BODY) {
        status = run_machine(bkt, ctx);

        /* Anything other than APR_SUCCESS means that we cannot immediately
         * read again (for now).
         */
        if (status)
            return status;
    }
    /* in STATE_BODY */

    return APR_SUCCESS;
}

SERF_DECLARE(apr_status_t) serf_bucket_response_status(
    serf_bucket_t *bkt,
    serf_status_line *sline)
{
    response_context_t *ctx = bkt->data;
    apr_status_t status;

    if (ctx->state != STATE_STATUS_LINE) {
        /* We already read it and moved on. Just return it. */
        *sline = ctx->sl;
        return APR_SUCCESS;
    }

    /* Running the state machine once will advance the machine, or state
     * that the stream isn't ready with enough data. There isn't ever a
     * need to run the machine more than once to try and satisfy this. We
     * have to look at the state to tell whether it advanced, though, as
     * it is quite possible to advance *and* to return APR_EAGAIN.
     */
    status = run_machine(bkt, ctx);
    if (ctx->state == STATE_STATUS_LINE) {
        *sline = ctx->sl;
    }
    else {
        /* Indicate that we don't have the information yet. */
        sline->version = 0;
    }

    return status;
}

static apr_status_t serf_response_read(serf_bucket_t *bucket,
                                       apr_size_t requested,
                                       const char **data, apr_size_t *len)
{
    response_context_t *ctx = bucket->data;
    apr_status_t rv;

    rv = wait_for_body(bucket, ctx);
    if (rv) {
        return rv;
    }

    if (ctx->dechunk) {
        rv = serf_bucket_read(ctx->dechunk, requested, data, len);
        if (SERF_BUCKET_READ_ERROR(rv))
            return rv;
        if (APR_STATUS_IS_EOF(rv)) {
            ctx->state = STATE_TRAILERS;
        }
        return rv;
    }

    if (requested > ctx->body_left) {
        requested = ctx->body_left;
    }

    /* Delegate to the stream bucket to do the read. */
    rv = serf_bucket_read(ctx->stream, requested, data, len);
    if (SERF_BUCKET_READ_ERROR(rv))
        return rv;

    /* Some data was read, so decrement the amount left. We may be done. */
    ctx->body_left -= *len;
    if (!ctx->body_left && !ctx->chunked) {
        ctx->state = STATE_DONE;
        rv = APR_EOF;
    }
    return rv;
}

static apr_status_t serf_response_readline(serf_bucket_t *bucket,
                                           int acceptable, int *found,
                                           const char **data, apr_size_t *len)
{
    response_context_t *ctx = bucket->data;
    apr_status_t rv;

    rv = wait_for_body(bucket, ctx);
    if (rv) {
        return rv;
    }

    /* ### need to deal with body_left */
    /* Delegate to the stream bucket to do the readline. */
    return serf_bucket_readline(ctx->stream, acceptable, found, data, len);
}

/* ### need to implement */
#define serf_response_read_iovec NULL
#define serf_response_read_for_sendfile NULL
#define serf_response_peek NULL

SERF_DECLARE_DATA const serf_bucket_type_t serf_bucket_type_response = {
    "RESPONSE",
    serf_response_read,
    serf_response_readline,
    serf_response_read_iovec,
    serf_response_read_for_sendfile,
    serf_default_read_bucket,
    serf_response_peek,
    serf_default_get_metadata,
    serf_default_set_metadata,
    serf_response_destroy_and_data,
};
