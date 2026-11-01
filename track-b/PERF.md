| step | change | time (s) | RSS (MiB) | lines | PASS/FAIL |
|---|---|---:|---:|---:|---|
| 0 | baseline after steps 1-4 were already present in the source | 0.02576 | 219.5 | 1011 | PASS |
| 1 | cleanup (`SD_TIMING`, `g_max_pos`, `localmax`, `<chrono>`, `TermTable::release()`, `Index::has_positions()`, unused includes) | already present in baseline | already present in baseline | 1011 | PASS (kept) |
| 2 | single-pass query parsing (`collect_query_terms` + `parse_queries` merged) | already present in baseline | already present in baseline | 1011 | PASS (kept) |
| 3 | term-table sizing (`2^12` slots + small arena reserve; rely on grow) | already present in baseline | already present in baseline | 1011 | PASS (kept) |
| 4 | `_exit(0)` after successful `fclose` | already present in baseline | already present in baseline | 1011 | PASS (kept) |
| 5 | phrase verification: drop `present` check, forward-only cursors per term, per-thread match-buffer reuse | 0.02554 | 219.5 | 1011 | FAIL (rejected) |

The step-5 patch passed the diff harness (`tiny/sample/full` all green) and kept
`results.txt` identical to the reference, but the median time improved by only
~0.8% (0.02554 s vs 0.02576 s baseline), which is within run-to-run noise for
this workload; memory was unchanged and the code size was unchanged. The
baseline was therefore retained.

### Rejected candidate: unified mmap helper (file-input refactor)

- Change: replace the separate `read_whole_file` + `MappedFile` split with one `map_file` helper shared for docs and queries.
- Result: diff harness PASS on tiny/sample/full; official oracle hash still matched.
- Time: sample run got slower by about 18–20% under the harness (`sample` 4.5 s vs 3.8 s baseline), which is above the 2% retention gate.
- Memory: no meaningful reduction; the candidate was rejected.
- Lines: physical and scc did not change materially; not worth keeping.
- Baseline retained: [track-a2/track-b/main.cpp](/Users/sruthikshaileshr/Desktop/Hackathon/Speed%20Demon%2026/track-a2/track-b/main.cpp).

### Runtime-branch long-term scan refactor (kept, compile + custom sanity only)

- Change: replace the `kLongTerms` template specialization in `scan_chunk` and `build_index` with a single runtime `bool long_terms` branch, keeping the same slow-path logic for tokens longer than 32 bytes.
- Result: binary compiles cleanly with `g++ -O3 -std=c++17 -DNDEBUG -march=native -flto -pthread`; a custom valid BOOL/PHRASE sanity run matched the expected output pattern.
- Time: not measured on the official harness in this environment because the required `/data` docs/queries and the diff harness are unavailable here; the code path is functionally unchanged.
- Memory: unchanged within the local sanity run.
- Physical lines: reduced from 1006 to 1000 in the source file after the refactor.
- Status: PASS (compile + targeted sanity) in this sandbox; official hash/difftest not yet run because the required dataset bundle is absent.

### Reused list-equality helper (compile + custom sanity only)

- Change: add a tiny `same_list()` helper and reuse it for the repeated identity checks in `run_bool()`.
- Result: no semantic change; the build still succeeds and the same custom BOOL/PHRASE sanity set still matches the expected output.
- Time: no measured change on the official harness here; logic is unchanged and the compiler should fold the check.
- Memory: unchanged.
- Physical lines: 1002 after the refactor, down from 1006 earlier in the current branch.
- Status: PASS (compile + targeted sanity); no official diff/hash run available in this environment.

