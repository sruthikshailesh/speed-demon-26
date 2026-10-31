# Speed Demon 2026: Track A (Ingest)

Team: <team name>

## Approach

A single C++17 program (`src/main.cpp`) reads the request logs from `/data`
and writes `summary.txt` to `/out`. It computes:

- total requests and total errors, with the overall error rate
- error rate per hour
- a latency table per hour and endpoint: count, p50, p95, p99, errors and error_ppm

On the full public dataset (3,000,000 requests) the output matches the
reference's sha256 byte for byte.

- Input: <e.g. mmap / one large read / buffered fread>
- Parsing: <e.g. hand-written parser, no iostreams>
- Aggregation: <e.g. flat arrays indexed by hour and endpoint>
- Percentiles: <e.g. exact via nth_element, or histogram buckets>
- Parallelism: <threads used and how work is split, or "single-threaded">
- Output: <how the formatting is matched to the reference exactly>

## Results

Local run on a MacBook Air, full dataset:

| Metric | Value |
|--------|-------|
| Wall time | 1.560 s |
| User CPU | 0.240 s |
| Sys CPU | 0.020 s |
| Peak RAM | 87.31 MB |
| Throughput | about 1.92M requests/s |

Board run: <run ID>, <time>, <memory>.
Reference (Python) for comparison: <time>.

## Build

Two-stage Docker build, fully offline. An allowlisted gcc image, pinned by
digest, compiles a static binary with `-O3 -std=c++17`. The binary is copied
into `debian:13-slim`, which is also pinned by digest.

## Dependencies

None. Standard library only.

## Testing

```
python3 check.py a --data data/a --expected <sha256>
```

## Next 24 hours

- Profile where the wall time goes. CPU time is only about 0.26 s of the
  1.56 s wall time, so input reading is the first suspect.
- <reduce peak memory>
- <parallelise parsing if not done yet>
- Re-verify the sha256 on the full dataset after every change.
