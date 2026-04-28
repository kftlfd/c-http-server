# Building a Simple HTTP Server in C (and Why It Wasn't So Simple)

I didn’t set out to build a “real” HTTP server.

The original goal was much simpler:  
**open a socket, read some bytes, write something back, done.**

That’s it.

Of course, it didn’t stay that simple for long.

---

## The First Version: “It Works”

The very first iteration looked something like this:

1. `socket()`
2. `bind()`
3. `listen()`
4. `accept()`
5. `read()`
6. `write()`

One client at a time, blocking I/O, no parsing—just echo whatever came in.

And technically, that _is_ a server.

You can `curl` it, you can see bytes move around, and it feels like progress. But almost immediately, a few problems show up:

- One slow client blocks everything
- You can’t handle multiple connections
- There’s no structure to requests or responses

So the first shift happens.

---

## From Blocking to Non-Blocking

To support multiple clients, I moved to **non-blocking sockets** and introduced `poll()`.

This changed the entire shape of the program.

Instead of:

```text
accept → read → write → done
```

it became:

```text
event loop → check readiness → act → repeat
```

Now the server:

- tracks multiple clients
- only reads/writes when the OS says it’s ready
- never blocks on a single connection

This is where the project stopped being “just a script in C” and started becoming a **system**.

---

## The State Machine Appears

Once you go non-blocking, you can’t just “read everything” or “write everything” anymore.

Reads and writes become partial.

So each client needs to remember:

- how much has been read
- how much has been written
- what phase it’s in

That’s how this naturally turns into a **state machine**:

```text
READING → HANDLING → WRITING → DONE
```

Each client carries its own state, buffers, and progress.

This was one of the biggest structural turning points:

> instead of “do everything at once”, the server now progresses step-by-step.

---

## “Let’s Parse HTTP (How Hard Can It Be?)”

At some point, echoing raw bytes isn’t enough.

So I added basic HTTP parsing:

- request line (`GET /index.html HTTP/1.1`)
- headers
- `Content-Length`
- `Connection`

That’s when things get subtle.

You realize:

- requests may arrive in chunks
- headers may be incomplete
- bodies may follow later
- clients may pipeline multiple requests

So parsing becomes incremental:

- wait until `\r\n\r\n`
- then parse headers
- then wait for body (if any)

This led to splitting parsing into:

- **“do we have enough data?”**
- **“can we interpret it?”**

Which is a surprisingly important distinction.

---

## Keep-Alive Changes Everything (Again)

HTTP/1.1 defaults to **keep-alive**.

So now:

- one connection can carry multiple requests
- you can’t just close after responding
- buffers must be reused
- leftover data must be preserved

This forced another structural change:

- resetting client state after each request
- shifting remaining bytes in the buffer
- re-entering the state machine without closing the socket

At this point, the server is no longer request-oriented—it’s **connection-oriented**.

---

## Static Files: From Echo to Something Useful

Serving files seems straightforward:

- map URL → filesystem path
- read file
- send it

But even here, details pile up:

- `/` → `index.html`
- `/foo` → `/foo.html` fallback
- `/dir/` → `/dir/index.html`
- MIME types
- file size limits
- directory traversal (`..`)
- invalid paths

Even with a “simple” implementation, this quickly becomes one of the densest parts of the code.

---

## The Accidental Complexity

At this point, the project includes:

- non-blocking I/O
- event loop (`poll`)
- per-client state
- request parsing
- response building
- file system interaction
- connection lifecycle management
- logging

None of these were part of the original plan.

They just… appeared as necessary steps.

---

## Drawing the Line (Avoiding Scope Creep)

There’s a very clear path where this could keep going:

- request/response queues
- streaming (instead of buffering everything)
- `sendfile()`
- caching
- proper HTTP compliance
- chunked encoding
- HTTP/2
- TLS
- thread pools or async I/O (`epoll`, `kqueue`)
- full routing system

At some point, I had to consciously stop.

Not because those things aren’t interesting—but because:

> the goal was to understand the fundamentals, not to reimplement nginx.

So some things are intentionally left out:

- full HTTP compliance
- streaming large files
- advanced concurrency
- complete header handling

---

## What This Project Actually Became

It started as:

> “Can I write a server in C?”

It ended up being:

> “What are the minimal moving parts of a real server?”

And those turned out to be:

- an event loop
- a state machine
- careful buffer management
- incremental parsing
- explicit handling of every edge case

---

## Lessons Learned

### 1. Simplicity is deceptive

“Just read and write” quickly turns into:

- partial reads
- partial writes
- buffering
- retry logic

---

### 2. State is everything

Once you go non-blocking:

- you can’t rely on call stack anymore
- everything must be stored explicitly

---

### 3. Protocols are messy

Even “basic HTTP” includes:

- optional headers
- different connection behaviors
- multiple request lifecycles

---

### 4. Architecture evolves under pressure

None of the abstractions were planned upfront.

They emerged as responses to problems:

- blocking → event loop
- partial I/O → state machine
- pipelining → buffer shifting

---

### 5. Knowing when to stop matters

There’s always “one more feature”.

Stopping before the project becomes unmanageable is part of the discipline.

---

## What I’d Do Next (If I Kept Going)

Some natural next steps would be:

- streaming responses (`sendfile`)
- better request/response separation
- switching from `poll()` to `epoll`/`kqueue`
- more robust HTTP parsing
- basic caching

But each of these would push the project into a different category:
from “learning tool” to “serious server implementation”.

---

## Final Thoughts

This project sits in an interesting place:

- too complex to be called “simple”
- too incomplete to be called “production-ready”

And that’s exactly where it’s useful.

It exposes the _shape_ of real systems without hiding behind frameworks or abstractions.

If nothing else, it answers the original question:

> “What does it actually take to build a server?”

More than expected—but in a good way.
