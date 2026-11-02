# Track C — Performance Optimization Journey

## 1. Overview

**Track:** C — Join  
**Language:** C++  
**Build configuration:** `-O3 -std=c++17 -DNDEBUG -flto -pthread -static -s`  
**Input files:** `records.csv`, `regions.csv`  
**Output file:** `aggregates.txt`

### Objective

Process two CSV datasets, join records using region IDs, and generate country-level aggregates.

The implementation must correctly handle hostile CSV input, including:

- Quoted fields containing commas.
- Escaped double quotes (`""`).
- Embedded LF and CRLF characters.
- UTF-8 BOM markers at the beginning of files and at certain record boundaries.
- Mixed LF and CRLF record terminators.
- Exact monetary calculations using integer cents.

The final output contains one line per country, sorted bytewise, with the following aggregated information:

- Record count.
- Sum of record IDs.
- Total amount.
- Minimum amount.
- Maximum amount.
- Number of distinct names.
- Total name bytes.
- Total note bytes.

The main objective was to minimize parsing overhead, memory consumption, synchronization and unnecessary data movement while preserving exact CSV semantics.

---

## 2. Optimization Journey

### Stage 1 — Understanding the workload

The first step was identifying the expensive operations in the workload.

The main performance challenges were:

1. Parsing complex CSV records correctly.
2. Handling quoted fields and escaped characters.
3. Joining millions of records with region information.
4. Counting distinct names for every country.
5. Aggregating financial values accurately.
6. Processing records concurrently without introducing excessive synchronization.

A conventional CSV parser combined with standard hash maps and per-thread aggregation could introduce considerable allocation and synchronization overhead.

**Optimization direction:** Build a specialized parser and aggregation pipeline around the exact input specification.

### Stage 2 — Memory-mapped input using mmap

**Problem:** Repeated buffered reads and copying input data into additional buffers can introduce unnecessary overhead.

**Optimization:** Used `mmap()` to map `records.csv` directly into the process address space.

Implementation:
- Opened the input using `open()`.
- Retrieved its size using `fstat()`.
- Mapped the file using `mmap()`.
- Processed records directly from the mapped memory.
- Released the mapping after parsing with `munmap()`.

**Why it helps:**
- Avoids explicitly copying the entire records file into a separate application buffer.
- Provides direct access to input bytes.
- Reduces repeated file-reading and buffering operations.

The main records file remains read-only throughout processing.

### Stage 3 — Quote-aware parallel partitioning

**Problem:** Splitting CSV files at ordinary newline characters is unsafe because quoted fields can contain embedded newlines.

A simple newline-based partition could split a record between workers and produce incorrect parsing.

**Optimization:** Implemented quote-aware record boundary detection.

The implementation uses quote parity:

- Even quote parity indicates that the parser is outside a quoted field.
- Odd quote parity indicates that the parser is inside a quoted field.
- A newline is considered a valid record boundary only when quote parity is even.

The partitioning process:

1. Divide the input into nominal byte ranges.
2. Count quote characters in each range.
3. Calculate the incoming quote parity for each partition.
4. Find the next valid record boundary.
5. Assign complete records to individual workers.

**Why it helps:**
- Enables parallel parsing of complex CSV input.
- Prevents splitting records at embedded newlines.
- Avoids requiring a single sequential parser for the entire file.

This is one of the most important correctness-related optimizations in Track C.

### Stage 4 — Parallelizing independent preparation tasks

**Problem:** Loading region information and preparing record partitions are independent operations.

Executing them sequentially would delay the beginning of the parsing pipeline.

**Optimization:** Ran region loading concurrently with the initial quote-counting phase.

Implementation:
- Started a helper thread for `regions.csv`.
- Counted quote characters across the records file using parallel workers.
- Joined the region-loading thread after quote counting completed.

**Why it helps:**
- Overlaps independent preparation work.
- Reduces unnecessary sequential execution.
- Allows region information to be prepared while the main dataset is being examined.

### Stage 5 — Specialized CSV parsing

**Problem:** General-purpose CSV parsing can introduce overhead when the input format and required fields are known.

**Optimization:** Implemented a specialized parser that processes fields directly from the mapped input.

Important techniques:

**Quoted-field scanning**

Implemented `scan_field()` to distinguish quoted and unquoted fields.

It handles:
- Quoted commas.
- Escaped double quotes.
- Field lengths after unescaping.
- Record boundaries.

**Reduced string copying**

Fields without escaped quotes can be referenced directly from the mapped file.

For escaped names, a reusable per-thread scratch buffer performs unescaping.

**Byte-level quote scanning**

Implemented an 8-byte SWAR-style quote detection technique using:
- `memcpy()` for word loading.
- Bitwise operations for quote detection.
- `__builtin_ctzll()` for locating the first matching byte.
- `__builtin_popcountll()` for counting quote characters.

This avoids a separate `memchr()` operation for every quoted field.

**Why it helps:**
- Reduces repeated character-by-character processing.
- Avoids unnecessary string allocations.
- Handles the CSV quoting rules required by the workload.
- Makes quote scanning more efficient for short fields.

### Stage 6 — Optimized integer parsing

**Problem:** Conventional decimal parsing repeatedly performs multiplication and addition for every digit.

For millions of records, this becomes a frequent operation.

**Optimization:** Introduced specialized 8-byte decimal parsing.

The implementation uses:

- `ndigits()` to identify the number of leading decimal digits.
- `digits8()` to convert up to eight digits using arithmetic transformations.
- `parse_u64()` to handle longer values using a fallback digit loop.

The implementation also prefetches the likely hash-table location while the remaining fields of the current record are being parsed.

**Why it helps:**
- Reduces the dependency chain of repeated decimal multiplication.
- Processes common short numeric fields efficiently.
- Overlaps some lookup latency with subsequent parsing work.

The optimized conversion retains integer arithmetic.

### Stage 7 — Exact financial calculations

**Problem:** Floating-point arithmetic can introduce rounding errors when processing monetary values.

**Optimization:** Represented monetary amounts as integer cents.

For example:

`125.75` becomes `12575`.

The parser separately processes:
- Optional negative sign.
- Whole-number portion.
- Decimal separator.
- Two fractional digits.

The resulting amount is stored as an integer.

**Why it helps:**
- Avoids floating-point precision issues.
- Makes addition and comparison exact within the supported integer range.
- Simplifies minimum, maximum and sum calculations.
- Preserves two-decimal monetary formatting.

This is a correctness-focused optimization as well as a performance decision.

### Stage 8 — Region lookup optimization

**Problem:** Every record needs to be associated with a country using its region ID.

Repeatedly searching the region dataset would add unnecessary overhead.

**Optimization:** Built a direct region-to-country lookup table.

The preparation process:

1. Load `regions.csv`.
2. Extract country codes.
3. Assign compact country IDs.
4. Build a lookup table indexed by region ID.
5. Use the table during record parsing.

Each record can then identify its country using a direct indexed lookup.

Unmatched region IDs are assigned to `_UNMATCHED`.

**Why it helps:**
- Avoids repeated general-purpose searches.
- Provides constant-time region lookup.
- Uses compact integer identifiers during aggregation.
- Separates join preparation from the main processing loop.

### Stage 9 — Shared name interning table

**Problem:** Counting distinct names separately across threads and merging the results later can introduce considerable memory overhead.

Maintaining separate name dictionaries for every worker also duplicates information.

**Optimization:** Introduced a shared open-addressing name table.

The table uses:

- \(2^{18}\) slots.
- A maximum load factor of approximately 70%.
- Compact 16-byte slot structures.
- Global integer IDs for interned names.
- A block-based memory arena for storing name bytes.

The lookup process:

1. Calculate a name hash.
2. Probe the shared table.
3. Compare hashes and lengths.
4. Perform full byte comparison when necessary.
5. Return an existing global name ID or insert a new name.

The hash function uses the name length and selected bytes rather than processing every byte through a serial FNV-1a multiplication chain.

Full `memcmp()` comparison ensures that hash collisions do not cause incorrect name matches.

**Why it helps:**
- Avoids duplicate name storage.
- Provides globally consistent name IDs.
- Reduces per-thread dictionary duplication.
- Makes distinct-name aggregation more memory-efficient.

### Stage 10 — Lock-free common lookups

**Problem:** A conventional mutex around every shared name lookup would create unnecessary contention.

Most names are expected to have already been inserted by another record or worker.

**Optimization:** Separated lookup operations from insertion operations.

The implementation uses:
- Acquire atomic loads for reading table slots.
- Release atomic stores when publishing new entries.
- A spinlock only when a name is not found.
- A second lookup after acquiring the lock to handle concurrent insertion races.

This means ordinary successful lookups do not require the insertion lock.

**Why it helps:**
- Keeps the common lookup path free from lock acquisition.
- Limits synchronization to new-name insertion.
- Allows multiple workers to search the same table concurrently.
- Prevents duplicate insertion of identical names.

The table is never rehashed or cleared during processing, which simplifies concurrent lookup.

### Stage 11 — Shared bitmap for distinct-name counting

**Problem:** Maintaining a separate distinct-name bitmap for every worker and country can consume considerable memory.

**Optimization:** Assigned every interned name a global ID and maintained a shared bitmap for each country.

For each record:

1. Obtain its global name ID.
2. Calculate the corresponding bitmap word.
3. Set the appropriate bit using an atomic OR.

The implementation uses relaxed atomic OR operations because the bitmap update only needs atomicity; it does not publish additional dependent data.

At the end of parsing, the system counts set bits using `__builtin_popcountll()`.

**Why it helps:**
- Avoids per-thread copies of the distinct-name bitmap.
- Prevents duplicate counting of the same name within a country.
- Reduces memory requirements.
- Makes distinct counting independent of the scalar aggregation process.

The implementation comments record an estimated peak bitmap cost of approximately 20 MB, compared with approximately 52 MB for the private-bitmap alternative tested.

### Stage 12 — Batched name processing

**Problem:** Sequential hash-table lookups can create dependency chains where the next lookup begins only after the previous lookup completes.

**Optimization:** Introduced batches of 32 pending name operations.

The parser collects:
- Name hash.
- Name pointer.
- Name length.
- Country ID.

Once the batch is full, `flush_pending()` performs the lookups and bitmap updates.

This allows multiple independent lookup operations to be issued without waiting for every previous lookup to finish.

**Why it helps:**
- Exposes independent memory operations.
- Helps overlap hash-table lookup latency.
- Reduces the impact of serial lookup dependencies.
- Groups bitmap updates into a separate processing stage.

Names requiring the reusable unescaping scratch buffer are handled immediately so their contents are not overwritten before interning.

### Stage 13 — Thread-local scalar aggregation

**Problem:** Updating shared country-level counters from multiple workers would introduce contention.

**Optimization:** Each worker maintains its own `Acc` array.

The accumulator stores:

- Record count.
- Sum of IDs.
- Sum of amounts.
- Minimum amount.
- Maximum amount.
- Total name bytes.
- Total note bytes.

Workers update their own accumulators while processing records.

After parsing, the scalar accumulators are merged country by country.

**Why it helps:**
- Avoids shared counter updates during parsing.
- Eliminates mutexes for ordinary scalar aggregation.
- Reduces synchronization.
- Keeps the main parsing loop independent.

### Stage 14 — Parallel final aggregation

**Problem:** Combining country-level statistics and calculating distinct-name counts can become a sequential finalization stage.

**Optimization:** Partitioned the country range across workers.

Each worker:
1. Processes its assigned countries.
2. Combines scalar accumulators from all parsing workers.
3. Calculates distinct-name counts using bitmap population counts.
4. Stores the final results in the corresponding country positions.

**Why it helps:**
- Parallelizes final aggregation.
- Separates parsing from final reduction.
- Distributes bitmap counting across workers.

### Stage 15 — Efficient output generation

**Problem:** Output generation should not introduce unnecessary formatting overhead or alter the required ordering.

**Optimization:** Used a custom integer formatter and a single reserved output buffer.

Implementation:
- Collected countries with nonzero record counts.
- Sorted them bytewise.
- Formatted integer values using `put_u64()`.
- Formatted monetary values using `put_cents()`.
- Appended complete output lines to a reserved string.
- Wrote the final buffer using `fwrite()`.

**Why it helps:**
- Avoids repeated stream-formatting operations.
- Reduces unnecessary output allocations.
- Preserves exact monetary formatting.
- Maintains deterministic bytewise country ordering.

---

## 3. Final Architecture

The final implementation follows this pipeline:

```text
                 records.csv
                      |
                      v
                Memory Mapping
                    mmap
                      |
           +----------+----------+
           |                     |
           v                     v
    Quote Counting         regions.csv
           |                     |
           v                     v
    Record Boundaries      Country Lookup
           |                     |
           +----------+----------+
                      |
                      v
             Parallel CSV Parsing
                      |
          +-----------+-----------+
          |           |           |
          v           v           v
        Worker 1    Worker 2    Worker N
          |           |           |
          +-----------+-----------+
                      |
          +-----------+-----------+
          |                       |
          v                       v
    Thread-Local             Shared Name
    Aggregation                Table
          |                       |
          |                  Global IDs
          |                       |
          |                  Country Bitmaps
          |                       |
          +-----------+-----------+
                      |
                      v
              Final Aggregation
                      |
                      v
              Distinct Counting
                      |
                      v
              Bytewise Sorting
                      |
                      v
                Output Formatting
                      |
                      v
                 aggregates.txt
```

---

## 4. Measured Optimization Decisions

The implementation comments document several measured comparisons.

These are useful because they show that optimization decisions were evaluated rather than selected solely based on intuition.

| Optimization experiment | Recorded result |
|---|---|
| Shared bitmap vs private bitmaps | Approximately 20 MB vs 52 MB peak bitmap memory |
| Batched name lookups | 0.886x wall time |
| 16-byte vs 24-byte hash slots | Approximately 5% faster end-to-end |
| Custom hash vs FNV-1a | 1.296 vs 1.300 probes/lookups; approximately 1.6% less wall time |
| Spinlock vs mutex | Approximately 8% faster end-to-end |
| Eight worker threads | Recorded as the measured thread-count knee |

**Important:** The 0.886x wall-time figure means the batched implementation took approximately 88.6% of the comparison implementation's wall time, equivalent to an approximately 11.4% reduction.

These measurements are specific to the experiments documented in the implementation and should be accompanied by their original benchmark conditions when available.

---

## 5. Memory Optimization Strategy

Memory efficiency was a major consideration in Track C.

The implementation uses several complementary approaches:

| Technique | Memory benefit |
|---|---|
| `mmap()` | Avoids an explicit full-file input copy |
| `std::string_view`-style pointer references | Avoids copying ordinary field contents |
| Reusable scratch buffer | Avoids repeated temporary allocations |
| Block-based name arena | Reduces individual name allocations |
| Compact 16-byte hash slots | Reduces hash-table footprint |
| Global name IDs | Avoids duplicated name dictionaries |
| Shared country bitmaps | Reduces distinct-counting memory |
| Thread-local scalar accumulators | Avoids shared synchronization structures |

The shared bitmap approach is particularly important because it avoids allocating a separate full name bitmap for every worker.

---

## 6. Correctness and Validation

The optimizations must preserve the CSV specification and exact aggregation results.

### CSV correctness

The parser must correctly handle:

- Quoted commas.
- Escaped double quotes.
- Embedded newlines.
- CRLF and LF terminators.
- BOM markers.
- Final records without a newline.

### Join correctness

- Region IDs are mapped to country IDs.
- Unknown regions are assigned to `_UNMATCHED`.
- Country aggregation uses the correct joined country.

### Financial correctness

- Amounts are represented in integer cents.
- Negative values are preserved.
- Sum, minimum and maximum are calculated using integer arithmetic.

### Distinct-name correctness

- Name equality uses full byte comparison.
- Global IDs are assigned to interned names.
- Country bitmaps count each distinct name once.
- Concurrent bitmap updates use atomic operations.

### Output correctness

- Exactly one output line per populated country.
- Country names are sorted bytewise.
- Monetary values retain two decimal places.
- Lines are LF terminated.

Recommended validation:

1. Compare output with the reference implementation.
2. Verify SHA-256 equality.
3. Test CSV records containing embedded newlines.
4. Test escaped quotes and quoted commas.
5. Test BOM markers and mixed line endings.
6. Test unmatched region IDs.
7. Test duplicate names across different workers.
8. Verify monetary totals against a trusted reference.

---

## 7. Build and Execution

The implementation uses the following build configuration:

```bash
g++ -O3 -std=c++17 -DNDEBUG -flto -pthread -static -s \
    -o solution src/main.cpp
```

Runtime inputs:

```text
/data/records.csv
/data/regions.csv
```

Expected output:

```text
/out/aggregates.txt
```

---

## 8. Conclusion

The Track C implementation focuses on optimizing the entire CSV join pipeline rather than concentrating on parsing speed alone.

The major architectural decisions were:

- Quote-aware parallel input partitioning.
- Specialized byte-level CSV parsing.
- Optimized integer conversion.
- Direct region-to-country lookup.
- Shared name interning.
- Lock-free common hash-table lookups.
- Atomic bitmap-based distinct counting.
- Batched name processing.
- Thread-local scalar aggregation.
- Parallel final reduction.

The most significant design principle was to separate the common operations from expensive exceptional operations.

Ordinary name lookups remain lock-free, while new-name insertion uses synchronization. Ordinary field references avoid copying, while escaped names use reusable scratch storage. Scalar aggregation stays thread-local, while distinct-name information uses a shared compact bitmap.

This combination creates a pipeline designed for high-throughput processing without sacrificing CSV correctness or exact financial calculations.

**Optimization philosophy: Parse intelligently. Share selectively. Aggregate locally. Synchronize only when necessary.**
