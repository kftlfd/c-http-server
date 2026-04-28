#include <stdio.h>      // FILE
#include <string.h>     // strlen, strstr, strrchr
#include <strings.h>    // strcasecmp
#include <sys/stat.h>   // stat()
#include <limits.h>     // PATH_MAX

#include "fs.h"

#define MAX_FILE_SIZE (4 * 1024 * 1024) // 4 MB

int is_valid_path(const char* path) {
    // not empty and starts with "/"
    if (!path || strlen(path) < 1 || path[0] != '/') return 0;

    // allow only safe characters
    for (const char* p = path; *p; p++) {
        char c = *p;
        if (
            !(c >= 'a' && c <= 'z')
            && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9')
            && c != '/'
            && c != '.'
            && c != '-'
            && c != '_'
            ) return 0;
    }

    // reject ".." and double slash
    if (strstr(path, "..") || strstr(path, "//")) return 0;

    return 1;
}

int file_exists(const char* path, int* is_dir) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return 1;
    return 0;
}

int resolve_path(char* out, size_t cap, const char* fs_root, const char* url_path) {
    char path[PATH_MAX];
    int is_dir = 0;
    int n = 0;

    // Case 1: "/"
    if (url_path[0] == '/' && url_path[1] == '\0') {
        n = snprintf(path, sizeof(path), "%s/index.html", fs_root);
        if (n < 0 || (size_t)n >= sizeof(path)) return 0;

        if (file_exists(path, &is_dir) && !is_dir) {
            n = snprintf(out, cap, "%s", path);
            if (n < 0 || (size_t)n >= cap) return 0;
            return 1;
        }

        return 0;
    }

    // build base path, strip leading '/'
    const char* rel = url_path + 1;
    n = snprintf(path, sizeof(path), "%s/%s", fs_root, rel);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    // Case 2: ends with "/"
    size_t len = strlen(url_path);
    if (url_path[len - 1] == '/') {
        char with_index[PATH_MAX];
        n = snprintf(with_index, sizeof(with_index), "%s/index.html", path);
        if (n < 0 || (size_t)n >= sizeof(with_index)) return 0;

        if (file_exists(with_index, &is_dir) && !is_dir) {
            n = snprintf(out, cap, "%s", with_index);
            if (n < 0 || (size_t)n >= cap) return 0;
            return 1;
        }

        return 0;
    }

    // Case 3: try exact file
    if (file_exists(path, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", path);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    // Case 4: try ".html"
    char with_html[PATH_MAX];
    n = snprintf(with_html, sizeof(with_html), "%s.html", path);
    if (n < 0 || (size_t)n >= sizeof(with_html)) return 0;
    if (file_exists(with_html, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", with_html);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    // Case 5: try directory index
    char with_index[PATH_MAX];
    n = snprintf(with_index, sizeof(with_index), "%s/index.html", path);
    if (n < 0 || (size_t)n >= sizeof(with_index)) return 0;
    if (file_exists(with_index, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", with_index);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    return 0;
}

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    ext++; // skip '.'

    if (strcasecmp(ext, "html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";

    return "application/octet-stream";
};

int read_file(const char* path, char** out_buf, int* out_len) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;

    if (!S_ISREG(st.st_mode)) return 0;

    if (st.st_size < 0 || st.st_size > MAX_FILE_SIZE) return 0;

    size_t size = st.st_size;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    char* buf = malloc(size);
    if (!buf) { fclose(f); return 0; }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);

    if (read_bytes != size) { free(buf); return 0; }

    *out_buf = buf;
    *out_len = size;
    return 1;
}
