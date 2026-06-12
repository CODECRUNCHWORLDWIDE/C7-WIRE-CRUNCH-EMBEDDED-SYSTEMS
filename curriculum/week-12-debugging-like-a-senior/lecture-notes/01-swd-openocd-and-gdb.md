# Lecture 1 — SWD, OpenOCD, and GDB

> *For eleven weeks you have debugged the RP2040 from the outside: a UART line, a blinking LED, a USB serial port. Those are keyholes. This lecture opens the door. The two pins labeled SWCLK and SWDIO on every Cortex-M chip are a debug transport that lets a probe halt the cores between any two instructions, read and write every register and every byte of memory, set hardware breakpoints in flash you cannot otherwise modify, and watch SRAM change while the firmware runs full speed. Once you have driven one GDB-over-SWD session, printf-debugging feels like reading a book through a mail slot. This lecture is about the transport (SWD, and its older cousin JTAG), the daemon that speaks it (OpenOCD), and the debugger that drives the daemon (GDB) — and how to script the whole stack so the boring parts happen in one keystroke.*

## 1. The two wires

ARM Cortex-M parts expose debug through the **CoreSight** debug architecture, and the cheapest physical transport for it is **Serial Wire Debug (SWD)**: two pins, `SWCLK` (a clock the probe drives) and `SWDIO` (a bidirectional data line). The RP2040 brings these out on dedicated pads and routes them to the 3-pin debug header on the Pico board (RP2040 datasheet §2.3.4, "SWD", pp. 14–15). A probe on those two wires can do everything a debugger needs:

- Halt and resume the cores.
- Read and write `R0–R15`, `xPSR`, `MSP`, `PSP`, `CONTROL`, `PRIMASK` while halted.
- Read and write any memory address through the MEM-AP (the "Memory Access Port") — including peripheral registers, SRAM, and the XIP flash window.
- Set up to 4 hardware breakpoints and 2 hardware watchpoints (the RP2040's M0+ configuration; Cortex-M0+ TRM, DDI0484).
- Single-step one instruction at a time.
- Read SRAM *while the core runs*, with no halt — the mechanism RTT uses (Lecture 3).

The wire protocol is defined by the **ARM Debug Interface Architecture Specification, ADIv5.2** (IHI0031, free from ARM). You do not need to implement it — OpenOCD does — but the mental model matters: SWD packets address a small set of **Debug Port (DP)** registers (which select and configure the access ports) and **Access Port (AP)** registers (the MEM-AP, through which all memory and register access flows). When OpenOCD reads `R0`, what physically happens is: write the **Debug Core Register Selector** (`DCRSR` at `0xE000EDF4`) to request `R0`, poll the status, then read the **Debug Core Register Data** register (`DCRDR` at `0xE000EDF8`) — all of that over the MEM-AP, all of that over two wires.

The RP2040 has a wrinkle: it has **two** M0+ cores, and SWD addresses them through a *multidrop* selection (a target ID in the SWD line-reset sequence picks core 0 or core 1). OpenOCD's `target/rp2040.cfg` declares both `rp2040.core0` and `rp2040.core1`; in GDB you usually debug core 0 and leave core 1 parked. When you `monitor reset halt`, OpenOCD halts both. This dual-core multidrop is RP2040-specific and is the one thing about its SWD that differs from a single-core STM32 or SAMD.

## 2. SWD vs JTAG

You will meet **JTAG** in the field — on FPGAs, on older MCUs, on anything with a 20-pin ARM debug header — so know the contrast.

| Property                  | SWD                                  | JTAG                                              |
|---------------------------|--------------------------------------|---------------------------------------------------|
| Pins                      | 2 (`SWCLK`, `SWDIO`)                  | 4–5 (`TCK`, `TMS`, `TDI`, `TDO`, optional `TRST`) |
| Topology                  | Single target (multidrop on RP2040)  | Daisy-chain: many devices on one scan chain       |
| Standard on Cortex-M      | Yes — the default                    | Supported but legacy on most M-class parts        |
| Boundary scan (board test)| No                                   | Yes (IEEE 1149.1, its original purpose)           |
| Bandwidth                 | Comparable for debug                  | Comparable; daisy-chain adds overhead             |
| On the RP2040             | The exposed transport                | Not brought out                                   |

JTAG is older (IEEE 1149.1, designed for boundary-scan board test in the 1980s) and its TAP (Test Access Port) state machine clocks instructions and data through a chain of devices via `TMS`/`TCK`. SWD was ARM's answer to "we only have two spare pins": it carries the same ADIv5 debug semantics over two wires by multiplexing direction on `SWDIO`. For a Cortex-M, SWD gives you everything JTAG would for debug, at two pins instead of five, which is why every modern Cortex-M part defaults to it and the RP2040 does not even route JTAG. The one capability you lose is boundary scan — irrelevant for firmware debugging, essential for board bring-up test, which is why FPGAs keep JTAG. **Exam point:** the RP2040 exposes SWD, not JTAG; SWD is two wires; JTAG is the five-wire daisy-chainable transport with boundary scan.

## 3. The probe: a second Pico as a debugprobe

You do not need an expensive probe. Raspberry Pi publishes `debugprobe.uf2` — CMSIS-DAP firmware that turns a spare Pico into a USB debug probe plus a UART bridge (`raspberrypi/debugprobe`, BSD/Apache). Flash it the usual way (hold BOOTSEL, drop the UF2), and the probe Pico enumerates as a CMSIS-DAP device that OpenOCD speaks to.

Wiring (probe Pico → target Pico):

```text
Probe GP2  ->  Target SWCLK   (debug header pin 2)
Probe GP3  ->  Target SWDIO   (debug header pin 1)
Probe GND  ->  Target GND     (debug header pin 3, and any GND)
Probe GP4  ->  Target GP1     (UART bridge: probe TX -> target RX)  [optional]
Probe GP5  ->  Target GP0     (UART bridge: probe RX <- target TX)  [optional]
```

The `SWCLK`/`SWDIO`/`GND` three are mandatory for debug. The `GP4`/`GP5` UART bridge is the convenience that lets the *same* probe carry your `printf` over USB-serial while it debugs — handy, but note it is a real UART and perturbs timing exactly like any UART (which is why we prefer RTT later). Power the target from its own USB or from the probe's `VSYS`/`VBUS`; do not back-power through the debug header.

The **Raspberry Pi Debug Probe** (the $12 official product) is the same firmware in a tidy enclosure with proper connectors; a **SEGGER J-Link** is the commercial gold standard (faster, RTT-native, SystemView/Ozone support) and a drop-in for OpenOCD via `interface/jlink.cfg`. Everything in this lecture works with any of the three; we assume the cheap second-Pico probe.

## 4. OpenOCD: the daemon in the middle

**OpenOCD** (Open On-Chip Debugger, GPL-2.0) is the userspace daemon that speaks ADIv5 to the probe on one side and the **GDB Remote Serial Protocol** on a TCP port on the other. You launch it once and leave it running:

```sh
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg
```

`interface/cmsis-dap.cfg` configures the probe transport (CMSIS-DAP over USB); `target/rp2040.cfg` configures the dual-core M0+ target. On success OpenOCD prints something like:

```text
Info : CMSIS-DAP: SWD supported
Info : SWCLK/TCK = 1 SWDIO/TMS = 1 ...
Info : DAP init complete
Info : [rp2040.core0] Cortex-M0+ r0p1 processor detected
Info : [rp2040.core1] Cortex-M0+ r0p1 processor detected
Info : starting gdb server for rp2040.core0 on 3333
Info : Listening on port 3333 for gdb connections
```

Port **3333** is the GDB server. Port **4444** is OpenOCD's own Telnet command console (you can `telnet localhost 4444` and type OpenOCD commands directly — useful for `flash` and `rtt` commands without a GDB attached). Port **6666** is the Tcl RPC interface (scripting).

Inside a GDB session, any line prefixed `monitor` is passed straight through to OpenOCD. The ones you use constantly:

- `monitor reset halt` — reset the target and halt at the reset vector. The single most-used command; always start a session with it.
- `monitor reset run` — reset and let it run.
- `monitor halt` / `monitor resume` — halt/resume without reset.
- `monitor flash write_image erase firmware.elf` — program the ELF into flash (OpenOCD knows the RP2040 flash algorithm).
- `monitor rtt setup 0x20000000 0x40000 "SEGGER RTT"` then `monitor rtt start` — set up RTT (Lecture 3).

The RP2040 wrinkle to know: because flash is XIP-mapped and the M0+ has only **4 hardware breakpoints**, OpenOCD cannot transparently use software breakpoints in flash (a software breakpoint replaces an instruction with `BKPT`, which requires writing flash, which requires an erase). So on the RP2040 you mostly set **hardware** breakpoints (`hbreak` in GDB), and you are limited to four at a time. RAM-resident code (`__not_in_flash_func`) can take software breakpoints. This four-breakpoint ceiling is a real constraint you will hit and a frequent quiz target.

## 5. GDB over the remote

With OpenOCD running, attach GDB:

```sh
gdb-multiarch firmware.elf
```

then inside GDB:

```text
(gdb) target extended-remote localhost:3333
(gdb) monitor reset halt
(gdb) load
(gdb) break main
(gdb) continue
```

`target extended-remote` (not plain `remote`) keeps the connection alive across program restarts, so you can `load` again without reconnecting. `load` programs the ELF's loadable sections over SWD (faster than `monitor flash write_image` for small images, and it also sets up the symbol-to-address mapping GDB needs). `break main` then `continue` runs to `main`.

The core GDB vocabulary you live in:

```text
(gdb) break foo                 set a (software, RAM) breakpoint at foo
(gdb) hbreak foo                set a HARDWARE breakpoint (use this for flash code)
(gdb) tbreak foo                temporary breakpoint, auto-deleted after first hit
(gdb) break bar.c:42            break at a source line
(gdb) break baz if n > 100      conditional breakpoint
(gdb) watch g_counter           hardware watchpoint: halt when g_counter is WRITTEN
(gdb) rwatch g_flag             halt when g_flag is READ
(gdb) awatch g_state            halt on read OR write
(gdb) info registers            dump R0-R15, xPSR
(gdb) p/x $pc                   print the program counter in hex
(gdb) p/x $sp                   print the stack pointer
(gdb) x/16xw $sp                examine 16 words of memory at SP, in hex
(gdb) bt                        backtrace (call chain)
(gdb) step                      step one source line, INTO calls
(gdb) next                      step one source line, OVER calls
(gdb) stepi                     step one machine instruction
(gdb) finish                    run until the current function returns
(gdb) info line *0x10001234     which source line is at this address
```

The two that change how you work:

**Hardware watchpoints.** `watch g_counter` tells the M0+'s DWT comparator to halt the core the instant any instruction writes `g_counter`. This is how you find "who is corrupting this variable" — the bug class that no `printf` ever solves, because by the time the corruption is visible the culprit is long gone. You get **2** watchpoints on the RP2040 (two DWT comparators); use them deliberately. A watchpoint is not a polling loop — it is silicon comparing the address bus on every cycle, free of any timing perturbation.

**Conditional breakpoints.** `break process_packet if packet->len > 1500` halts only on the malformed input you care about, skipping the thousand well-formed packets. The condition is evaluated on the target-halt each time the breakpoint is hit, so a condition that fires rarely still pays the halt-cost on every hit — keep conditions cheap.

## 6. GDB scripting — making it one keystroke

A senior never types the same five commands twice. GDB has three layers of automation.

### `.gdbinit` and `define`

A `.gdbinit` in your project directory runs automatically on GDB launch. Put your connection boilerplate and custom commands there:

```text
# .gdbinit
target extended-remote localhost:3333
monitor reset halt

define reload
    monitor reset halt
    load
    monitor reset halt
end
document reload
    Reset, reflash the current ELF, and halt at the reset vector.
end

define regs
    info registers
    printf "PC  = 0x%08x\n", $pc
    printf "SP  = 0x%08x\n", $sp
    printf "LR  = 0x%08x\n", $lr
    printf "PSR = 0x%08x\n", $xpsr
end
```

Now `reload` and `regs` are one-word commands. (You may need `set auto-load safe-path /` once, or to add your project dir to GDB's safe-path, for `.gdbinit` to auto-run — GDB refuses to auto-load init files from untrusted directories by default.)

### Breakpoint `commands`

Attach a command list to a breakpoint so hitting it does something automatically and (optionally) continues:

```text
(gdb) break uart_isr
(gdb) commands
> silent
> printf "uart_isr: status=0x%08x\n", uart_get_status()
> continue
> end
```

This turns a breakpoint into a logging tap that prints and resumes without halting your attention — a poor man's trace, useful when you want a printf-grade log but the firmware has no working stdio. (Beware: each hit halts the core, so this *does* perturb timing — it is not for timing bugs.)

### The Python API

GDB embeds CPython. You can register new commands as `gdb.Command` subclasses and read target memory and symbols from Python. This is how the mini-project's one-keystroke postmortem works:

```python
# cc_postmortem.py  — source it with: (gdb) source cc_postmortem.py
import gdb

class Postmortem(gdb.Command):
    """Decode the stacked Cortex-M0+ exception frame at the current SP."""
    def __init__(self):
        super().__init__("postmortem", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        inferior = gdb.selected_inferior()
        sp = int(gdb.parse_and_eval("$sp"))
        # Read the 8-word hardware-stacked frame.
        raw = inferior.read_memory(sp, 32)
        import struct
        r0, r1, r2, r3, r12, lr, pc, xpsr = struct.unpack("<8I", raw)
        print(f"  stacked PC  = 0x{pc:08x}")
        print(f"  stacked LR  = 0x{lr:08x}")
        print(f"  stacked xPSR= 0x{xpsr:08x}  (exc #{xpsr & 0x1ff})")
        # Map the faulting PC to a source line.
        try:
            line = gdb.execute(f"info line *0x{pc:08x}", to_string=True)
            print("  " + line.strip())
        except gdb.error:
            print("  (no source line for that PC)")

Postmortem()
```

After a fault, `postmortem` reads the eight stacked words, unpacks them with the correct little-endian order, and decodes the faulting PC to a source line. One keystroke replaces the manual `x/8xw $sp` plus mental field-counting. We build the full version in the mini-project.

## 6a. What the GDB remote protocol actually sends

It is worth seeing one layer down, because when OpenOCD and GDB disagree you will be reading this on the wire. The GDB Remote Serial Protocol is a line of `$<payload>#<checksum>` packets over the TCP socket; the host acks each with `+`. The payloads you care about:

- `?` — "why did you stop?" Replies with a stop reason (`S05` = SIGTRAP, the breakpoint/halt signal).
- `g` — "read all registers." Replies with the hex of `R0–R15`, `xPSR` concatenated. `G<hex>` writes them.
- `m<addr>,<len>` — "read `len` bytes at `addr`." Replies with the hex of those bytes. This is the packet `cc-debug.py` uses to scan SRAM for the crash-dump magic.
- `M<addr>,<len>:<hex>` — "write these bytes." The `set var` and `load` paths use it.
- `c` / `s` — continue / single-step.
- `Z0,<addr>,<kind>` / `Z1,<addr>,<kind>` — insert a software (`Z0`) or hardware (`Z1`) breakpoint; `z0`/`z1` remove them. The RP2040's flash forces GDB toward `Z1` (hardware), which is why you see `hbreak`'s 4-comparator ceiling.
- `Z2`/`Z3`/`Z4,<addr>,<kind>` — write / read / access watchpoints, backed by the DWT comparators.

You almost never type these by hand, but two debugging situations need them: (1) writing a probe driver or a custom dump tool (`cc-debug.py` speaks the `m` packet directly, because it is simpler than scripting GDB for a raw memory scan), and (2) diagnosing a flaky probe — `set debug remote 1` in GDB prints every packet, and a corrupted reply (a checksum mismatch, a truncated `m` response) points at a wiring or a USB-bandwidth problem rather than a target bug. The protocol is documented in the GDB manual's "Remote Protocol" appendix; you need a reading knowledge, not a writing one.

## 6b. How a hardware watchpoint actually works

When you type `watch g_state`, GDB resolves `g_state` to an address and a size, then issues a `Z2` packet that tells OpenOCD to program one of the M0+'s **DWT comparators**. The DWT (Data Watchpoint and Trace unit) has, on the RP2040, **two** comparators usable as watchpoints. Each comparator holds an address, a mask (for ranges), and a function selector (match-on-write, match-on-read, match-on-access). The comparator sits on the core's load/store address bus and, on every memory access, compares the address against its programmed value in hardware — zero software cost, no polling. When a match occurs, the comparator raises a debug event that halts the core.

This is why a watchpoint is the right tool for "who corrupts this variable": it is silicon watching the bus, so it catches the *exact* store, halts before the next instruction, and `bt` names the culprit — with no timing perturbation until the moment it fires. Contrast the software alternative (single-step the whole program checking the variable after each instruction): that is thousands of times slower and so perturbing it changes every timing-dependent behavior. The hardware watchpoint is the non-perturbing version, and you get two of them; spend them deliberately. (`rwatch` matches reads, `awatch` matches either; `watch` matches writes, the common case.)

A subtlety: a watchpoint set by *symbol* on a stack local is auto-deleted by GDB when that frame returns (the address becomes meaningless). Watch globals, or watch by explicit address (`watch *(uint32_t *)0x20001234`) when you need it to persist across scope changes.

## 6c. RTT over OpenOCD, end to end

You will use RTT (Lecture 3) constantly, and the bring-up is an OpenOCD `monitor` sequence worth seeing here because it lives in the GDB session:

```text
(gdb) monitor rtt setup 0x20000000 0x42000 "SEGGER RTT"
(gdb) monitor rtt start
rtt: Searching for control block 'SEGGER RTT'
rtt: Control block found at 0x2000xxxx
(gdb) monitor rtt server start 9090 0
```

`rtt setup <ram_base> <ram_size> <magic>` tells OpenOCD where in SRAM to scan and what magic to scan for — the same `"SEGGER RTT"` string the target embedded in its control block. `rtt start` performs the scan (a sequence of `m` memory reads over SWD) and, once it finds the block, begins polling the up-channel ring. `rtt server start 9090 0` exposes channel `0` on TCP port `9090`, which you drain with `nc localhost 9090`. Because the polling is plain SWD memory reads, **the core never halts** — the log streams while the firmware runs full speed. This is the whole point of RTT and the thing a UART `printf` cannot match.

## 7. A complete first session

Putting it together — the workflow you will run a hundred times this week:

1. **Wire the probe** (SWCLK/SWDIO/GND), plug both Picos into USB.
2. **Launch OpenOCD:** `openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg` (leave it running in one terminal).
3. **Launch GDB:** `gdb-multiarch build/firmware.elf` (in another terminal; `.gdbinit` connects and resets).
4. `load` to flash, `monitor reset halt` to start clean.
5. Set breakpoints/watchpoints, `continue`, inspect, step.
6. When it faults, `bt` and `postmortem` to read the wreckage.

Exercise 1 walks you through this against a program with planted bugs (a null-pointer write, an off-by-one that corrupts a watched variable, an infinite loop you catch with a halt). Do it on the bench, not in your head — the muscle memory is the point.

A note on the `tui` mode: `gdb -tui` (or `Ctrl-X A` inside GDB) splits the terminal into a source view and a command view, so you see the current line highlighted as you step. It is genuinely useful for stepping through unfamiliar code, but it is also fragile over flaky terminals and can corrupt the display. Know it exists; use it when stepping logic, drop back to plain mode when reading registers and memory dumps where the layout matters.

## 7a. Reading a JTAG pinout (because you will meet one)

The RP2040 does not expose JTAG, but the moment you touch an FPGA dev board, an older STM32 with a 20-pin header, or a network appliance's debug port, you are looking at JTAG, and you must read its pinout cold. The standard **ARM 20-pin JTAG header** (0.1" 2×10) carries:

```text
 1  VTref      2  NC / VSupply
 3  nTRST      4  GND
 5  TDI        6  GND
 7  TMS        8  GND
 9  TCK       10  GND
11  RTCK      12  GND
13  TDO       14  GND
15  nRESET    16  GND
17  NC        18  GND
19  NC        20  GND
```

The five signals that matter: **TCK** (test clock, the probe drives), **TMS** (test mode select, steps the TAP state machine), **TDI** (data in to the chain), **TDO** (data out of the chain), and **nTRST** (optional async test reset). `VTref` is how the probe senses the target's I/O voltage so it level-shifts correctly — get this wrong (no VTref) and a good probe refuses to drive the lines, which is a "why won't it connect" you will eventually hit. The same 20-pin header also carries SWD on a *subset* of pins (the **SWD/JTAG combined** convention reuses pin 7 as `SWDIO` and pin 9 as `SWCLK`, with `SWO` on pin 13), which is why most ARM probes speak both and auto-detect. The newer 10-pin **Cortex Debug** connector (0.05" fine-pitch) is the modern compact version and is what the Raspberry Pi Debug Probe's SWD cable mates with.

Reading a pinout you have never seen: find `GND` (usually every other pin on one row — a dead giveaway), find `VTref`/`Vcc`, then `TCK`/`TMS`/`TDI`/`TDO` for JTAG or `SWCLK`/`SWDIO` for SWD. If the board has a 3-pin or fine-pitch header, it is almost certainly SWD-only. The midterm's "unfamiliar board" may well hand you a header to identify; the skill is to map silkscreen and pin-row patterns to the standard, not to memorize one board.

## 7b. The dual-core multidrop, in practice

The RP2040's two cores share the SWD bus via ADIv5 multidrop, and this surfaces in your session in small ways worth knowing. `target/rp2040.cfg` declares `rp2040.core0` and `rp2040.core1`; GDB attaches to core 0's server on 3333 by default. If you need core 1 (you launched code on it via the SDK's `multicore_launch_core1`), OpenOCD can expose a second GDB server, and you `target extended-remote localhost:3334` from a second GDB. A `monitor reset halt` halts *both* cores; a breakpoint set on core 0 does not halt core 1, so a bug in core-1 code is invisible from a core-0 GDB — a real gotcha when you split work across cores (Week 6's FreeRTOS-on-both-cores, or any `multicore_fifo` producer/consumer). When "the debugger shows core 0 idle but the system is clearly doing something," the something is on core 1; attach the second GDB. This is RP2040-specific and a frequent source of confused debugging for people coming from single-core parts.

## 8. When the probe can't connect

Three failure modes you will hit, in order of frequency:

- **"Error: Could not initialize the debug port" / "DAP init failed."** Usually wiring: `SWCLK` and `SWDIO` swapped, or no common ground. Double-check against the silkscreen. Also: if the target's firmware reconfigures the SWD pads as GPIO (the RP2040 lets you, datasheet §2.19.6.1), the probe loses the port — `monitor reset halt` *before* `load` to catch the target at reset before its firmware runs.
- **"target not halted."** The core is running and a command that needs a halt was issued. `monitor halt` first.
- **Breakpoint "cannot insert."** You ran out of hardware breakpoints (4 max) or tried a software breakpoint in flash. Use `hbreak`, and delete unused breakpoints (`delete`).

The recovery anchor, as always, is BOOTSEL: if the target's firmware is so broken it bricks the SWD port, hold BOOTSEL on power-up to land in the Boot ROM's USB-MSC mode (which leaves SWD alone) and reflash a known-good image. The Boot ROM never touches the debug port, so SWD always comes back after a BOOTSEL recovery.

A fourth failure mode worth naming because it wastes the most time: **a flaky USB cable or hub to the probe.** CMSIS-DAP runs over USB, and a marginal cable produces intermittent OpenOCD errors that look exactly like target bugs — random `DAP transfer error`, dropped GDB packets, "target not responding" that comes and goes. `set debug remote 1` showing checksum mismatches on the RSP packets, or OpenOCD logging USB retries, is the tell. The fix is mundane (a known-good short cable, a powered hub, or the probe direct to a root port), but the lesson is senior: when the *symptoms* are non-deterministic and span unrelated operations, suspect the *transport* before the target. A clean, repeatable failure is a target bug; a flaky, everything-is-weird failure is usually the link.

## 8a. The `load` vs `monitor flash write_image` distinction

Two ways to get your ELF onto the target, and they differ in ways that matter. `load` (a GDB command) transfers the ELF's loadable segments over the GDB connection and also tells GDB exactly where every symbol lives, so breakpoints-by-name and `addr2line`-grade symbol resolution work immediately afterward — but `load` writes through GDB's generic memory-write path, which on the RP2040 means OpenOCD's flash algorithm runs underneath, and for a large image it can be slow. `monitor flash write_image erase firmware.elf` runs OpenOCD's flash programmer directly (faster for big images) but does *not* update GDB's notion of the loaded symbols on its own. The practical recipe for the bench: `monitor reset halt`, then `load` (so symbols are right), then `monitor reset halt` again (so you start at the reset vector with the new image). If you only `monitor flash write_image`, remember to `file firmware.elf` in GDB afterward so your symbols match what is actually on the chip — a mismatch here produces breakpoints that land at the wrong address and a `bt` full of nonsense, a confusing failure that is really just stale symbols.

## 9. Summary

SWD is two wires that give a probe total introspection of the M0+: halt, register access, memory access, 4 hardware breakpoints, 2 watchpoints, and live SRAM reads. JTAG is the older five-wire daisy-chainable transport with boundary scan; the RP2040 exposes SWD, not JTAG. A second Pico flashed with `debugprobe.uf2` is a free CMSIS-DAP probe. OpenOCD bridges the probe to a GDB server on port 3333; `monitor` commands pass through to OpenOCD. GDB drives it all — `hbreak` for flash code (4 max), `watch` for the two hardware watchpoints, conditional breakpoints to filter, and `.gdbinit`/breakpoint-`commands`/the Python API to make the routine one keystroke. Next lecture: what the M0+ actually does when it faults, the eight-word stacked frame, and how to write a fault handler that captures a core dump you can read after the crash.

## References for this lecture

- RP2040 datasheet §2.3.4 "SWD", pp. 14–15; §2.4 "Cortex-M0+ Processor", pp. 16–18; §2.19.6.1 (GPIO override of SWD pads). <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- ARM Debug Interface Architecture Specification ADIv5.2, IHI0031. <https://developer.arm.com/documentation/ihi0031/latest/>
- Cortex-M0+ Technical Reference Manual, DDI0484 (breakpoint/watchpoint comparator counts). <https://developer.arm.com/documentation/ddi0484/latest/>
- OpenOCD user guide, "GDB and OpenOCD" and "General Commands" chapters. <https://openocd.org/doc/html/index.html>
- Raspberry Pi "Getting started with Raspberry Pi Pico", Appendix A (Picoprobe) and Appendix B (OpenOCD). <https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf>
- Raspberry Pi debugprobe firmware. <https://github.com/raspberrypi/debugprobe>
- GDB user manual, "Remote Debugging", "Breakpoints", "Watchpoints", "Extending GDB". <https://sourceware.org/gdb/current/onlinedocs/gdb/>
