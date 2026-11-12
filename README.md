# powerd

A lightweight, zero-dependency Linux power management daemon written in pure C and compiled statically against `musl-libc`. Designed to run across both modern `systemd` distributions and minimal Puppy Linux / BusyBox systems without Python, D-Bus, or runtime library dependencies.

## Features

- **Cross-Vendor Hardware Enforcement:** Automatically detects CPU vendor (`/proc/cpuinfo`) and applies vendor-specific power ceilings:
  - **Intel (`intel_pstate` + `intel-rapl`):** Parks P-core SMT siblings (`is_smt_sibling`), disables Turbo Boost (`no_turbo = 1`), and enforces a `6 W` (`6000000 uW`) RAPL `PL1` package limit in `low-power` mode.
  - **AMD (`acpi-cpufreq` / `amd-pstate`):** Clamps `scaling_max_freq` to the `1.60 GHz` base P-state (`1600000 kHz`), switches governors to `powersave`, and optionally invokes `ryzenadj` (`8 W` STAPM) if present—with zero hard external dependencies.
- **Static `musl` Binary:** Single portable x86_64 ELF executable (`~25 KB`) with zero shared library requirements.
- **Universal Init Support:** Ships with automated installation for both `systemd` units and Puppy Linux (`/root/Startup`) environments.

## Verified Hardware Benchmarks

Measured under sustained multi-thread CPU load (`yes` stress workers) comparing stock Linux defaults against `powerd` in `low-power` mode:

| Platform & CPU | OS & Scaling Driver | Telemetry Source | Stock Load | `powerd` (`low-power`) | Power Reduction |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **HP ProBook 440 G9**<br>Intel Core i5-1235U (2P+8E / 12T) | BookwormPup64 (`intel_pstate`) | CPU Package (`intel-rapl:0`) | 11.02 W (`11,016 mW`) | **5.12 W (`5,118 mW`)** | **-53.5% (-5.90 W)** |
| **ASUS TUF Gaming A15 (FA507UV)**<br>AMD Ryzen 7 8845HS (8C / 16T) | CachyOS (`acpi-cpufreq`) | Total System Battery (`BAT1`) | 20.57 W (`20,565 mW`) | **12.76 W (`12,755 mW`)** | **-38.0% (-7.81 W)** |

<details>
<summary>Raw Hardware Verification Logs</summary>

### Intel Core i5-1235U (HP ProBook 440 G9 — BookwormPup64)
```text
[powerctl] Mode switched to: low-power
Online Cores : 0,2,4-11
No Turbo     : 1
RAPL PL1 uW  : 6000000
BEFORE powerd (Load) : 11016 mW
AFTER  powerd (Load) : 5118 mW
```

### AMD Ryzen 7 8845HS (ASUS TUF FA507UV — CachyOS)
```text
[powerctl] Mode switched to: low-power
Max Freq (kHz) : 1600000
Active Gov     : powersave
Ryzen 7 8845HS BEFORE (Stock Load) : 20565 mW
Ryzen 7 8845HS AFTER  (1.60 GHz)   : 12755 mW
```
</details>

## Quick Install

```sh
curl -sSL https://raw.githubusercontent.com/kushagra112007/powerd/main/scripts/install.sh | sh
```

## Usage

```sh
powerctl low          # Enforce low-power profile (6W Intel PL1 / 1.6GHz AMD clamp)
powerctl balanced     # Restore balanced scaling (15W Intel PL1 / schedutil)
powerctl performance  # Full turbo & performance governor
powerctl status       # Display active mode, online cores, and package wattage
```

## Building from Source

```sh
musl-gcc -O2 -march=x86-64 -static src/powerd.c -o bin/powerd-x86_64
```
