# Noder 
learning c++ by building this thing...

progress??
huilen.

I did some kind of benchmark for this. Context single threaded python script that does stages of requests per second and connections scaling.

Results:
```sh
➜  tcp_sender ./load_test.py --verify
Target 127.0.0.1:4321  reply=get  verify=True  value<=512B  15s/stage

  conns      ok      ops/s   p50 ms   p99 ms  conn s connErr   errs  tmout badGET   fail%  error types
      1       1       9784      0.2      0.3     0.0       0      0      0      0   0.00%  -
     10      10      18442      0.0      0.6     1.1       0      0      9 110104  39.80%  -

BROKE at 10 connections (failure rate 39.80%); last healthy stage: 1.
Peak throughput: 18442 ops/s at 10 connections.

```