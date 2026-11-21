# HP 8903 Audio Analyzer — Mac & Windows

A self-contained application for controlling the **HP 8903A / 8903B Audio
Analyzer** over GPIB on macOS (Intel and Apple silicon, 10.13+) and, later,
Windows 7–11. No NI-488.2, no VISA, no HP VEE runtime.

**Status: stage 1 — driver, instrument layer and command-line tester.**
The graphical app comes next, on top of this core.

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

## First test on real hardware

1. Download `hp8903-cli` from the latest **build** run under GitHub Actions
   (artifact `hp8903-cli-macos-universal`), or build it yourself (below).
2. Clear the download quarantine and make it executable:
   ```
   xattr -d com.apple.quarantine hp8903-cli; chmod +x hp8903-cli
   ```
3. With the adapter plugged in:
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
libusb is downloaded and built automatically as a static library.

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
tools/           hp8903-cli
tests/           packet-framing, parser and sweep tests (no hardware needed)
```

## Licence

GPL-2.0. The NI GPIB-USB protocol is ported from the Linux kernel driver
`drivers/gpib/ni_usb/ni_usb_gpib.c`, © 2004 Frank Mori Hess, GPL-2.0.
The 8903 measurement sequences follow Pete Millett's VEE programs, as
documented by pvdm's Python port. No code from either was copied.
