# Challenge 2 — Aliasing and the input antialiasing filter

This challenge is half analysis and half bench measurement. The deliverable is one folder added to the repo:

```
challenges/aliasing-bench/
  README.md            <- your analysis and design choices
  filter-sweep.csv     <- the swept-sine measurement
  filter-sweep.png     <- the plot of the same
  filter-schematic.png <- your hand-drawn or KiCad schematic
```

There is no `.c` file to write; the firmware reuses Exercise 3. The work is on the wire and on paper.

---

## Background

The ADC samples at 500 kS/s on a single channel. Nyquist says the maximum bandwidth representable in the captured stream is 250 kHz. Any signal energy at the ADC input pin above 250 kHz folds (aliases) into the 0–250 kHz band and appears as a false tone at frequency `|f_input - n * f_s|` for some integer `n` such that the result lies in [0, f_s/2].

Example: with `f_s = 500 kHz`, a 600 kHz input alias appears at `|600 - 500| = 100 kHz`. A 1.3 MHz input aliases to `|1300 - 1500| = 200 kHz` (n=3). A 5 MHz harmonic from a nearby switching regulator aliases to `|5000 - 5000| = 0` (a DC offset) or, more typically with phase noise on the offending source, smears into a band near DC.

Without a filter on the input, your "audio capture" picks up every switching transient on the board and presents it to you as a confused soup of false tones. With a properly designed filter, the input is band-limited and the captured spectrum reflects the actual audio.

---

## Part A — Compute the filter

Design a first-order RC low-pass with a corner frequency at 100 kHz. The transfer function is:

$$H(f) = \frac{1}{1 + j \, f / f_c}$$

with

$$f_c = \frac{1}{2\pi R C}$$

**Question 1.** Pick R = 1.6 kΩ. Compute C such that f_c = 100 kHz. Round to the nearest E12 series value (10, 12, 15, 18, 22, 27, 33, 39, 47, 56, 68, 82) and recompute the actual corner.

**Question 2.** With your rounded values, compute the attenuation at:
- 250 kHz (the captured Nyquist)
- 500 kHz (the sample rate)
- 1 MHz (a typical switching-regulator fundamental)
- 5 MHz (a typical switching-regulator third harmonic)

Express each in dB and as a fraction (e.g. "0.32 of input amplitude").

**Question 3.** Is a first-order filter enough? Define "enough" as: signal energy at 5 MHz must be attenuated to below 1 LSB at the ADC (3.3 V / 4096 = 0.806 mV). Assume the switcher injects 50 mV peak at 5 MHz. Show your calculation. If first-order is not enough, propose a second-order (Sallen-Key with one op-amp) and recompute.

---

## Part B — Build the filter

Wire R and C between your signal source and GP26.

```
signal_source -----[R]-----+----- GP26 (ADC0)
                           |
                          [C]
                           |
                          GND
```

The capacitor goes from the GP26 node to ground. Use ceramic (X7R or C0G) — electrolytic is no good at 100 kHz. Keep the leads short; lead inductance at 5 MHz is ~30 nH per cm, which forms an L-C with the input capacitance of the ADC and changes the corner.

**Sanity check before sweep.** Drive the input with a 10 kHz, 1 Vpp sine. Run the Exercise 3 firmware. Expect the consumer to log roughly constant RMS values (since 10 kHz is well within the passband). If the RMS is wildly noisy, the filter is wrong or the capacitor is the wrong dielectric.

---

## Part C — Sweep the input

Drive the filter with a function generator producing a 1 Vpp sine. Sweep the frequency in decades:

- 100 Hz, 1 kHz, 10 kHz, 100 kHz, 250 kHz, 500 kHz, 1 MHz, 2 MHz, 5 MHz

For each frequency, capture 10 chunks (about 20 ms of data at 500 kS/s) and compute the RMS in the consumer task. The signal generator's output amplitude is the input; the captured RMS is the output. Plot:

- x-axis: log(f) from 100 Hz to 5 MHz
- y-axis: 20*log10(output / input) in dB

Expected shape: flat from 100 Hz to about 60 kHz (-3 dB at the corner), -20 dB/decade rolloff above. At 1 MHz the output should be ~20 dB below input; at 5 MHz, ~34 dB below. If you see a *rising* response above the corner (e.g. a bump at 200 kHz), the filter has a parasitic resonance — likely a too-long capacitor lead.

**The interesting frequency**: 500 kHz. The sample rate is exactly 500 kHz; a perfect filter would give zero output (the alias of 500 kHz lands at DC, and DC has its own block-cap if you have one upstream). In practice you will see -6 dB or so; that energy is what aliases. Report what you see.

Save the data as `filter-sweep.csv` and the plot as `filter-sweep.png`.

---

## Part D — The post-mortem

Write a one-page `README.md` in `challenges/aliasing-bench/` answering:

1. **What corner frequency did you pick, and why?** (You should have justified 100 kHz in terms of the signal you want to capture vs the Nyquist; this is the architectural choice that drives the filter design.)
2. **What did the swept-sine measurement actually show?** Compare measured to theoretical. Highlight any deviation > 3 dB and explain it.
3. **What is the practical aliasing risk in your bench setup?** Identify the strongest source of energy above 250 kHz on your bench (typical answers: the Pico's 1.2 V switcher at ~1.2 MHz, USB high-speed transitions at multiples of 480 MHz aliasing down via probe pickup, a USB-charger hash at 100–300 kHz). Did your filter handle it?
4. **If you were doing this for production**, would you use this filter or upgrade to second-order? Justify in terms of the signal-to-alias-noise budget you computed in Part A Question 3.

The README is one page of prose; the CSV and PNGs are the evidence. Commit all three.

---

## Pass criterion

You pass when the swept-sine plot, the schematic, and the README are all in the repo, and the README correctly identifies at least one real bench-level aliasing source and explains how the filter addresses (or fails to address) it. The grading concern is engineering judgement, not curve fit; even a measurement that shows the filter underperforming is acceptable if the post-mortem identifies why.

---

## A note on cost

This challenge requires:
- 1 resistor (any value near 1.6 kΩ, ~0.01 USD)
- 1 capacitor (ceramic, any value computed in Part A, ~0.05 USD)
- A function generator (or a fellow student's, or a 555 timer + comparator wired as a square-to-sine on a breadboard ~0.50 USD in parts)

Total bench cost is under a dollar if you have a generator; under five dollars if you build a generator. The challenge is in the engineering, not the parts.
