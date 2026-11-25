# Benchmark results

AMD64 Family 25 Model 97 Stepping 2, AuthenticAMD · Windows 10 · 9 runs, minimum

Turi vs expert C over 6 benchmarks: geometric mean 1.170x (4 wins, 1 ties within 2 %, 1 losses). Above 1.00x means Turi is faster.

| Benchmark | Language | Min time (ms) | vs C | Peak memory (MiB) | Output |
|---|---|---:|---:|---:|---|
| binarytrees | turi | 54.9 | 0.77x | 11.6 | same |
| binarytrees | c | 42.5 |  | 8.1 | same |
| binarytrees | cpp | 629.9 | 0.07x | 11.8 | same |
| binarytrees | rust | 609.0 | 0.07x | 12.2 | same |
| binarytrees | go | 311.9 | 0.14x | 23.6 | same |
| fannkuch | turi | 150.2 | 1.07x | 4.0 | same |
| fannkuch | c | 160.7 |  | 4.0 | same |
| kmers | turi | 42.6 | 1.12x | 17.4 | same |
| kmers | c | 47.7 |  | 9.4 | same |
| kmers | cpp | 162.3 | 0.29x | 11.2 | same |
| kmers | rust | 763.8 | 0.06x | 14.0 | same |
| kmers | go | 226.3 | 0.21x | 16.3 | same |
| mandelbrot | turi | 198.7 | 1.08x | 4.0 | same |
| mandelbrot | c | 214.8 |  | 4.0 | same |
| nbody | turi | 547.2 | 1.01x | 4.1 | same |
| nbody | c | 552.9 |  | 4.0 | same |
| spectralnorm | turi | 97.0 | 2.53x | 4.0 | same |
| spectralnorm | c | 245.0 |  | 4.1 | same |
