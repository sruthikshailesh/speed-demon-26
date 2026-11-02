# Track B — Performance Optimization Journey

## 1. Overview

**Track:** B — Index  
**Language:** C++  
**Build configuration:** `-O3 -std=c++17 -DNDEBUG -march=native -flto -pthread -static -s`  
**Input files:** `docs.txt`, `queries.txt`  
**Output file:** `results.txt`

### Objective

Build an efficient search index over a document collection and execute Boolean and phrase queries.

The implementation must support:

- ASCII case-insensitive tokenization.
- ASCII alphanumeric token recognition.
- Non-ASCII bytes as token separators.
- Boolean AND, OR and NOT operations.
- Strict left-to-right Boolean evaluation.
- Phrase matching using consecutive token positions.
- Exact document counts.
- Sum of matching document IDs.
- The ten smallest matching document IDs.

The main challenge is to minimize indexing time, memory consumption and query execution time without changing the search semantics.

**Optimization direction:** Index only the information required by the queries, build the index in parallel and use compact posting-list structures for efficient query execution.

---

## 2. Optimization Journey

### Stage 1 — Understanding the workload

The first step was examining the relationship between the document collection and the query set.

A conventional search engine might index every token and retain positional information for every occurrence.

However, this workload provides the queries before the document index is constructed.

That creates an opportunity to avoid storing unnecessary information.

The main challenges were:

1. Tokenizing a large document collection.
2. Identifying relevant query terms.
3. Building document posting lists.
4. Supporting phrase queries with positional information.
5. Executing Boolean operations efficiently.
6. Reducing index memory consumption.
7. Parallelizing indexing and query execution.

**Optimization direction:** Build a query-specific inverted index rather than a general-purpose full-text index.

### Stage 2 — Query-first indexing

**Problem:** Indexing every token in every document wastes memory when only a small subset of terms is referenced by the queries.

**Optimization:** Read and parse `queries.txt` before processing `docs.txt`.

The implementation:

1. Reads all queries.
2. Extracts the terms appearing in those queries.
3. Assigns each unique term a dense integer ID.
4. Records the operators used by Boolean queries.
5. Identifies which terms require positional information.

A `need_pos` array tracks whether a term occurs in at least one phrase query.

**Why it helps:**
- Avoids indexing irrelevant terms.
- Reduces the number of posting lists.
- Reduces positional storage.
- Makes index construction more memory-efficient.

This is one of the most important architectural optimizations in Track B.

### Stage 3 — Compact term dictionary

**Problem:** Repeatedly storing and comparing query-term strings during indexing introduces unnecessary overhead.

**Optimization:** Implemented a custom open-addressing term dictionary.

The `TermTable` uses:

- A hash table with linear probing.
- Dense integer term IDs.
- A contiguous character arena.
- Compact hash-table slots.
- Hash and length checks before full string comparison.

Term strings are stored once in the arena.

The hash table stores offsets into the arena instead of owning separate strings for every slot.

**Why it helps:**
- Avoids repeated string allocations.
- Reduces dictionary memory overhead.
- Provides efficient term lookup.
- Makes posting-list indexing operate on integer IDs instead of strings.

The dictionary becomes read-only after query parsing, allowing concurrent document scanning without synchronization.

### Stage 4 — Memory-mapped document input

**Problem:** Loading the entire document collection into a separate string buffer creates additional memory usage and copying overhead.

**Optimization:** Used `mmap()` to map `docs.txt` into memory.

Implementation:

1. Open the file.
2. Retrieve its size using `fstat()`.
3. Map it using `mmap()`.
4. Process document bytes directly from the mapped region.

**Why it helps:**
- Avoids explicitly copying the entire document file into another application buffer.
- Allows direct byte-level scanning.
- Reduces repeated file-reading overhead.

The mapped input is accessed read-only.

### Stage 5 — Parallel document partitioning

**Problem:** Sequentially tokenizing the entire document collection limits processing throughput.

**Optimization:** Divided the document collection into approximately equal-sized byte ranges.

Each worker receives:

- A starting pointer.
- An ending pointer.
- The document ID of its first document.

Partitions are aligned to newline boundaries so that workers process complete documents.

Document IDs are reconstructed using the dense document-ID assumption in the input specification.

**Why it helps:**
- Allows independent document scanning.
- Avoids splitting documents across workers.
- Reduces synchronization during tokenization.
- Makes document processing scalable across available CPU cores.

The build thread count can be controlled through `SD_BUILD_THREADS`.

### Stage 6 — Fast tokenization with a lookup table

**Problem:** Repeatedly checking whether every input character belongs to the token alphabet can introduce unnecessary branching.

**Optimization:** Created a 256-entry token lookup table.

The `g_tok` table maps:

- Uppercase ASCII letters to lowercase letters.
- Lowercase ASCII letters to themselves.
- ASCII digits to themselves.
- All other bytes to zero.

During tokenization, each input byte requires only a lookup.

A zero value indicates a separator.

**Why it helps:**
- Avoids repeated character classification.
- Performs ASCII lowercasing during the same lookup.
- Handles non-ASCII bytes as separators.
- Simplifies the tokenization hot path.

### Stage 7 — Query-term filtering during tokenization

**Problem:** Creating tokens for every document word and storing them would waste work when most words are irrelevant to the queries.

**Optimization:** The scanner creates temporary token data and immediately checks whether it exists in the query dictionary.

For tokens up to 32 bytes:

1. Copy the normalized token into a small stack buffer.
2. Calculate its hash.
3. Look it up in the read-only term table.
4. Emit an occurrence only if the token is relevant.

Longer tokens use a slower reusable thread-local string path when long query terms exist.

**Why it helps:**
- Avoids storing irrelevant tokens.
- Keeps the common token path compact.
- Reduces posting-list construction work.
- Avoids repeated heap allocation for ordinary short tokens.

Every token still increments the document token-position counter, including tokens that are not part of the query dictionary.

This is essential for correct phrase matching.

### Stage 8 — Two-pass parallel CSR construction

**Problem:** Dynamically growing a separate posting vector for every term creates allocation overhead and makes parallel index construction difficult.

**Optimization:** Replaced per-term dynamic vectors with a two-pass compressed sparse row (CSR) construction.

The first pass:

- Tokenizes each document.
- Counts document frequency per term.
- Counts positional occurrences for phrase-relevant terms.
- Stores compact term and position events in a worker-local stream.
- Records document boundaries.

The second pass:

- Uses the first-pass counts to calculate exact storage offsets.
- Allocates final posting and position arrays.
- Replays the compact event streams.
- Writes posting lists directly into their assigned CSR regions.

**Why it helps:**
- Avoids repeated per-term vector growth.
- Calculates storage requirements before allocation.
- Reduces memory fragmentation.
- Allows workers to write into independent output ranges.
- Avoids tokenizing the original corpus twice.

The final index uses contiguous arrays rather than thousands of separately allocated posting vectors.

### Stage 9 — Selective positional indexing

**Problem:** Phrase queries need token positions, but Boolean queries only need document membership.

Storing positions for every query term would increase memory consumption unnecessarily.

**Optimization:** Introduced the `need_pos` array.

During the first pass:

- Document frequency is counted for every query term.
- Positional occurrence counts are collected only for phrase-relevant terms.

During the second pass:

- Posting lists are created for all query terms.
- Position arrays are populated only when `need_pos[term]` is true.

**Why it helps:**
- Reduces index memory usage.
- Avoids storing unnecessary token positions.
- Preserves exact phrase matching.
- Separates document-level search information from positional search information.

This makes the index specific to the actual query workload.

### Stage 10 — Compact event streams

**Problem:** The first-pass indexing results need to be transferred into the final CSR structure.

Keeping separate dynamically allocated records for every token occurrence would increase memory and allocation overhead.

**Optimization:** Encoded indexing events into 64-bit integers.

The event stream stores:

- Term ID in the upper 32 bits.
- Token position in the lower 32 bits.

Document boundary events use a reserved high-half marker.

Each worker maintains its own event stream.

**Why it helps:**
- Uses a compact representation.
- Avoids separate event structures.
- Reduces intermediate allocation overhead.
- Allows the second pass to reconstruct posting information without rescanning the original corpus.

The event streams are worker-local, so their construction does not require shared synchronization.

### Stage 11 — Exact CSR memory allocation

**Problem:** Growing posting lists incrementally makes memory requirements unpredictable and can introduce repeated reallocations.

**Optimization:** Used first-pass counts to calculate exact final array sizes.

The index maintains:

- `post_data` for document IDs.
- `pos_data` for token positions.
- `pstart_data` for positional boundaries.
- Offset arrays for individual terms.

Each term receives a contiguous slice of the final arrays.

**Why it helps:**
- Eliminates repeated posting-vector growth.
- Reduces allocation overhead.
- Improves memory locality.
- Makes posting-list access predictable.
- Separates index metadata from posting data.

### Stage 12 — Optimized Boolean set operations

**Problem:** Boolean queries require repeated intersections, unions and differences of sorted posting lists.

Using general-purpose containers for these operations would introduce unnecessary overhead.

**Optimization:** Implemented specialized sorted-list operations:

- `intersect_into()`
- `union_into()`
- `difference_into()`

All operations work directly with sorted document-ID arrays.

**Intersection optimization**

The implementation chooses the smaller posting list as the driving list.

When there is a large size difference, it uses binary search instead of a complete merge.

For more similarly sized lists, it uses a sequential merge with an eight-element comparison fast path.

**Union optimization**

Two sorted lists are merged while removing duplicate document IDs.

**Difference optimization**

The implementation produces elements present in the first list but absent from the second.

For highly unequal list sizes, it uses binary search.

**Why it helps:**
- Avoids general-purpose set containers.
- Takes advantage of sorted posting lists.
- Reduces unnecessary comparisons.
- Adapts the intersection strategy to posting-list sizes.

### Stage 13 — SIMD-assisted intersection

**Problem:** Comparing document IDs one at a time can be inefficient when scanning larger posting lists.

**Optimization:** Introduced architecture-specific eight-element comparison.

On x86 systems with SSE2:

- Load eight 32-bit values in two four-element vectors.
- Compare both vectors against the target document ID.
- Combine the comparison masks.
- Detect whether any match exists.

On ARM64 systems:

- Use NEON vector comparisons.
- Combine the comparison results.
- Detect whether a matching value exists.

A scalar fallback is provided for other architectures.

**Why it helps:**
- Compares multiple document IDs per operation.
- Reduces scalar comparison overhead.
- Uses available CPU vector instructions.
- Maintains portable fallback behavior.

### Stage 14 — Boolean query evaluation

**Problem:** Executing associative Boolean operations in their original operand order can cause unnecessarily large intermediate posting lists.

**Optimization:** Groups consecutive operations of the same associative type.

For example, a sequence of AND operations can be evaluated in any order without changing the result.

The implementation:

1. Collects the consecutive operands.
2. Sorts them by posting-list size.
3. Processes the smallest lists first.
4. Uses reusable intermediate buffers.

The same approach is used for consecutive OR operations.

NOT remains a binary difference operation and the overall Boolean expression retains strict left-to-right semantics.

**Why it helps:**
- Reduces intermediate result sizes for AND operations.
- Allows smaller lists to eliminate candidates earlier.
- Reduces unnecessary set-operation work.
- Preserves the required Boolean evaluation semantics.

### Stage 15 — Phrase-query optimization

**Problem:** Phrase matching requires more than checking whether all query terms occur in a document.

The terms must appear at consecutive token positions and in the correct order.

**Optimization:** Uses a two-stage phrase-search process.

**Stage A — Candidate document filtering**

- Retrieve posting lists for all phrase terms.
- Identify the smallest posting list.
- Intersect posting lists in increasing size order.
- Retain only documents containing every phrase term.

**Stage B — Positional verification**

For each candidate document:

1. Retrieve the positional slices for the query terms.
2. Examine candidate starting positions.
3. Check whether every other term appears at its required consecutive position.
4. Accept the document only when the complete phrase is present.

**Why it helps:**
- Avoids positional checks across the entire document collection.
- Uses document-level intersections to reduce candidates.
- Preserves exact phrase semantics.
- Reduces unnecessary positional searches.

### Stage 16 — Parallel query execution

**Problem:** Executing all queries sequentially wastes available CPU resources.

**Optimization:** Divided the query list into contiguous ranges and assigned each range to a worker thread.

Each query worker owns its own `ThreadCtx`, containing:

- Two Boolean intermediate buffers.
- Two phrase candidate buffers.
- A private output string.

The index itself is read-only during query execution.

**Why it helps:**
- Allows independent queries to execute concurrently.
- Avoids shared intermediate buffers.
- Eliminates synchronization during ordinary query execution.
- Reuses allocated memory between queries handled by the same worker.

The query thread count can be controlled through `SD_THREADS`.

### Stage 17 — Efficient result formatting

**Problem:** Standard formatting operations can introduce overhead when many queries produce output.

**Optimization:** Introduced a custom integer formatter.

For every result, the implementation calculates:

- Number of matching documents.
- Sum of matching document IDs.
- Up to ten smallest matching document IDs.

Because posting lists are sorted, the first ten entries are already the smallest IDs.

Each worker writes results into its own string buffer.

The final output is written in worker-range order, preserving query order.

**Why it helps:**
- Avoids repeated stream formatting.
- Uses already-sorted posting lists.
- Reduces synchronization during output generation.
- Preserves deterministic result ordering.

---

## 3. Final Architecture

The implementation follows this pipeline:

```text
                 queries.txt
                      |
                      v
                Query Parsing
                      |
                      v
               Unique Term Table
                      |
                +-----+------+
                |            |
                v            v
             Query Ops   Phrase Terms
                             |
                             v
                       need_pos[]
                             |
                             |
                  docs.txt (mmap)
                             |
                             v
                  Parallel Partitioning
                             |
             +---------------+---------------+
             |               |               |
             v               v               v
          Worker 1        Worker 2        Worker N
             |               |               |
             +---------------+---------------+
                             |
                             v
                   Tokenization and Lookup
                             |
                             v
                    Compact Event Streams
                             |
                             v
                    First-Pass Counting
                             |
                             v
                    Exact CSR Allocation
                             |
                             v
                    Second-Pass Filling
                             |
                             v
                      Search Index
                             |
                             v
                   Parallel Query Execution
                             |
               +-------------+-------------+
               |             |             |
               v             v             v
             Worker 1      Worker 2      Worker N
               |             |             |
               +-------------+-------------+
                             |
                             v
                   Boolean / Phrase Search
                             |
                             v
                    Result Aggregation
                             |
                             v
                     results.txt
```

---

## 4. Performance Optimization Principles

| Optimization | Main benefit |
|---|---|
| Query-first indexing | Avoids irrelevant terms |
| Custom term dictionary | Reduces string allocation overhead |
| `mmap()` | Avoids explicit full-file copying |
| Parallel document partitioning | Uses multiple CPU cores |
| Token lookup table | Faster byte classification |
| Query-term filtering | Avoids unnecessary index entries |
| Two-pass CSR construction | Exact allocation and compact storage |
| Selective positional indexing | Reduces memory usage |
| Packed event streams | Reduces intermediate representation overhead |
| SIMD-assisted intersection | Faster posting-list comparisons |
| Adaptive intersection | Uses list-size-aware algorithms |
| Boolean operand reordering | Reduces intermediate results |
| Phrase candidate filtering | Reduces positional verification work |
| Thread-local query buffers | Reduces synchronization |
| Custom integer formatting | Reduces output overhead |

---

## 5. Correctness and Validation

Performance improvements are useful only when the search results remain exactly correct.

### Tokenization correctness

- Only ASCII letters and digits belong to tokens.
- Uppercase ASCII letters are converted to lowercase.
- All other bytes are separators.
- Every token advances the token-position counter.

### Boolean correctness

- AND performs sorted intersection.
- OR performs sorted union.
- NOT performs binary difference.
- Operators are evaluated strictly from left to right.
- Reordering is restricted to consecutive runs of the same associative operator.

### Phrase correctness

- All query terms must occur in the document.
- Terms must appear in the required order.
- Positions must be consecutive.
- Repeated terms must be handled correctly.

### Output correctness

- Correct result count.
- Correct sum of document IDs.
- Correct ten smallest document IDs.
- Query output order preserved.
- Empty results formatted correctly.

### Recommended validation

1. Compare `results.txt` against the reference implementation.
2. Verify SHA-256 equality.
3. Test mixed-case tokens.
4. Test punctuation and non-ASCII separators.
5. Test empty posting lists.
6. Test repeated query terms.
7. Test complex Boolean expressions.
8. Test phrases with repeated words.
9. Test empty and single-term queries.
10. Test multiple thread-count configurations.

---

## 6. Build and Execution

### Compilation

```bash
g++ -O3 -std=c++17 -DNDEBUG -march=native \
    -flto -pthread -static -s \
    -o runner src/main.cpp
```

### Execution

```bash
./runner /data /out
```

### Input files

```text
/data/docs.txt
/data/queries.txt
```

### Expected output

```text
/out/results.txt
```

### Thread configuration

The implementation supports two environment variables:

```bash
SD_BUILD_THREADS=8
SD_THREADS=8
```

- `SD_BUILD_THREADS` controls parallel index construction.
- `SD_THREADS` controls parallel query execution.

Both are constrained by the available CPU count and configured limits.

---

## 7. Conclusion

The Track B implementation focuses on building a search index specifically designed around the provided query workload.

Instead of indexing every document token and retaining all positional information, it first examines the queries and stores only the information required to answer them.

The main architectural decisions were:

- Query-aware term selection.
- Memory-mapped document access.
- Parallel document tokenization.
- Compact term dictionaries.
- Two-pass CSR index construction.
- Selective positional storage.
- SIMD-assisted posting-list operations.
- Posting-list-size-aware Boolean evaluation.
- Candidate-based phrase verification.
- Parallel query execution.

The central design principle was to minimize the amount of information stored and the amount of work performed for each query.

The resulting implementation is designed to balance indexing speed, query throughput, memory consumption and exact search correctness.

**Optimization philosophy: Index only what matters. Store it compactly. Search the smallest candidate set. Avoid work that cannot affect the answer.**
