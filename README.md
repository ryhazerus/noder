# Noder

A key-value store over TCP that I'm building to learn C++. It's a hobby project, so expect rough edges.

progress?? huilen. but less than before.

## What it does

- TCP server on port `4321`
- Each request is one line of JSON ending in `\n`
- Keys and values are stored in memory (they're gone when you stop the server)

| command  | example                                          | reply |
|----------|--------------------------------------------------|-------|
| `ADD`    | `{"command":"ADD","key":"foo","value":"bar"}`    | none  |
| `UPDATE` | `{"command":"UPDATE","key":"foo","value":"baz"}` | none  |
| `DELETE` | `{"command":"DELETE","key":"foo"}`               | none  |
| `GET`    | `{"command":"GET","key":"foo"}`                  | `{"command":"GET","found":true,"key":"foo","value":"baz"}` |

You can try it with netcat:

```sh
nc localhost 4321
{"command":"ADD","key":"foo","value":"bar"}
{"command":"GET","key":"foo"}
```

## Building

Needs CMake and a compiler with C++26 support. nlohmann/json is downloaded automatically.

```sh
cmake -S . -B build          # defaults to a Release build
cmake --build build
./build/node_connector
```

Benchmark with a Release build. A debug build is about 2x slower and also prints every request.

## Benchmarks

The load generator is a single-threaded Python script that increases the number of connections step by step. It probably can't push past ~25k ops/s on its own, so the flat part in the middle is likely the script, not the server.

**First version.** Broke at 10 connections, because all clients from `127.0.0.1` shared one buffer and replies went to the wrong socket.

```
  conns      ok      ops/s   p50 ms   p99 ms  conn s connErr   errs  tmout badGET   fail%
      1       1       9784      0.2      0.3     0.0       0      0      0      0   0.00%
     10      10      18442      0.0      0.6     1.1       0      0      9 110104  39.80%
```

**Now.** Clients are identified by `ip:port`, `TCP_NODELAY` is on, `poll` replaced `select`, and the listen backlog is bigger.

```
  conns      ok      ops/s   p50 ms   p99 ms  conn s connErr   errs  tmout badGET   fail%
      1       1       9059      0.2      0.4     0.0       0      0      0      0   0.00%
     10      10      24411      0.9      2.3     0.0       0      0      0      0   0.00%
    100     100      24134     10.2     15.5     0.0       0      0      0      0   0.00%
    500     500      25039     49.4     63.1     0.1       0      0      0      0   0.00%
   1000    1000      25438     96.6    123.5     0.2       0      0      0      0   0.00%
   2000    2000      16337    304.0    640.0     0.3       0      0      0      0   0.00%
   5000    5000       7795   1635.1   3329.5     0.7       0      0     31      0   0.02%
  10000   10000       9346     72.1   4762.4     1.5       0      0   9297      0   6.17%
```

Peak was 25k ops/s at 1000 connections. It starts struggling around 2000 and breaks at 10k.

## Things I learned the hard way

- A `std::string_view` doesn't own anything. Storing views to strings that get freed later = crash.
- An IP address does not identify a connection. Every local client is `127.0.0.1`. Use `ip:port`.
- `select()` only works with file descriptors below 1024. Go past that and the server dies. Use `poll` or `epoll`.
- An exception that escapes a thread calls `std::terminate` and takes down the whole process.
- Nagle's algorithm combined with delayed ACKs adds about 40ms of waiting to small replies. Turn on `TCP_NODELAY`.
- `listen(fd, 5)` means only 5 connections can wait to be accepted. Everyone else waits for TCP retries.
- Printing every connected client on each new connection gets slow (O(n²)) once there are a lot of clients.
- Profile before optimizing. I wanted to rewrite the store, but `perf` showed it uses ~3% of the time. JSON is ~40%, and allocations are another ~28%.

## TODO

- [ ] Reply straight to the client that sent the request (right now every reply searches all clients while holding a global lock)
- [ ] Replace thread-per-connection with an `epoll` event loop, one thread per core
- [ ] Build replies without `nlohmann::json`, or switch to the Redis protocol so I can use `redis-benchmark`
- [ ] Shard the store per thread once that's actually the bottleneck
- [ ] Persistence? maybe. one day.
