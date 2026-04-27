#include <stdlib.h>     // NULL
#include <stdio.h>      // sscanf, snprintf
#include <string.h>     // strstr, strcmp
#include <strings.h>    // strcasecmp

#include "buffer.h"
#include "http.h"

#define MAX_HEADERS_SIZE (16 * 1024)   // 16 KB
#define MAX_HEADERS_COUNT 100
#define MAX_HEADER_LINE 2048

static int next_request_id = 1;

const char* connection_str(conn_t conn) {
    return conn == CONN_CLOSE ? "close" : "keep-alive";
}

// ----------------------------------------------
// Parse request
// ----------------------------------------------

/**
 * Parse request line, headers, populate `client->request`
 */
parse_result_t parse_request_headers(request_t* req) {
    buffer_t* buf = &req->buffer;

    /**
     * Parse request line
     */
    char* line_end = strstr(buf->data, "\r\n");
    if (!line_end) return PARSE_INCOMPLETE;

    *line_end = '\0';

    if (sscanf(buf->data,
        "%7s %1023s %15s",
        req->method,
        req->path,
        req->version
    ) != 3) return PARSE_ERROR;

    *line_end = '\r';

    req->connection = CONN_CLOSE;
    if (strcmp(req->version, "HTTP/1.1") == 0) {
        req->connection = CONN_KEEP_ALIVE;
    }

    /**
     * Parse headers
     */
    char* p = line_end + 2;
    char* headers_end = buf->data + req->headers_len;

    req->body_len = 0;

    int headers_count = 0;

    while (p < headers_end - 2) {
        char* next = strstr(p, "\r\n");
        if (!next) return PARSE_ERROR;

        if (next == p) break; // empty line

        headers_count++;
        if (headers_count > MAX_HEADERS_COUNT) return PARSE_ERROR;

        *next = '\0';

        char key[MAX_HEADER_LINE];
        char value[MAX_HEADER_LINE];

        if (sscanf(p, "%[^:]: %2047[^\r\n]", key, value) == 2) {

            if (strcasecmp(key, "content-length") == 0) {
                char* end;
                long len = strtol(value, &end, 10);
                if (*end != '\0' || len < 0 || len > MAX_REQUEST_SIZE) return PARSE_ERROR;
                req->body_len = (int)len;
            }
            else if (strcasecmp(key, "connection") == 0) {
                if (strcasecmp(value, "keep-alive") == 0) {
                    req->connection = CONN_KEEP_ALIVE;
                }
                else if (strcasecmp(value, "close") == 0) {
                    req->connection = CONN_CLOSE;
                }
            }
            else if (strcasecmp(key, "transfer-encoding") == 0) {
                return PARSE_NOT_SUPPORTED; // not supported
            }
        }

        *next = '\r';
        p = next + 2;
    }

    return PARSE_OK;
}

parse_result_t try_parse_request(request_t* req) {
    buffer_t* buf = &req->buffer;

    if (req->headers_len > 0) {
        int body_received = buf->len - req->headers_len;
        if (body_received < req->body_len) return PARSE_INCOMPLETE;

        return PARSE_OK;
    }

    char* headers_end = strstr(buf->data, "\r\n\r\n");

    if (!headers_end) {
        if (buf->len > MAX_HEADERS_SIZE) return PARSE_ERROR;
        return PARSE_INCOMPLETE;
    }

    req->headers_len = headers_end + 4 - buf->data;
    req->id = next_request_id++;

    return parse_request_headers(req);
}

// ----------------------------------------------
// Build response
// ----------------------------------------------

int response_build(
    response_t* res,
    http_status_t status_code,
    const char* status_text,
    const char* content_type,
    const char* body,
    int body_len,
    conn_t connection
) {
    int header_cap = 1024;
    int total_cap = header_cap + body_len;
    if (!buffer_init(&res->buffer, total_cap)) return 0;

    int header_len = snprintf(
        res->buffer.data,
        header_cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status_code,
        status_text,
        content_type ? content_type : "application/octet-stream",
        body_len,
        connection_str(connection)
    );

    if (header_len < 0 || header_len >= header_cap) {
        buffer_free(&res->buffer);
        return 0;
    }

    if (body_len > 0 && body != NULL) {
        memcpy(res->buffer.data + header_len, body, body_len);
    }

    res->buffer.len = header_len + body_len;
    res->sent = 0;
    res->status_code = status_code;
    res->connection = connection;

    return 1;
}

int create_error_response(response_t* res, http_status_t code) {
    const char* text = "Internal Server Error";

    switch (code) {
    case HTTP_400_BAD_REQUEST: text = "Bad Request"; break;
    case HTTP_403_FORBIDDEN: text = "Forbidden"; break;
    case HTTP_404_NOT_FOUND: text = "Not Found"; break;
    case HTTP_405_NOT_ALLOWED: text = "Method Not Allowed"; break;
    case HTTP_501_NOT_IMPLEMENTED: text = "Not Implemented"; break;
    default: break;
    }

    return response_build(
        res,
        code,
        text,
        "text/plain",
        NULL,
        0,
        1 // always close on error
    );
}
