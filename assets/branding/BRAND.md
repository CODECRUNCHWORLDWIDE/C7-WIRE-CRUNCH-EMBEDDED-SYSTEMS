# C7 · Crunch Wire — Brand Guide

> **Voice:** datasheet-grade. Specific, sober, hardware-honest.
> **Feel:** the engineering bench at 11 PM — logic analyzer humming, scope on the corner, oscilloscope grid showing through everything.

Extends the family brand. C7-specific overrides only.

---

## Identity

- **Full name:** Crunch Wire — Embedded Systems & IoT
- **Program code:** C7
- **Full title in copy:** *C7 · Crunch Wire — Embedded Systems & IoT*
- **Tagline (short):** Firmware that doesn't lie to you.
- **Tagline (long):** A free, open-source 24-week semester on embedded firmware, microcontrollers, and connected devices — from blank linker scripts to signed OTA updates and a watchdog you can prove works.
- **Canonical URL:** `codecrunchglobal.vercel.app/course-c7-embedded`
- **License:** GPL-3.0

---

## Where C7 diverges from the family palette

Inherits Ink/Parchment/Gold. Adds a single restrained orange — the **Trace Orange** of an oscilloscope's analog channel:

| Role | Name | Hex | Use |
|------|------|-----|-----|
| Accent | Trace Orange | `#FB923C` | Highlights for signal edges, the C7 mark, "high attention" status badges |
| Accent deep | Trace Orange deep | `#C2410C` | Hover states, eyebrows on parchment |
| Accent soft | Trace Orange soft | `#FED7AA` | Subtle warnings, "tolerance window" overlays in diagrams |
| Grid | Scope Grid | `#1F2937` | The faint grid behind hero / code surfaces |

```css
:root {
  --trace:       #FB923C;
  --trace-deep:  #C2410C;
  --trace-soft:  #FED7AA;
  --scope-grid:  #1F2937;
}
```

The reason for orange specifically: most consumer hardware-tutorial branding leans toward "Arduino blue" or "Raspberry Pi raspberry." Trace Orange signals *we are building things that move signals*, not "look how friendly hardware is."

### Typography

Same family — but **every voltage, frequency, pin number, register address, datasheet page citation, and clock division ratio is rendered in JetBrains Mono.** Non-mono physical quantities in body text read as "approximate" — mono reads as "spec."

---

## Recurring page elements

### The "signal envelope"

A small inline visualization element used in lectures and challenges. Shows the timing or voltage envelope a peripheral must meet:

```
   3.3V ──┐                    ┌──── VOH min
          │                    │
          │     ▁▁▁▁▁▁▁▁▁     │
          │   ▁▁         ▁▁   │
          │ ▁▁             ▁▁ │
   0.0V ──┴──                ──┴──── VOL max

         t-rise              t-fall
         ≤ 50 ns             ≤ 50 ns
```

Always JetBrains Mono. Always show the *limit* not just the typical. Inline in MD with code-fenced ASCII art; in HTML, a small SVG.

### The "fault model" card

Every connected-device design must include a **fault model** at submission. The recurring card shape:

```
┌───────────────────────────────────────────────────────────┐
│  FAULT MODEL — capstone-sensor-mesh                       │
│                                                           │
│  Power loss mid-write:    handled via dual-bank bootloader│
│  Wi-Fi drop > 30s:        local store-and-forward queue   │
│  Sensor stuck-at-zero:    hysteresis + timeout            │
│  Battery low:             graceful broadcast + sleep      │
│  Firmware corruption:     rollback to last known good     │
└───────────────────────────────────────────────────────────┘
```

This card is C7's signature. A capstone without one is incomplete.

---

## Voice rules

- **Specify units always.** "3.3V at the MCU pin, after 100 nF decoupling" — not "the right voltage."
- **Cite the page.** "STM32F4 RM0090, §4.3.5" — not "the datasheet says."
- **Distinguish typical from limit.** Real engineers don't conflate them. Neither does our copy.
- **Avoid the word "smart" applied to objects.** "Connected thermostat," not "smart thermostat."
- **No "internet of things" as marketing.** "IoT" or "connected fleet of devices" — never the full phrase used as branding.
- **Power matters.** Every example acknowledges power draw or sleep behavior, even when the lesson is about something else.

---

## Course page conventions

The course page for C7 uses the inverted variant (Slate background, Parchment text, Trace Orange accent) with a faint Scope Grid overlay. The 24-week ladder appears as a "timeline trace" — like a logic-analyzer capture across the semester.

---

*GPL-3.0. Fork freely.*
