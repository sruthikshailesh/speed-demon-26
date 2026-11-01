# Speed Demon 2026: Track B (Index)

Answers boolean and phrase queries over a text corpus. C++17, standard library and pthreads only, no dependencies.

Usage: `runner /data /out` reads `docs.txt` and `queries.txt` and writes `results.txt`.

## Approach

**Read the queries first, index only what they need.** The queries are available up front, so I collect the distinct terms they mention into a hash table before touching the corpus. The index then holds postings only for those terms, and token positions only for terms that appear in a phrase query. This keeps both build time and memory small.

**Two-pass parallel CSR index.**
- The corpus is memory-mapped and split into line-aligned chunks, one per thread.
- Pass 1 tokenizes each chunk once. It counts document frequency and occurrences per term (in per-thread counters, so no atomics) and records each hit as a packed (term, position) word.
- A prefix sum turns the counts into exact offsets for every term and thread.
- Pass 2 replays the recorded words and scatters them into flat arrays (postings, positions, position starts). There are no per-term vectors and no reallocation, and the corpus is never tokenized twice.

**Queries.**
- BOOL queries are evaluated strictly left to right, with NOT as a binary difference. A run of the same AND or OR operator is associative, so those operands are processed smallest posting list first.
- Intersections use a binary search when one list is much smaller, and an 8-wide SIMD block compare (SSE2/NEON) otherwise.
- PHRASE queries first intersect the documents containing all terms (smallest list first), then check consecutive positions with binary search, anchored on the rarest term.
- Queries are split into contiguous ranges across threads, and results are written in query order, so output is deterministic.

**Tokenization follows the spec:** a token is a maximal run of ASCII `[a-z0-9]` after ASCII lowercasing, and every other byte (including bytes >= 0x80) is a separator.

## Why these choices

- **Query-driven indexing instead of a full index:** the work is bounded by the query vocabulary, not the whole corpus vocabulary, so build time and memory drop.
- **CSR arrays instead of per-term vectors:** exact memory, contiguous scans, and no allocator traffic.
- **One tokenization pass:** the recorded stream replaces a second parse of the corpus.
- **Smallest-list-first ordering:** cheapest way to shrink candidate sets early.

## Correctness

- Output was compared with the official reference using `check.py` (SHA-256 match).
- Edge cases handled: terms absent from the corpus, empty results, NOT with identical operands, repeated terms, phrases longer than one term, very long tokens (fallback path), a single-word phrase, and queries with a single term.
- Output is deterministic across runs and thread counts.

## Build and run

Offline multi-stage Docker build. The program reads `/data`, writes only `results.txt` to `/out`, and exits 0.

```
docker build --network=none -t track-b .
python3 check.py b --data <data dir> --expected <sha256>
```

## Dependencies

None.

## With another 24 hours

- [e.g. compress posting lists (delta + varint) to cut memory]
- [e.g. cache results of repeated sub-expressions across queries]
- [e.g. tune thread count and chunk sizes on the live board]

See `PERF.md` for the optimisation log.
