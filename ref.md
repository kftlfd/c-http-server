# 🚀 Next Big Step — Stateful Clients & State Machines

This is where your server evolves from:

> “handles one request per connection”

to:

> “handles real-world network behavior correctly”

---

# 🧠 Why your current model is limiting

Right now:

```text
poll → handle_client → read whole request → respond → close
```

Problems:

* You assume the request arrives in one go ❌
* You block logic inside `handle_client` ❌
* You can’t support:

  * partial reads
  * keep-alive
  * slow clients

---

# 💡 The Core Idea

Instead of:

```text
function handles entire request
```

You move to:

```text
each client has STATE
each poll cycle advances that state
```

---

# 🧩 What is a “state machine”?

A **state machine** means:

Each client is always in one of these states:

```text
READING_HEADERS
READING_BODY
WRITING_RESPONSE
DONE
ERROR
```

And your server loop does:

```text
for each client:
    depending on state → do next step
```

---

# 🧱 Step 1 — Define client structure

Right now you store only:

```c
struct pollfd
```

You need something like:

```c
typedef enum {
    STATE_READING,
    STATE_WRITING,
    STATE_DONE,
    STATE_ERROR
} client_state_t;

typedef struct {
    int fd;

    client_state_t state;

    char *buffer;
    int buffer_len;
    int buffer_cap;

    int headers_done;
    int content_length;

    int response_sent;
    char *response;
    int response_len;
} client_t;
```

---

## 🧠 Why each field exists

### `state`

Controls what to do next:

* read?
* write?
* close?

---

### `buffer`, `buffer_len`, `buffer_cap`

You now:

* accumulate incoming data **across multiple reads**

Because network input is **chunked unpredictably**

---

### `headers_done`

You don’t know upfront when headers end.

So you:

* scan buffer for `\r\n\r\n`
* flip this flag when found

---

### `content_length`

Needed to know:
👉 how much body to expect

---

### `response` fields

Because:

* `write()` may be partial
* you need to resume sending later

---

# 🔄 Step 2 — Change your poll loop model

## ❌ Current

```text
if readable:
    handle_client(fd)
    close(fd)
```

---

## ✅ New model

```text
if readable:
    read_into_client_buffer()

if writable:
    write_from_client_buffer()

if done/error:
    cleanup client
```

---

# 🔧 Step 3 — Modify poll usage

You must dynamically change what you watch:

```c
pollfds[i].events = POLLIN;   // when reading
pollfds[i].events = POLLOUT;  // when writing
```

---

# 🔁 Step 4 — Reading becomes incremental

Instead of:

```c
read_request(fd, req)
```

You do:

```c
read(fd, client->buffer + offset, ...)
```

Then:

### After each read:

1. Check:

   ```c
   strstr(buffer, "\r\n\r\n")
   ```
2. If headers complete:

   * parse `Content-Length`
3. If full body received:

   * build response
   * switch state → `STATE_WRITING`

---

# ✍️ Step 5 — Writing becomes incremental

Instead of:

```c
write_all(...)
```

You do:

```c
write(fd, response + sent, remaining)
```

Update:

```c
client->response_sent += n;
```

When done:

```text
STATE_DONE
```

---

# 🧹 Step 6 — Cleanup

When:

```text
STATE_DONE or STATE_ERROR
```

→ close fd
→ free buffers
→ remove from poll array

---

# 🧠 Why this is better

### Before:

* One blocking flow
* Assumes perfect network

### After:

* Fully event-driven
* Handles:

  * slow clients
  * partial packets
  * real TCP behavior

---

# ⚠️ Important mindset shift

You are moving from:

> “function processes request”

to:

> “event loop progresses client state”

That’s the core leap.

---

# 🧭 What changes you’ll actually make

### You will:

1. Replace `handle_client()` with:

   * `handle_read(client_t*)`
   * `handle_write(client_t*)`

2. Store `client_t` alongside `pollfd`

3. Stop closing connection immediately

4. Remove `read_request()` (it no longer fits)

---

# 🚀 Suggested next step

Don’t try to rewrite everything at once.

Start with:

### Phase 1

* Introduce `client_t`
* Keep:

  * only header reading
  * no body yet

### Phase 2

* Add body handling

### Phase 3

* Add response writing state
