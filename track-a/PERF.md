# Track A — Performance Optimization Journey

## 1. Overview

**Track:** A — Ingest  
**Language:** C++  
**Approach:** Memory-mapped input, parallel processing, thread-local aggregation, compact indexing and exact percentile selection.

The objective was to process 3.1 million structured log records and generate an exact per-hour, per-endpoint latency summary while minimizing:

- Execution time
- Peak memory consumption
- CPU overhead
- Unnecessary memory allocations
- Data movement and synchronization

The implementation was designed to preserve the required output format and exact percentile calculations.

---

## 2. Optimization Journey

### Stage 1 — Understanding the workload

Before optimizing, we examined the characteristics of the input and output.

The workload involved:

- 3.1 million log records.
- Timestamp-based hourly grouping.
- Endpoint-based aggregation.
- HTTP status and error counting.
- P50, P95 and P99 latency calculations.
- A fixed 72-hour processing window.
- Multiple endpoints across the dataset.

The main challenge was not simply reading the input quickly. It was efficiently grouping millions of records and calculating exact latency percentiles without excessive memory usage.

**Optimization direction:** Reduce unnecessary parsing, allocation, copying and synchronization.

### Stage 2 — Memory-mapped input using mmap

**Problem:** Traditional file-reading approaches can introduce additional buffering and copying overhead.

**Optimization:** Replaced conventional stream-based input handling with memory-mapped file access using `mmap()`.

Implementation:
- Opened the input file using `open()`.
- Retrieved its size using `fstat()`.
- Mapped the file into memory using `mmap()`.
- Processed records directly from the mapped region.

**Why it helps:**
- Avoids explicitly copying the entire input into a separate user-space buffer.
- Allows direct access to input bytes.
- Reduces overhead associated with repeated stream operations.

The input remains read-only throughout processing.

### Stage 3 — Parallel processing with newline-aligned partitioning

**Problem:** Processing millions of records sequentially limits the available CPU parallelism.

**Optimization:** Divided the mapped input into approximately equal-sized sections and processed them using multiple threads.

Implementation:
- Determined available CPU capacity.
- Used CPU affinity information on Linux.
- Limited the worker count to a maximum of 12.
- Assigned each worker an independent input section.
- Aligned partition boundaries immediately after newline characters.

**Why it helps:**
- Multiple CPU cores can process separate input sections concurrently.
- Each worker processes complete records.
- No shared input-position lock is needed.
- Reduces synchronization during parsing.

The thread count is capped rather than automatically creating an unlimited number of workers.

### Stage 4 — Thread-local aggregation

**Problem:** Sharing aggregation structures between multiple threads can introduce synchronization overhead and contention.

**Optimization:** Introduced a separate `Work` structure for every worker thread.

Each worker maintains its own:

- Endpoint dictionary.
- Endpoint hashes.
- Hour/endpoint groups.
- Latency vectors.
- Hash-table indexes.

**Why it helps:**
- Avoids shared writes during record processing.
- Eliminates the need for mutexes in the aggregation hot path.
- Reduces contention between workers.
- Keeps intermediate aggregation independent.

Instead of repeatedly updating shared global structures, workers aggregate their own records first.

### Stage 5 — Lightweight endpoint hashing

**Problem:** Repeatedly searching endpoint strings or creating new string objects for every record introduces unnecessary work.

**Optimization:** Implemented a custom open-addressing hash table using linear probing.

Key techniques:

- Used `std::string_view` instead of copying endpoint strings.
- Kept endpoint references pointing into the memory-mapped input.
- Stored endpoint hashes separately.
- Compared hashes before performing full string equality checks.
- Used linear probing to resolve collisions.
- Resized the endpoint hash table when its load exceeded 50%.

The hash function uses the endpoint length and selected bytes from its beginning and end.

Full string comparison is still performed when hashes match, preserving collision correctness.

**Why it helps:**
- Reduces repeated string allocations.
- Avoids copying endpoint names.
- Makes endpoint lookup lightweight.
- Keeps the hash table compact.

### Stage 6 — Direct hour/endpoint indexing

**Problem:** Repeatedly searching for an existing hour/endpoint group would add unnecessary lookup overhead.

**Optimization:** Used a direct-indexed group table.

The group index is calculated using:

`hour * kEndpointLimit + endpoint_id`

The implementation uses:

- A fixed 72-hour domain.
- A maximum endpoint index domain of 1842.
- A direct lookup table using integer indexes.
- A most-recently-used group cache.

The `last` field avoids repeating a lookup when consecutive records belong to the same group.

**Why it helps:**
- Avoids general-purpose map lookups.
- Provides constant-time group lookup.
- Reduces repeated key comparisons.
- Makes aggregation more predictable.

### Stage 7 — Reducing memory allocations

**Problem:** Repeated vector growth and unnecessary object creation can increase memory-management overhead.

**Optimization:** Applied several allocation-conscious techniques.

1. Reserved initial latency-vector capacity for new groups.
2. Used compact integer indexes instead of storing repeated endpoint strings.
3. Reused thread-local structures throughout parsing.
4. Used `std::move()` when transferring groups into the final aggregation structure.
5. Avoided unnecessary intermediate input buffers.

The latency vectors remain dynamically sized because exact percentile calculations require retaining the latency observations.

**Why it helps:**
- Reduces vector reallocations.
- Minimizes unnecessary copying.
- Reduces duplicate string storage.
- Improves memory efficiency.

### Stage 8 — Compact merging of thread-local results

**Problem:** After parallel parsing, thread-local results must be combined into the final hourly endpoint summary.

**Optimization:** Introduced a global sorted endpoint list and a dense hour/endpoint grid.

The merging process:

1. Collect endpoint references from all workers.
2. Sort and remove duplicate endpoint names.
3. Assign each endpoint a global rank.
4. Convert worker-local endpoint IDs into global ranks.
5. Merge each worker's groups into the corresponding dense grid position.
6. Move groups directly when the destination is empty.
7. Append latency observations when multiple workers contribute to the same group.

**Why it helps:**
- Avoids repeatedly searching for destination groups.
- Uses compact integer ranks.
- Reduces copying when moving entire groups.
- Provides direct access to the final hour/endpoint combinations.

### Stage 9 — Exact percentile calculation

**Problem:** Sorting every latency observation for every group requires unnecessary work when only P50, P95 and P99 are needed.

**Optimization:** Used `std::nth_element()` for larger groups and full sorting for small groups.

The implementation follows nearest-rank percentile calculation:

`rank = ceil(percentile * n / 100)`

For groups containing more than 32 observations:
- Select P50 using `std::nth_element()`.
- Select P95 using a subsequent partial selection.
- Select P99 using another partial selection.

For groups containing 32 or fewer observations, the vector is sorted directly.

**Why it helps:**
- Avoids fully sorting large latency vectors.
- Reduces unnecessary comparison operations.
- Calculates exact percentile values rather than approximate estimates.

Importantly, the implementation does not use sampling or percentile approximation.

### Stage 10 — Parallel output generation

**Problem:** Processing and formatting every final group sequentially adds avoidable work after aggregation.

**Optimization:** Divided the dense output domain into batches of 2048 groups.

Implementation:
- Created multiple output buffers.
- Used an atomic counter to distribute batches between workers.
- Calculated percentiles and formatted populated groups in parallel.
- Stored output in batch order.
- Wrote the completed buffers sequentially.

A custom integer formatting function was also introduced to avoid repeated formatting-library overhead.

**Why it helps:**
- Parallelizes final percentile processing and formatting.
- Avoids concurrent writes to the same output buffer.
- Preserves deterministic output ordering.
- Reduces formatting overhead.

---

## 3. Final Architecture

The final implementation follows this processing pipeline:

```text
              requests.log
                    |
                    v
             Memory Mapping
                 (mmap)
                    |
                    v
          Newline-Aligned Splitting
                    |
          +---------+---------+
          |         |         |
          v         v         v
        Worker 1  Worker 2  Worker N
          |         |         |
          v         v         v
      Thread-Local Parsing and Aggregation
          |         |         |
          +---------+---------+
                    |
                    v
          Global Endpoint Ranking
                    |
                    v
          Dense Hour/Endpoint Grid
                    |
                    v
          Merge Thread-Local Groups
                    |
                    v
          Exact Percentile Selection
                    |
                    v
          Parallel Batch Formatting
                    |
                    v
              summary.txt
```

---

## 4. Performance Results

### Reported benchmark run

| Metric | Result |
|---|---:|
| Valid records processed | 3,100,000 |
| Invalid records | 0 |
| Execution time | 0.4561 seconds |
| Throughput | 6,797,219 records/second |
| Peak RAM | 47.41 MiB |
| Reported CPU usage | 38% |
| Reported RAM usage | 47 MiB |

### Reference figures

| Metric | Reference |
|---|---:|
| Execution time | 41.2 seconds |
| Peak memory | 2940 MB |

**Benchmark note:** The reference figures and our reported run should not be treated as a controlled speedup comparison unless they were measured using the same hardware, input and execution conditions. The 0.4561-second result is the reported run, not a guaranteed result on every machine.

---

## 5. Correctness and Validation

Performance improvements are useful only when the output remains correct.

The implementation preserves several important properties:

- Complete records are processed without splitting lines between workers.
- Endpoint equality uses full string comparison after hash matching.
- Thread-local aggregation is merged into the final hour/endpoint grid.
- Percentiles use exact nearest-rank selection.
- Error counts are accumulated across workers.
- Output is generated in deterministic hour and endpoint order.
- The final output retains the required summary fields.

Validation should include:

- Comparing output against the reference implementation.
- Checking SHA-256 output equality.
- Verifying record counts.
- Verifying error counts.
- Checking P50, P95 and P99 values.
- Testing empty and small input files.
- Repeating benchmark runs to check consistency.

---

## 6. Key Optimization Principles

The main performance principles applied in this implementation were:

| Optimization | Main benefit |
|---|---|
| `mmap()` | Reduced input-copying overhead |
| Multithreading | Parallel record processing |
| Thread-local aggregation | Reduced synchronization |
| `std::string_view` | Reduced string allocations |
| Custom hash table | Lightweight endpoint lookup |
| Direct indexing | Reduced group lookup overhead |
| Reserved vectors | Reduced reallocations |
| Move semantics | Reduced copying |
| `std::nth_element()` | Faster exact percentile selection |
| Batch processing | Parallel finalization |
| Custom integer formatting | Reduced formatting overhead |

---

## 7. Conclusion

The final Track A implementation focuses on reducing overhead at every stage of the data-processing pipeline.

Rather than relying on a single optimization, it combines memory-mapped input, parallel parsing, thread-local aggregation, compact indexing, allocation reduction and exact percentile selection.

The central design decision was to minimize shared state during the expensive parsing stage and postpone merging until the workers had completed their independent processing.

The result is a high-throughput C++ implementation designed to process millions of log records with low reported memory consumption while preserving exact aggregation and percentile requirements.

**Optimization philosophy: Less copying. Less allocation. Less synchronization. More useful work per CPU cycle.**
