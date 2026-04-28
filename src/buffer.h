#pragma once

typedef struct Buffer {
    char* data;
    int len;
    int cap;
} buffer_t;

int buffer_init(buffer_t* buf, int size);

void buffer_free(buffer_t* buf);

int buffer_reserve(buffer_t* buf, int needed);
