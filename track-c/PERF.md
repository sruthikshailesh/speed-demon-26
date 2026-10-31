# PERF.md: Track C (Join)

Team: Team Voltex
Language: C++17, g++ -O3 -flto, static build
Dependencies: 0 (standard library only)

## Baseline

| Run ID | What | Time | Memory |
|--------|------|------|--------|
| <id> | Unchanged Python reference (reference/c_join.py) | <s> | <MB> |

## Optimisation log

| # | Run ID | Change | Why | Time before | Time after | Memory | Kept? |
|---|--------|--------|-----|-------------|------------|--------|-------|
| 1 | <id> | C++ rewrite, mmap input, threaded range parsing | Python interpreter and single thread dominate | <s> | <s> | <MB> | Yes |
| 2 | <id> | Shared lock-free name table (replaces a private table per thread) | No per-thread copy to merge later | <s> | <s> | <MB> | <Yes/No> |
| 3 | <id> | Shared per-country bitmap with relaxed atomic OR (replaces a private bitmap per thread) | Lower peak RSS | <s> | <s> | <MB> | <Yes/No> |
| 4 | <id> | Name lookups batched 32 at a time | Overlap table misses | <s> | <s> | <MB> | <Yes/No> |
| 5 | <id> | SWAR quote and digit scanning | Avoid per-byte loops | <s> | <s> | <MB> | <Yes/No> |
| 6 | <id> | Cheaper name hash (first and last 8 bytes plus length) | FNV-1a is a serial dependency chain | <s> | <s> | <MB> | <Yes/No> |
| 7 | <id> | Spinlock instead of std::mutex for inserts | Avoid futex wake-ups | <s> | <s> | <MB> | <Yes/No> |

Fill each row from a real board run. Delete rows you didn't actually test.

## What did not work

- <attempt>: <what happened and what you learned>

## Correctness

- Output matches the reference on seed 1, sample and full scale.
- Edge cases from specs/c-join.md checked: quoted commas, "" escapes, embedded LF/CRLF, BOM at start and mid-file, mixed CRLF/LF, unmatched regions.

## Final result

- Best board run: <id>, <time>, <memory>, <points>
- Speedup vs reference: <reference time / your time>
- Next steps: <ideas>
