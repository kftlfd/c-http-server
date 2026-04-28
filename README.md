# HTTP echo/static-files server

## Features

- pure C, no dependencies
- non-blocking I/O (poll-based event loop, process client until blocked)
- per-client state machine (read -> handle -> write)
- HTTP/1.1 keep-alive support
- basic request parsing (request line + some headers)
- static file serving (GET + HEAD)
- simple routing (/, .html fallback, index.html)
- connection timeouts
- basic pipelining support

## Limitations:

- only HTTP/1.1 (no HTTP/2, no TLS)
- only GET+HEAD supported in fs mode (no POST, etc.)
- no "Host" header validation (HTTP/1.1 requirement)
- no "Transfer-Encoding: chunked"
- no request streaming (entire request buffered)
- no response streaming (entire file buffered in memory)
- no persistent caching (files read on each request)
- limited MIME type detection
- no range requests (partial content)
- no directory listing
- no URL decoding (%20 etc.), no query strings (?a=1)
- strict path validation (may reject some valid URLs)
- no concurrency beyond poll() (single-threaded)
- fixed limits:
  - max clients
  - max request size
  - max file size

## Security notes:

- prevents directory traversal ("..")
- restricts allowed path characters
- follows symlinks via `stat()` (not restricted)
- not hardened for production use

# Setup

## Build

Requirements: `gcc`, `make`

> On Linux/MacOS everything needed is probably already pre-installed

```bash
# compile project
$ make all
```

## Usage

```bash
# echo
$ ./server [echo]

# serve static files
$ ./server fs <path>

# see all options with
$ ./server --help
```

# TODO:

- retry reading files until `size`
- sendfile() / streaming responses for large files
- LRU file cache
- more complete MIME type handling
- configuration (limits, timeouts)
- `writev() + `struct iovec` (`<sys/uio.h>`) for concurrency-safe logs
- don't operate directly on `request.buffer` during parsing
- split Request into `raw_request_t` (buffer + lengths) and `http_request_t` (method, path, headers)
- split `create_fs_response` function
- use `epoll`/`kqueue` instead of `poll`
- unit tests (`munit`)
