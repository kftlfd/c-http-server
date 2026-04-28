#pragma once

#include "buffer.h"

#define MAX_REQUEST_SIZE (1024 * 1024) // 1 MB

typedef enum HttpStatus {
    HTTP_200_OK = 200,
    HTTP_400_BAD_REQUEST = 400,
    HTTP_403_FORBIDDEN = 403,
    HTTP_404_NOT_FOUND = 404,
    HTTP_405_NOT_ALLOWED = 405,
    HTTP_500_INTERNAL_ERROR = 500,
    HTTP_501_NOT_IMPLEMENTED = 501
} http_status_t;

typedef enum ParseResult {
    PARSE_INCOMPLETE,
    PARSE_OK,
    PARSE_ERROR,
    PARSE_NOT_SUPPORTED
} parse_result_t;

typedef enum Connection {
    CONN_CLOSE,
    CONN_KEEP_ALIVE
} conn_t;

typedef struct Request {
    int id;
    buffer_t buffer;

    int headers_len;
    int body_len;

    char method[8];
    char path[1024];
    char version[16];
    conn_t connection;
} request_t;

typedef struct Response {
    buffer_t buffer;
    int sent;

    int status_code;
    conn_t connection;
} response_t;

const char* connection_str(conn_t conn);

parse_result_t try_parse_request(request_t* req);

int response_build(
    response_t* res,
    http_status_t status_code,
    const char* status_text,
    const char* content_type,
    const char* body,
    int body_len,
    conn_t connection
);

int create_error_response(response_t* res, http_status_t code);
