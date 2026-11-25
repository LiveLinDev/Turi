# Benchmark results

AMD64 Family 25 Model 97 Stepping 2, AuthenticAMD · Windows 10 · 9 runs, minimum

Turi vs expert C over 6 benchmarks: geometric mean 1.127x (3 wins, 1 ties within 2 %, 2 losses). Above 1.00x means Turi is faster.

| Benchmark | Language | Min time (ms) | vs C | Peak memory (MiB) | Output |
|---|---|---:|---:|---:|---|
| binarytrees | turi | 48.1 | 0.75x | 11.6 | same |
| binarytrees | c | 36.2 |  | 8.1 | same |
| binarytrees | cpp | 555.8 | 0.07x | 11.8 | same |
| binarytrees | rust | 484.6 | 0.07x | 12.2 | same |
| binarytrees | go | 264.9 | 0.14x | 17.8 | same |
| fannkuch | turi | 142.9 | 1.09x | 4.0 | same |
| fannkuch | c | 155.5 |  | 4.0 | same |
| kmers | turi | 39.8 | 1.00x | 17.4 | same |
| kmers | c | 40.0 |  | 9.4 | same |
| kmers | cpp | 121.1 | 0.33x | 11.2 | same |
| kmers | rust | 108.8 | 0.37x | 14.1 | same |
| kmers | go | 94.9 | 0.42x | 16.2 | same |
| mandelbrot | turi | 181.8 | 0.97x | 4.0 | same |
| mandelbrot | c | 175.9 |  | 4.0 | same |
| nbody | turi | 482.8 | 1.08x | 4.1 | same |
| nbody | c | 520.6 |  | 4.0 | same |
| spectralnorm | turi | 89.7 | 2.38x | 4.0 | same |
| spectralnorm | c | 213.8 |  | 4.1 | same |
