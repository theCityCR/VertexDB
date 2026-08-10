## Summary — 2026-08-10

Illustrative snapshot (not the CI shape gate). Google Benchmark context: 4 logical CPUs, 2888 MHz, `release` build, host `runnervmvrwv9`. Times are median CPU time from this JSON.

| Benchmark | Arg | CPU time |
| --- | ---: | ---: |
| `BM_IndexedPointLookup` | 1,000 | ~166 ns |
| `BM_IndexedPointLookup` | 100,000 | ~163 ns |
| `BM_FilteredSelect` | 1,000 | ~3.2 µs |
| `BM_ConcurrentPointLookups` | 1 | ~29 µs |
| `BM_ConcurrentPointLookups` | 4 | ~80 µs |
| `BM_CteIndexedWinSelect` | 1,000 | ~1.82 µs |
| `BM_CteMaterializedSelect` | 1,000 | ~36.5 ms |
| `BM_TransactionRollback` | 1,000 | ~5.5 s |
