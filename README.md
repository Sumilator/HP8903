# HP 8903 Audio Analyzer — Mac & Windows

A self-contained application for controlling the **HP 8903A / 8903B Audio
Analyzer** over GPIB on macOS (Intel and Apple silicon, 10.13+) and, later,
Windows 7–11. No NI-488.2, no VISA, no HP VEE runtime.

**Status: stage 2 — the app.** It has frequency response, THD+N, voltage and
power sweeps (four traces per tab), a live monitor, and a raw-command console.
It runs against a built-in simulator; it has not yet been tried on real hardware.

## Why another one?

* [Pete Millett's software](http://www.pmillett.com/hp_8903_software.htm) is
  the original, but it runs on HP VEE and the vendor's GPIB stack, so it is
  hard to install on modern Windows.
* [pvdm's hp8903a](https://framagit.org/pvdm/hp8903a) is a Python/Qt port for
  Linux. On macOS it supports only Prologix adapters, because NI-488.2 for Mac
  needs a kernel extension.
* This project drives the **NI GPIB-USB-HS** directly from user space, using
  a port of the Linux kernel's `ni_usb_gpib` protocol. There is nothing to
  install: plug in the adapter and run.

## Supported adapters

| Adapter | USB ID | Status |
|---|---|---|
| NI GPIB-USB-HS | 3923:709b | supported (own driver) |
| NI GPIB-USB-HS+ | 3923:7618 | supported (own driver) |
| NI GPIB-USB-B | 3923:702a/702b | not yet: needs NI firmware at power-up |
| Prologix / AR488 | serial | planned |
| Simulator | — | `--adapter mock` |

On a Mac, **NI-488.2 must not be installed**. Its kernel extension would claim
the adapter before this program can.

## Getting the Mac app

1. On GitHub, open the repo's **Actions** tab, then the latest green **build**
   run. Download the artifact `HP8903-macos-universal`. It holds
   `HP8903-mac.zip` (the app) and `hp8903-cli`.
2. Unzip, move **HP8903.app** to Applications, then clear the download
   quarantine once (the app isn't notarised by Apple):
   ```
   xattr -dr com.apple.quarantine /Applications/HP8903.app
   ```
3. Choose **Simulated HP 8903** to try it without hardware, or your NI adapter.

Exports (CSV) and screenshots go to `~/Documents/HP8903/`. Settings are kept in
`~/Library/Application Support/HP8903/`.

### Filters

On the 8903A, H1 is the built-in 400 Hz high-pass and H2 is the optional
weighting filter (A-weighting here). On the 8903B, H1 and H2 are the
left and right plug-in slots. The names are editable under **Instrument...**.

## First test on real hardware (command line)

`hp8903-cli` isolates the driver from the UI. Clear its quarantine and make
it executable (`xattr -d com.apple.quarantine hp8903-cli; chmod +x hp8903-cli`), then:
   ```
   ./hp8903-cli list        # adapter detected?
   ./hp8903-cli probe       # driver talks to the adapter, becomes CIC
   ./hp8903-cli read        # 8903 on the bus at address 28: one reading
   ./hp8903-cli measure --freq 1000 --level 1 --mode thd
   ./hp8903-cli sweep fr --points 20 --csv fr.csv
   ```
   Loop the 8903's oscillator output to its input for a quick self-check.
   `--addr N` changes the GPIB address. Front panel `21.0 SPCL` shows it.

## Building

Requires CMake 3.20+ and a C++17 compiler (Xcode command-line tools on Mac).
libusb, GLFW, Dear ImGui, ImPlot and stb are downloaded and built
automatically, so the result is a single self-contained app.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On macOS this produces a universal (arm64 + x86_64) binary for 10.13+.

## Layout

```
core/gpib/       Transport interface, NI GPIB-USB driver, simulator
core/hp8903/     8903 program codes, reading parser, sweep engine
app/             Dear ImGui + ImPlot application (GLFW / OpenGL)
tools/           hp8903-cli
tests/           packet-framing, parser and sweep tests (no hardware needed)
```

## Front-panel display

The **Monitor** tab mirrors the 8903's front panel so it can be read from
across the bench. Readings are red seven-segment LEDs, frequency on the left and
the measurement on the right, and the other figures are in green. It shows a main reading plus an optional second one that
alternates with it (for example THD+N and AC level), and figures derived from
them:

* AC level: dBV, dBu, power into a set load, gain re the source level.
* THD+N: dB, the SINAD equivalent, ppm.
* SINAD: the THD+N % equivalent.
* Min, max and average.

**Big display**, or double-clicking the display, fills the window. **F**
toggles full screen and **Esc** goes back. *Read frequency counter* reads the
8903's left display using RL/RR. These codes are documented for the 8903B, and
if an 8903A doesn't answer, the display shows the source frequency instead.

## Licence

GPL-2.0. The NI GPIB-USB protocol is ported from the Linux kernel driver
`drivers/gpib/ni_usb/ni_usb_gpib.c`, © 2004 Frank Mori Hess, GPL-2.0.
The B612 Mono font (app/fonts) is by The B612 Project Authors, SIL Open Font
Licence 1.1 (see app/fonts/B612-OFL.txt).
The 8903 measurement sequences follow Pete Millett's VEE programs, as
documented by pvdm's Python port. No code from either was copied.
