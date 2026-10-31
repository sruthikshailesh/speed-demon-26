# PERF.md: Track A (Ingest)

Team: <team name>
Language: C++17, g++ -O3, static build
Dependencies: 0 (standard library only)

## Baseline

| Run ID | What | Time | Memory |
|--------|------|------|--------|
| <id> | Unchanged Python reference | <s> | <MB> |

## Optimisation log

| # | Run ID | Change | Time | Peak RAM | Notes |
|---|--------|--------|------|----------|-------|
| 1 | <board id> | C++17 rewrite, -O3, static | 1.560 s (local) | 87.31 MB | 3,000,000 requests, 1.92M req/s |

Local run, full dataset: wall 1.560 s, user CPU 0.240 s, sys CPU 0.020 s.

## Observations

- CPU time (0.26 s) is much lower than wall time (1.56 s), at 16.67% CPU. The run is mostly waiting, likely on file I/O, so reading input is the next thing to look at.
- <what you tried next and what it changed>

## What did not work

- <attempt>: <why it didn't help>

## Final result

- Best board run: <id>, <time>, <memory>, sha256 matches the reference
- Speedup vs reference: <reference time / your time>
