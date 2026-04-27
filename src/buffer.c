#include <stdlib.h>     // NULL
#include <string.h>     // memset
#include <limits.h>     // INT_MAX

#include "buffer.h"

int buffer_init(buffer_t* buf, int size) {
    memset(buf, 0, sizeof(buffer_t));
    buf->data = malloc(size);
    if (!buf->data) return -1;
    buf->len = 0;
    buf->cap = size;
    return 1;
}

void buffer_free(buffer_t* buf) {
    if (buf->data) free(buf->data);
    memset(buf, 0, sizeof(buffer_t));
}

int buffer_reserve(buffer_t* buf, int needed) {
    int new_cap = buf->cap;

    while (new_cap < needed) {
        if (new_cap >= INT_MAX / 2) return -1;
        new_cap *= 2;
    }

    if (new_cap <= buf->cap) return 1;

    char* tmp = realloc(buf->data, new_cap);

    if (tmp == NULL) return -1;

    buf->data = tmp;
    buf->cap = new_cap;
    return 1;
}
