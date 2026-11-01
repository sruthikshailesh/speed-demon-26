# Track D performance notes

## Measurement conditions

All timings here are from the provided seed-1 full graph and query set, with
byte-identical output hash `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e`.
Measurements ran locally on macOS ARM64 with Apple Clang 21.0.0. This is not the
judging host (an Intel i5-13500), and the local machine's CPU model was not
available. Treat these as comparisons between variants on this host, not as
predictions of official T or M.

Phase times are monotonic-clock instrumentation in milliseconds. `parse`
includes input mapping, token parsing, and degree counting; `CSR` includes
prefix sums and edge placement; `BFS` is the locality reorder; `queries` is
shortest-path processing; `output` includes decimal formatting and writing.
RSS is process peak resident memory in MiB from macOS `ru_maxrss`. Instrumented
phase runs and uninstrumented RSS runs are identified separately. Values shown
as `—` were not collected; they are not estimates. Single-run phase values are
noisy, and instrumentation adds a small amount of overhead.

## Correct baseline and CSR builder comparison

| Variant | Workers | Parse | CSR | BFS | Queries | Output | Total | Peak RSS | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Sequential CSR + `std::priority_queue` baseline, phase medians across 5 runs | 1 | 86.4 | 101.5 | — | 5,842.4 | 0.3 | 6,003.0 | 168.9 | Correct baseline; too slow |
| Direct parallel two-pass CSR, one instrumented run | 12 | 26.0 | 28.5 | — | 697.6 | ~0.3 | 752.4 | 307.8 | Correct and fast over baseline; higher parse/build cost than buffered buckets |
| Buffered per-worker edge buckets + CSR, v12 phase medians across 3 runs | 1 | 70.3 | 35.8 | 75.6 | 870.7 | ~0.2 | 1,052.6 | 222.2 | Kept as the basis for the optimized path |
| Same v12 implementation | 2 | 36.8 | 20.5 | 50.0 | 445.2 | ~0.2 | 552.9 | 267.1 | — |
| Same v12 implementation | 4 | 26.0 | 14.7 | 41.6 | 241.8 | ~0.1 | 324.2 | 281.1 | — |
| Same v12 implementation | 6 | 19.1 | 12.8 | 32.3 | 179.9 | ~0.2 | 244.3 | 290.8 | — |
| Same v12 implementation | 12 | 14.9 | 8.6 | 24.8 | 137.6 | ~0.3 | 186.2 | 317.4 | Fastest v12 worker setting |

The direct two-pass builder counts edges, prefixes, then reparses the input to
place them. The retained builder parses once into thread-local edge buckets,
then compacts those buckets into CSR. On the measured v12 build, its parse plus
CSR phases were 23.5 ms at 12 workers, compared with 54.5 ms for direct
two-pass CSR. Direct two-pass CSR used about 9.6 MiB less peak RSS in that
comparison, but took longer overall and did not include the later locality and
query improvements. The final version also unmaps input files promptly and
reduced the BFS queue reservation, lowering observed peak RSS.

CSR was retained because contiguous offsets, destinations, and weights give
compact storage and predictable adjacency scans. The parallel BFS relabel is
worth its preprocessing cost: disabling it more than doubled the measured
query phase.

## Priority queue and shortest-path experiments

The following heap comparison used the same full dataset and v5-family graph
preparation. Phase values are one instrumented run per variant; outer wall and
RSS are medians from three uninstrumented runs. This isolates the heap change
better than comparing unrelated end-to-end versions.

| Heap | Workers | Parse | CSR | BFS | Queries | Output | Instrumented total | Outer wall median (range) | Peak RSS | Decision |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `std::priority_queue` with packed keys | 12 | 25.5 | 27.7 | 89.3 | 209.0 | ~0.2 | 351.7 | 0.364 s (0.358–0.920) | 352.2 | Rejected |
| Packed binary STL heap | 12 | 25.1 | 28.3 | 93.4 | 221.4 | ~0.2 | 368.4 | 0.377 s (0.373–0.807) | 352.9 | Rejected |
| Custom 4-ary heap | 12 | 25.0 | 28.1 | 89.0 | 214.4 | ~0.3 | 356.8 | 0.370 s (0.357–0.783) | 352.9 | Rejected |
| Monotone radix heap | 12 | 25.5 | 28.7 | 92.4 | 159.8 | ~0.2 | 306.6 | 0.317 s (0.317–0.827) | 356.6 | Kept |

The radix heap reduced the measured query phase by about 23% versus the packed
`std::priority_queue` variant. Its small RSS increase was acceptable. Packed
64-bit `(distance, vertex)` keys are used on the normal `uint32_t` pass; groups
that encounter an overflowing candidate are rerun with `uint64_t` distances.

Bidirectional Dijkstra with a reverse CSR was also measured: parse 25.3 ms,
forward CSR 10.8 ms, BFS 26.3 ms, reverse CSR 15.7 ms, queries 362.2 ms,
output 0.2 ms, total 440.6 ms, and peak RSS 477.0 MiB (one instrumented full
run). It was rejected because it was slower than the one-sided grouped-source
radix search and substantially increased memory.

Grouping queries by source was retained. An eight-run uninstrumented A/B gave
0.198 s median for grouped queries versus 0.200 s for one search per query;
the difference was within run-to-run noise. Grouping still avoids repeated
searches for shared sources and permits stopping once every requested target
settles. The one-per-query instrumented run measured parse 26.5 ms, CSR 10.8
ms, BFS 30.0 ms, queries 136.7 ms, output 0.2 ms, total 204.2 ms, and 220.0
MiB RSS. The grouped final profile is reported below.

An in-degree-zero target shortcut (with `source == target` handled first) and
an out-degree-zero source shortcut were retained. Disabling the in-degree
shortcut measured parse 26.5 ms, CSR 7.8 ms, BFS 29.7 ms, queries 164.7 ms,
output 0.3 ms, total 229.0 ms, and 252.8 MiB RSS. This also avoids graph
searches that cannot produce a path.

## Final implementation and worker counts

These phase values are one instrumented run per worker count of the final
implementation. Peak RSS is from a separate uninstrumented three-run sweep of
the final source. The one-run timing at 12 workers was affected by host
variance; the separate repeated timings below are a better summary of T.

| Workers | Parse | CSR | BFS | Queries | Output | Instrumented total | Peak RSS, separate 3-run sweep |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 190.3 | 45.8 | 76.4 | 891.4 | 0.2 | 1,204.0 | 146.3 MiB |
| 2 | 42.0 | 20.8 | 49.1 | 539.7 | 0.2 | 651.8 | 179.6 MiB |
| 4 | 37.2 | 15.9 | 38.1 | 275.8 | 0.2 | 367.3 | 185.5 MiB |
| 6 | 25.3 | 11.8 | 43.1 | 199.4 | 0.2 | 279.8 | 204.4 MiB |
| 12 | 25.0 | 8.5 | 28.7 | 200.9 | 0.2 | 263.2 | 219.7 MiB |

For final source, an uninstrumented five-run 12-worker set had raw wall times
of 0.212, 0.206, 0.193, 0.205, and 0.217 seconds. The middle-three mean was
0.207 seconds, with 224.9 MiB peak RSS. A separate three-run worker sweep gave
medians/ranges of 1.386 s (1.271–1.397) at 1 worker, 0.704 s (0.683–0.716) at
2, 0.445 s (0.413–0.456) at 4, 0.325 s (0.325–0.336) at 6, and 0.263 s
(0.260–0.266) at 12. The latter run set was slower than the five-run set,
consistent with scheduling or thermal noise; 12 workers was fastest in both.

## Container contract run

The official `harness/run_submission.py` was run for five full-seed-1 runs.
The submission image built successfully with `docker build --network=none`
from the locally cached pinned base images. The final arm64 image was
140,917,570 bytes. The official harness passed its correctness, timeout, and
sandbox gates; all runs exited 0, matched the published output hash, and had
an empty `docker diff` outside the allowed mounts.

| Run | Timer wall (s) | Kernel `memory.peak` (bytes) |
|---:|---:|---:|
| 1 | 0.280487 | 206,135,296 |
| 2 | 0.266404 | 206,524,416 |
| 3 | 0.257456 | 206,544,896 |
| 4 | 0.273439 | 206,581,760 |
| 5 | 0.348351 | 206,528,512 |

The harness dropped the lowest and highest times, giving **T = 0.273443 s**;
**M = 206,581,760 bytes (197.0 MiB)**, the highest of the five memory peaks.
The batch spread gate passed. This ran in Docker Desktop's Linux arm64 VM with
10 available CPUs pinned as `0-9`, not on the scoring i5-13500; it is a local
contract check, not an official score measurement.

The harness's normal cgroup reader expects Linux `/sys/fs/cgroup` on the host,
which macOS cannot access directly. For this run, only that reader command was
redirected into a temporary privileged helper container in the Docker VM,
using `--network=none`, `--pull=never`, and `--cgroupns=host`. It read the same
run-specific `memory.peak` file and removed the completed cgroup; the official
build, timer, run contract, output/hash checks, and filesystem-diff checks were
unchanged. The helper image had to be pulled separately because the local
cache contained the pinned Debian base only by digest without the helper tag;
the submission build and the harness containers themselves used no network.

Additional final-source ablations:

| Experiment | Parse | CSR | BFS | Queries | Output | Total | Peak RSS | Finding |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| BFS locality reorder disabled, one profile | 23.2 | 9.1 | 0.6 | 303.3 | 0.2 | 336.4 | 202.1 MiB (max of 8 uninstrumented runs) | Rejected; query phase rose from roughly 140 ms to 303 ms, outweighing the saved 28 ms preprocessing |
| Generation-counter distance arrays, one profile | 25.1 | 8.6 | 27.0 | 149.3 | 0.2 | 210.2 | 283.4 MiB | Rejected; interleaved 12-run A/B was slightly slower (0.199 vs 0.197 s median) and used about 59 MiB more than the touched-node list |
| Final touched-node distance reset, phase medians | 18.0 | 8.3 | 28.1 | 140.3 | 0.2 | 198.8 | 229.1 MiB instrumented | Kept; lower memory and slightly faster in the interleaved A/B |

The locality reorder assigns vertices in parallel BFS order, then rewrites CSR
arcs. The benefit comes from improved graph locality during Dijkstra. Per
worker distance arrays are memory-mapped lazily and reset only at touched
vertices; generation counters were tested and were both slower and larger on
this host. Padded relaxed-atomic job cursors limit false sharing while workers
take independent source groups.

## Bottlenecks and limits

On the final 12-worker profile, query processing remained the largest single
phase, while parsing and CSR construction were each below 35 ms combined in
the repeated profile. Output was about 0.2 ms and is not a meaningful
bottleneck. Local results favor 12 workers, but the official host has a
different CPU and limits; its optimum and peak cgroup memory may differ from
the local Docker contract run above.
