# Week 5 — Exercises

Three exercises this week. Each one is graded by an artifact you commit — a disassembly diff, a passing host-side unit test, a working firmware that mixes C and C++ translation units in one `.elf`.

| File                                                                                                                                | What you build                                                                  | Estimated time |
|-------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------|----------------|
| [exercise-01-gpio-template-vs-c-macro.md](./exercise-01-gpio-template-vs-c-macro.md)                                                | Build a `Gpio<Pin>` C++20 template; diff its disassembly against the C macro version | 2 h            |
| [exercise-02-cpp-spi-driver-with-concepts.md](./exercise-02-cpp-spi-driver-with-concepts.md)                                        | Re-implement Week 4's SPI driver in C++20 with `concepts` constraining the bus type | 3 h            |
| [exercise-03-mixed-c-and-cpp-translation-units.md](./exercise-03-mixed-c-and-cpp-translation-units.md)                              | Build one `.elf` where the SPI transport is C++, the BMP280 driver is C, and they call each other across `extern "C"` | 2 h            |

Do them in order. Exercise 2 builds on Exercise 1 (uses the `Gpio<Pin>` template for the CS line). Exercise 3 builds on Exercise 2 (uses the C++ SPI transport plus a stock-C BMP280 driver from Week 4).

Each exercise has the same shape:
- **Goal** (one paragraph)
- **Setup** (toolchain version, build flags, parts)
- **Reading** (specific lecture sections, ISO standard clauses, GCC manual sections)
- **Steps** (numbered procedure with checkpoints)
- **Artifact** (what you commit)
- **Common faults** (the symptoms you will hit and the diagnostic)

The artifact for *every* exercise this week includes a `objdump` excerpt showing the generated code. The discipline is "you do not trust the abstraction; you read the disassembly." If your disassembly looks wrong, the abstraction is wrong. Fix it before committing.
