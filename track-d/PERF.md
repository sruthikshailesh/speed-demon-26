# Track D — Performance Optimization Journey

## 1. Overview

**Track:** D — Traverse  
**Language:** C++  
**Build configuration:** Source code provided; exact compiler flags should be recorded from the final build.  
**Input files:** `graph.txt`, `queries.txt`  
**Output file:** `distances.txt`

### Objective

Compute exact directed shortest-path distances for the supplied source-target queries.

The implementation is designed around:

- Large graph input.
- Efficient graph parsing.
- Compact adjacency storage.
- Parallel graph construction.
- Improved graph memory locality.
- Repeated queries sharing source vertices.
- Efficient shortest-path computation.
- Correct handling of distances exceeding 32-bit range.

**Optimization direction:** Minimize graph construction overhead, improve memory access patterns, reuse shortest-path searches and avoid exploring unnecessary parts of the graph.

---

## 2. Optimization Journey

### Stage 1 — Understanding the workload

The first step was examining the graph structure and query workload.

The implementation needs to process a directed weighted graph and answer multiple source-target shortest-path queries.

The main challenges are:

1. Reading a large graph efficiently.
2. Constructing a compact adjacency representation.
3. Reducing graph traversal overhead.
4. Avoiding repeated searches from identical source vertices.
5. Reducing priority-queue overhead.
6. Handling large shortest-path distances correctly.
7. Using multiple CPU cores effectively.

**Optimization direction:** Optimize both graph preprocessing and shortest-path execution rather than focusing only on Dijkstra's algorithm.

### Stage 2 — Memory-mapped graph input

**Problem:** Reading a large graph through repeated input operations can introduce unnecessary overhead.

**Optimization:** Used `mmap()` to map `graph.txt` directly into memory.

The `map_file()` function:

1. Opens the input file.
2. Retrieves its size using `fstat()`.
3. Maps the file using `mmap()`.
4. Returns a pointer to the mapped data.

The graph is then parsed directly from memory.

After graph parsing and construction, the mapping is released using `munmap()`.

**Why it helps:**
- Avoids explicitly copying the entire graph file into another buffer.
- Allows direct pointer-based parsing.
- Reduces application-level input buffering.
- Supports efficient partitioning of the input data.

### Stage 3 — Fast integer parsing

**Problem:** Standard formatted input functions can introduce overhead when parsing millions of numeric values.

**Optimization:** Implemented a custom unsigned decimal parser.

The `num()` function:

- Skips whitespace.
- Reads consecutive decimal digits.
- Builds the integer directly.
- Advances the input pointer.

The `skip()` function handles spaces, tabs, carriage returns and newlines.

**Why it helps:**
- Avoids repeated formatted input operations.
- Reduces parsing overhead.
- Uses direct pointer advancement.
- Keeps the graph parser simple.

### Stage 4 — Parallel graph partitioning

**Problem:** Parsing the entire graph sequentially limits graph construction throughput.

**Optimization:** Divided the graph input into approximately equal-sized byte ranges.

Each worker receives a portion of the mapped graph.

Partitions are aligned to newline boundaries so that individual edge records remain intact.

Each worker parses edge records containing:

- Source vertex.
- Destination vertex.
- Edge weight.

**Why it helps:**
- Allows graph parsing to happen concurrently.
- Reduces sequential input-processing work.
- Creates independent worker-local edge collections.
- Avoids synchronization during ordinary edge parsing.

### Stage 5 — Worker-local edge buckets

**Problem:** Multiple workers inserting edges into shared graph structures would require synchronization and could create contention.

**Optimization:** Introduced a two-dimensional collection of edge buckets:

`B[T * T]`

Each parsing worker has separate destination buckets.

Edges are assigned to buckets according to the source vertex's destination partition.

**Why it helps:**
- Avoids a shared insertion point.
- Reduces contention during graph construction.
- Groups edges by the vertex range that will own their CSR rows.
- Makes the later CSR construction easier to parallelize.

After the buckets have been consumed, their allocated storage is released.

### Stage 6 — Compressed Sparse Row (CSR) construction

**Problem:** A graph represented using many independent adjacency vectors can introduce allocation overhead and poor memory locality.

**Optimization:** Constructed the graph using Compressed Sparse Row storage.

The graph uses:

- `off[]` to store adjacency row boundaries.
- `arc[]` to store destination vertices and edge weights.

Each adjacency list is represented as a contiguous range:

`arc[off[u] ... off[u + 1])`

The implementation first counts outgoing edges for each vertex, calculates row boundaries, and then fills the final edge array.

**Why it helps:**
- Stores graph edges contiguously.
- Avoids one allocation per vertex.
- Improves adjacency traversal locality.
- Reduces pointer indirection.
- Provides efficient neighbor iteration.

The CSR representation is especially useful because Dijkstra repeatedly scans outgoing edges.

### Stage 7 — Parallel CSR row construction

**Problem:** Building all adjacency rows sequentially would leave significant graph preprocessing work on one CPU core.

**Optimization:** Assigned separate vertex ranges to different workers.

Each worker:

1. Counts outgoing edges for its assigned vertices.
2. Calculates row boundaries.
3. Fills the corresponding adjacency rows.
4. Releases consumed edge buckets.

The destination partitions are organized so that workers can write into their assigned CSR regions.

**Why it helps:**
- Parallelizes adjacency construction.
- Reduces shared-write contention.
- Produces a compact final graph representation.
- Separates graph construction by vertex range.

### Stage 8 — Graph relabeling using parallel BFS

**Problem:** The original vertex numbering may not provide good memory locality during graph traversal.

Vertices that are frequently reached from one another may have IDs that are far apart.

**Optimization:** Introduced a parallel graph relabeling phase based on breadth-first traversal.

The implementation:

1. Divides the original vertex range into regions.
2. Starts a traversal from each region.
3. Uses an atomic compare-and-exchange operation to claim previously unvisited vertices.
4. Records discovered vertices in worker-local BFS order.
5. Calculates the new vertex-ID ranges.
6. Builds a mapping from old IDs to new IDs.
7. Reconstructs the graph using the new IDs.

**Why it helps:**
- Attempts to place related traversal vertices closer together.
- Can improve adjacency and metadata access locality.
- Reduces some irregular memory-access costs.
- Provides a reordered graph for the shortest-path phase.

The relabeling is performed before queries are parsed into their final internal vertex IDs.

### Stage 9 — Rebuilding CSR after relabeling

**Problem:** Changing vertex IDs requires the adjacency structure to be updated as well.

**Optimization:** Allocated a new CSR representation and rebuilt every edge using the old-to-new vertex mapping.

For each reordered vertex:

- Its adjacency row is assigned a new offset.
- Every destination vertex is translated using `perm[]`.
- Edge weights are preserved.
- The original graph arrays are released after reconstruction.

The `hasin` metadata is also transferred to the new vertex numbering.

**Why it helps:**
- Keeps the graph representation consistent after relabeling.
- Preserves directed edge relationships.
- Produces a contiguous graph structure in the new numbering.
- Releases the original CSR storage once it is no longer required.

### Stage 10 — Early query filtering

**Problem:** Some source-target queries can be resolved without running Dijkstra.

**Optimization:** Added inexpensive checks before scheduling graph searches.

The implementation immediately handles:

- Source equals target: distance is zero.
- Target has no incoming edge from another vertex: unreachable from a different source.
- Source has no outgoing edges: no different target can be reached.

Queries that pass these checks are added to the search workload.

**Why it helps:**
- Avoids unnecessary shortest-path searches.
- Reduces the number of scheduled jobs.
- Handles simple cases with constant-time checks.

The `hasin` flag is built during graph construction and retained through graph relabeling.

### Stage 11 — Grouping queries by source

**Problem:** Running Dijkstra independently for every query repeats substantial graph traversal when multiple queries share a source vertex.

**Optimization:** Sorted queries by source and then target vertex.

Queries with the same source are grouped into a single `Group`.

Each group contains:

- Source vertex.
- First query position.
- Last query position.

The shortest-path search processes all requested targets in the group.

Once every target has been settled, the search terminates early.

**Why it helps:**
- Reuses one search for multiple targets.
- Avoids repeating exploration from the same source.
- Reduces unnecessary graph traversal.
- Makes early termination possible.

This is one of the most important algorithmic optimizations in the implementation.

### Stage 12 — Early-exit Dijkstra

**Problem:** Standard single-source Dijkstra continues until the entire reachable graph has been processed.

However, the workload only requires distances to a limited set of requested targets.

**Optimization:** Modified the search to stop when every target in the current source group has been settled.

The algorithm:

1. Starts from the group's source vertex.
2. Extracts the minimum-distance candidate.
3. Ignores stale queue entries.
4. Checks whether the extracted vertex is a requested target.
5. Records its shortest distance.
6. Stops once all requested targets have been settled.
7. Otherwise, relaxes its outgoing edges.

**Why it helps:**
- Avoids exploring unnecessary graph regions.
- Stops as soon as the required answers are available.
- Makes search work dependent on requested targets rather than requiring a complete single-source traversal.

### Stage 13 — Monotone radix heap

**Problem:** A conventional binary heap introduces logarithmic insertion and extraction costs.

Dijkstra's algorithm processes nondecreasing distance keys, making it suitable for a monotone integer priority queue.

**Optimization:** Implemented a radix heap with 65 buckets.

The queue groups entries according to the highest differing bit between the distance and the last extracted distance.

The implementation:

- Inserts distance-vertex pairs into radix buckets.
- Extracts directly from bucket zero when possible.
- Finds the next nonempty bucket when bucket zero is empty.
- Finds the minimum key in that bucket.
- Redistributes its entries.
- Continues processing in nondecreasing distance order.

**Why it helps:**
- Uses the monotonicity of Dijkstra's distance extraction.
- Avoids a conventional binary-heap structure.
- Provides an integer-key-oriented priority queue.
- Can reduce priority-queue overhead for suitable workloads.

### Stage 14 — Complemented distance map

**Problem:** A distance array needs a way to distinguish an unvisited vertex from a vertex whose distance is zero.

**Optimization:** Stores complemented distances.

The implementation uses:

- Zero to represent an unseen vertex.
- Bitwise-complemented distance values for visited vertices.

The source is initialized with the complemented representation of zero.

Distance comparisons are performed against the complemented values.

**Why it helps:**
- Avoids a separate visited array.
- Combines visitation state and distance storage.
- Reduces per-search metadata requirements.

The `touched` vector tracks vertices whose distance entries must be reset after the search.

### Stage 15 — Reusing search memory

**Problem:** Clearing a full distance array after every search would require work proportional to the total number of vertices, even when a search visits only a small part of the graph.

**Optimization:** Introduced a worker-local `touched` vector.

Whenever a vertex is first discovered, it is added to this vector.

At the end of the search, only touched vertices are reset.

The radix-heap buckets are also cleared for reuse.

**Why it helps:**
- Avoids full-array clearing between searches.
- Makes cleanup proportional to the number of visited vertices.
- Reuses allocated search structures.
- Reduces repeated memory initialization work.

### Stage 16 — Parallel shortest-path jobs

**Problem:** Independent source groups can be processed concurrently.

**Optimization:** Distributed source-group jobs among worker threads.

Each worker owns its own `Search` instance, including:

- Distance storage.
- Touched vertices.
- Radix-heap buckets.
- Search counters.

The implementation uses atomic job cursors to distribute work dynamically within assigned job ranges.

**Why it helps:**
- Executes independent source groups concurrently.
- Avoids sharing mutable search state.
- Allows workers to reuse their own search memory.
- Reduces the impact of uneven job durations compared with fixed one-job-per-thread assignment.

The number of workers is determined from the available CPU affinity where supported, with an optional `SD_T` override.

### Stage 17 — 32-bit and 64-bit distance handling

**Problem:** Using 64-bit distances for every search increases distance-storage and queue-key costs, even when ordinary shortest paths fit within 32 bits.

However, silently overflowing a 32-bit distance would produce incorrect results.

**Optimization:** Implemented a two-stage distance strategy.

**First pass:**

- Runs Dijkstra with 32-bit distance values.
- Detects when a candidate distance exceeds the representable range.
- Marks the corresponding source group as requiring a wider search.

**Second pass:**

- Collects only groups marked as overflowed.
- Reruns those groups using 64-bit distances.

**Why it helps:**
- Keeps the common search path compact.
- Avoids using wider distance storage for every search.
- Preserves correctness for larger distances.
- Limits the cost of 64-bit searches to groups that require them.

### Stage 18 — Cache prefetching

**Problem:** Graph traversal involves irregular adjacency and metadata accesses.

The next vertices and their adjacency rows may not already be present in CPU cache.

**Optimization:** Added compiler prefetch hints at selected traversal points.

Prefetching is used for:

- Future CSR row offsets.
- Future adjacency entries.
- Future vertex metadata during BFS relabeling.
- Selected adjacency data during Dijkstra.

**Why it helps:**
- Attempts to bring data into cache before it is needed.
- Can reduce memory-stall time for suitable access patterns.
- Targets the irregular memory-access behavior of graph algorithms.

Prefetching is workload- and hardware-dependent, so its actual benefit must be verified through benchmarking.

### Stage 19 — Output construction

**Problem:** Repeated formatted output operations can add overhead when writing many query results.

**Optimization:** Builds the complete output in a character buffer.

The implementation:

1. Reserves output capacity.
2. Converts each distance to decimal manually.
3. Writes `-1` for unreachable results.
4. Appends one newline per query.
5. Writes the resulting buffer using a low-level file descriptor.

The output is written in original query order because answers are stored by their original query index.

**Why it helps:**
- Avoids repeated stream formatting.
- Reduces output-system-call overhead.
- Preserves query order.
- Produces a compact output buffer.

---

## 3. Final Architecture

```text
                  graph.txt
                      |
                      v
                   mmap()
                      |
                      v
               Parallel Parsing
                      |
          +-----------+-----------+
          |           |           |
          v           v           v
       Worker 1    Worker 2    Worker N
          |           |           |
          +-----------+-----------+
                      |
                      v
              Worker-local Buckets
                      |
                      v
             Parallel CSR Construction
                      |
                      v
                Initial Graph
                      |
                      v
              Parallel BFS Relabeling
                      |
                      v
              Old-to-New ID Mapping
                      |
                      v
              Rebuilt CSR Graph
                      |
                      v
                  queries.txt
                      |
                      v
                Query Translation
                      |
                      v
                 Early Filtering
                      |
                      v
              Group Queries by Source
                      |
                      v
             Parallel Source-group Jobs
                      |
                      v
              Early-exit Dijkstra
                      |
                      v
                 Radix Heap
                      |
              +-------+-------+
              |               |
              v               v
          32-bit pass     Overflow groups
                              |
                              v
                         64-bit rerun
              |               |
              +-------+-------+
                      |
                      v
                Result Buffer
                      |
                      v
                 distances.txt
```

---

## 4. Performance Optimization Principles

| Optimization | Main benefit |
|---|---|
| Memory-mapped graph input | Avoids explicit full-file copying |
| Custom integer parser | Reduces parsing overhead |
| Parallel input partitioning | Uses multiple CPU cores |
| Worker-local edge buckets | Reduces shared-write contention |
| CSR graph representation | Compact adjacency storage |
| Parallel CSR construction | Faster graph preprocessing |
| Parallel BFS relabeling | Attempts to improve graph locality |
| Query pre-filtering | Avoids unnecessary searches |
| Source grouping | Reuses shortest-path searches |
| Early-exit Dijkstra | Avoids unnecessary exploration |
| Monotone radix heap | Optimizes integer priority-queue operations |
| Complemented distance map | Combines visited state and distance storage |
| Touched-vertex reset | Avoids clearing the entire distance array |
| Worker-local search state | Avoids shared mutable search structures |
| 32-bit fast path | Reduces common-case distance storage |
| 64-bit overflow rerun | Preserves large-distance correctness |
| Cache prefetching | Attempts to reduce memory stalls |
| Buffered output | Reduces formatting and write overhead |

---

## 5. Correctness and Validation

Performance improvements must not change shortest-path results.

### Graph correctness

- Directed edges must retain their original direction.
- Edge weights must remain unchanged.
- Every edge must appear in the correct adjacency row.
- Vertex relabeling must preserve graph connectivity.
- The old-to-new mapping must be applied consistently to queries.

### Shortest-path correctness

- Dijkstra must process nonnegative edge weights.
- Stale priority-queue entries must be ignored.
- A target is finalized only when its minimum-distance entry is extracted.
- A source-target query with identical vertices returns zero.
- Unreachable targets return `-1`.

### Distance-range correctness

- 32-bit overflow must be detected.
- Overflowed source groups must be rerun using 64-bit distances.
- No valid large distance may be silently truncated.

### Query correctness

- Queries sharing a source must all receive their correct answers.
- Early termination must happen only after all requested targets are settled.
- Answers must remain associated with their original query indices.

### Recommended validation

1. Compare all output lines against the reference implementation.
2. Verify SHA-256 equality.
3. Test a graph with no edges.
4. Test isolated vertices.
5. Test source equal to target.
6. Test unreachable targets.
7. Test multiple queries sharing one source.
8. Test directed edges where reverse paths do not exist.
9. Test zero-weight edges if permitted by the specification.
10. Test large shortest-path distances.
11. Test different `SD_T` values.
12. Compare results with and without graph relabeling.

---

## 6. Build and Execution

### Runtime invocation

```bash
./solution --data /data --out /out
```

### Input files

```text
/data/graph.txt
/data/queries.txt
```

### Expected output

```text
/out/distances.txt
```

### Thread configuration

The implementation supports:

```bash
SD_T=8
```

This overrides the detected worker count when set to a nonzero value.

The default worker count is based on hardware concurrency and, on Linux, the process CPU affinity mask.

---

## 7. Conclusion

The Track D implementation optimizes both graph preprocessing and shortest-path query execution.

Rather than treating every query as an independent Dijkstra run, it groups queries by source and shares the search across requested targets.

The main architectural decisions were:

- Memory-mapped graph parsing.
- Parallel edge collection.
- Compact CSR graph storage.
- Parallel graph relabeling.
- Source-grouped query execution.
- Early-exit Dijkstra.
- Monotone radix-heap priority queues.
- Worker-local search memory.
- Selective 64-bit distance reruns.
- Buffered output generation.

The central design principle was to reduce repeated work and improve the efficiency of graph traversal.

**Optimization philosophy: Build the graph efficiently. Reuse searches wherever possible. Stop when the required answers are found. Keep memory access and distance handling efficient.**
