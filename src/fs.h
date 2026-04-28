#pragma once

#include <stdlib.h> // size_t

int is_valid_path(const char* path);

int resolve_path(char* out, size_t cap, const char* fs_root, const char* url_path);

const char* get_mime_type(const char* path);

int read_file(const char* path, char** out_buf, int* out_len);
