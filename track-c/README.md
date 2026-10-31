# Speed Demon 2026: Track C (Join)

Team: Team Voltex
Language: C++17 (g++ -O3 -flto, static), standard library only, 0 dependencies

## Approach

One C++ program (`src/main.cpp`) joins `records.csv` to `regions.csv` and writes
`aggregates.txt`: per country (or `_UNMATCHED`) the record count, id sum, exact
amount sum/min/max in integer cents, distinct name count, name bytes and note bytes.
Output is sorted bytewise. No floating point is used anywhere.

- **Input:** `records.csv` is mmapped read-only, with no copy. It is cut into byte ranges, one per thread (8 threads, fewer on small machines).
- **Hostile CSV:** record boundaries are found by quote parity, so a newline counts only at even quote parity. Quoted commas, `""` escapes, embedded LF/CRLF, a BOM at the file start and at record boundaries, and mixed terminators are handled.
- **Parsing:** pointer arithmetic with 8-byte SWAR scans for quotes and digits. Amounts are parsed to exact integer cents.
- **Join:** `regions.csv` is loaded on a helper thread while the first pass counts quotes. A flat lookup table maps region id to country.
- **Distinct names:** one shared lock-free open-addressing name table. Hits are plain acquire loads. Only first sightings take a spinlock. A shared per-country bitmap counts distinct names with relaxed atomic ORs.
- **Batching:** name lookups are done 32 records at a time so table misses overlap.
- **Accumulators:** per-thread per-country, merged in parallel at the end.

## Correctness

Output matches the reference sha256 byte for byte on seed 1, at sample and full scale
(`cmp` against `reference/c_join.py`).

## Local results (MacBook Air, seed 1, full scale)

| Metric | Value |
|--------|-------|
| Wall time | 0.05 s |
| User CPU | 0.32 s |
| Peak memory footprint | 20 MB |

The maximum resident set size on macOS (222 MB) counts the mapped input file's pages.

Board run: <run ID>, <time>, <memory>, <points>

## Build

Two-stage Docker build, offline. An allowlisted gcc image, pinned by digest, builds a
static binary (`-O3 -std=c++17 -DNDEBUG -flto -pthread -static -s`, plus
`-march=x86-64-v3` on x86_64). The binary runs in `debian:13-slim`, also pinned.

## Dependencies

None. Standard library only.

## Next 24 hours

- <e.g. tune the thread count on the board's 6 P-cores>
- <e.g. check peak RSS on the board and reduce it>
- Re-verify the output hash after every change.
