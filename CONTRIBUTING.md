# Contributing to Turi (`veloz`)

Thanks for contributing! Turi follows three strict engineering invariants:

1. **Preserve memory & numerical safety.** Never bypass bounds checks, overflow guards, lifetime/alias checks, or IEEE-754 floating-point semantics (`-ffp-contract=off` mindset) for benchmark gains.
2. **Keep optional runtimes zero-overhead.** Features under `lib/*` (`web`, `gpu`, `tensor`, `crypto`, `data`, `cli`, `postgres`) must add **0 bytes and 0 external symbols** when not imported/referenced (verified on `examples/fibonacci.turi`).
3. **Report honest measurements.** Keep competitor C/C++/Rust/Go code functionally equivalent and include wins, ties, losses, binary sizes, and tradeoffs.

## Local Verification Checklist

Before opening a pull request:

```powershell
# Windows Release + full 40 CTests (0 compiler warnings expected)
.\tools\build.ps1

# Windows AddressSanitizer + UndefinedBehaviorSanitizer
.\tools\build.ps1 -BuildDir build-san -Sanitize
```

On Linux / macOS:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

If you modify syntax, CLI flags, or `AGENT_CARD.md`, ensure `agent` and `turi05_ai` CTests pass and keep `AGENT_CARD.md` compact (`<= 2000` tokens).
