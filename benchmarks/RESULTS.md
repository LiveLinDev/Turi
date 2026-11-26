# Benchmark results

AMD64 Family 25 Model 97 Stepping 2, AuthenticAMD · Windows 10 · 11 interleaved runs, minimum

Turi vs expert C over 6 benchmarks: geometric mean 1.285x (5 wins, 1 ties within 2 %, 0 losses). Above 1.00x means Turi is faster.

| Benchmark | Language | Min time (ms) | vs C | Peak memory (MiB) | Output |
|---|---|---:|---:|---:|---|
| binarytrees | turi | 25.5 | 1.48x | 4.0 | same |
| binarytrees | c | 37.7 |  | 8.1 | same |
| binarytrees | cpp | 563.4 | 0.07x | 11.8 | same |
| binarytrees | rust | 541.3 | 0.07x | 12.2 | same |
| binarytrees | go | 293.7 | 0.13x | 17.8 | same |
| fannkuch | turi | 152.7 | 1.06x | 4.0 | same |
| fannkuch | c | 161.6 |  | 4.0 | same |
| kmers | turi | 40.2 | 1.08x | 17.4 | same |
| kmers | c | 43.2 |  | 9.4 | same |
| kmers | cpp | 121.3 | 0.36x | 11.2 | same |
| kmers | rust | 113.5 | 0.38x | 14.1 | same |
| kmers | go | 97.6 | 0.44x | 16.2 | same |
| mandelbrot | turi | 184.2 | 1.00x | 4.0 | same |
| mandelbrot | c | 184.7 |  | 4.0 | same |
| nbody | turi | 487.6 | 1.09x | 4.0 | same |
| nbody | c | 529.9 |  | 4.0 | same |
| spectralnorm | turi | 91.4 | 2.46x | 4.0 | same |
| spectralnorm | c | 224.7 |  | 4.1 | same |
