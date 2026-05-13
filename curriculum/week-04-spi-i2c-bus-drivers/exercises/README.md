# Week 4 — Exercises

Three exercises this week. Each one is graded by an artifact you commit — a working firmware, a Saleae capture, a unit-test pass.

| File                                                                                   | What you build                                                                  | Estimated time |
|----------------------------------------------------------------------------------------|----------------------------------------------------------------------------------|----------------|
| [exercise-01-spi-an-oled.md](./exercise-01-spi-an-oled.md)                             | Drive an SSD1306 128×64 OLED over SPI0; render `"crunch-wire w04"`               | 2 h            |
| [exercise-02-i2c-a-bmp280.md](./exercise-02-i2c-a-bmp280.md)                           | Read BMP280 `id`, then raw pressure, then a compensated value in hPa             | 2 h            |
| [exercise-03-bus-driver-abstraction.md](./exercise-03-bus-driver-abstraction.md)       | Refactor Ex.1 + Ex.2 onto a shared `bus_t` interface; pass a host-side unit test | 3 h            |

Do them in order. Exercise 3 requires the artifacts of Exercises 1 and 2 to refactor against.

Each exercise has the same shape:
- **Goal** (one paragraph)
- **Setup** (wiring, parts, prerequisites)
- **Reading** (specific datasheet pages, RP2040 sections, UM10204 sections)
- **Steps** (numbered procedure with checkpoints)
- **Artifact** (what you commit)
- **Common faults** (the symptoms you will hit and the diagnostic)
