# powerd

A lightweight, zero-dependency Linux power management daemon written in pure POSIX C. Designed to enforce hard silicon power ceilings across both modern `systemd` distributions and minimal Puppy Linux / BusyBox systems without Python, D-Bus, or runtime library bloat (`<1 MB` RSS).

## Features (v1.3.0)

- **Automatic U-Series vs. H-Series TDP Classification:** Parses `/proc/cpuinfo` SKUs (`U`/`P`/`Y` vs. `H`/`HS`/`HX`/`HK`) and RAPL capability registers to apply architecture-specific power curves rather than one-size-fits-all wattage caps.
- **Dual Intel RAPL + MMIO Enforcement (`intel-rapl:0` & `intel-rapl-mmio:0`):**
  - Writes PL1, PL2, and a tightened **`1s` `tau` (`constraint_0_time_window_us`)** across both standard MSR RAPL and MMIO RAPL interfaces so OEM DPTF firmware cannot override package ceilings on H-series laptops.
  - Keeps Turbo enabled (`no_turbo = 0`) under RAPL regulation (`9W` PL1 on U-series, `10W` PL1 on H-series in `low-power`; `15W` PL1 in `balanced`) to avoid the static-uncore base-clock efficiency cliff.
  - Parks P-core SMT siblings (`is_smt_sibling`) on H-series processors in `low-power` mode while keeping all physical P- and E-cores online.
- **AMD Ryzen Architecture-Aware Control (`Zen 3` / `Zen 4+`):**
  - Detects Zen generation (`family`/`model`) to clamp `scaling_max_freq` (`2.80 GHz` on Zen 4+, `2.40 GHz` on Zen 3), park SMT siblings / excess cores in `low-power`, and enforce SMU limits via `ryzenadj` (`15.5W` STAPM sweet spot on H/HS chips in `balanced`, `6W` in `low-power`) when available—with zero hard external dependencies.
- **Autonomous AC / Battery Switching & Sleep Recovery:**
  - Automatically switches between `balanced` (AC) and `low-power` (Battery) via `/sys/class/power_supply/*/online` when set to `auto` (default), while honoring explicit manual overrides in `/run/powerd/mode`.
  - Detects suspend-to-RAM wakeups and periodically refreshes registers every `15s` so BIOS/EC resets never wipe active watt caps.
- **Hardened Daemon Lifecycle:** Single-instance `flock()` lock on `/run/powerd/powerd.pid`, live package telemetry in `/run/powerd/watts`, and `fork()` + `execve()` module probing (`intel_rapl_msr`) with zero `system()` shell invocations.

---

## Verified Hardware Benchmarks

Measured under sustained multi-threaded OpenSSL `rsa2048` workloads comparing Stock defaults against `powerd` (`v1.3.0`):

### 1. Power & Performance-Per-Watt Scaling (v1.3.0)

| Platform & Silicon | Profile | Sustained Power | RSA-2048 Sign Speed | Efficiency (Sign/s per Watt) | Power Delta vs. Stock |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Intel Core i5-13420H**<br>*(8C / 12T: 4P+4E, H-Series)*<br>BookwormPup64 (`intel_pstate`) | Stock (12T) | 25.90 W | 4,861.7 sign/s | 187.7 sign/s/W | Baseline |
| | **`powerd` Balanced (`15W` PL1, 1s tau)** | **15.01 W** | **8,171.2 sign/s** | **544.4 sign/s/W (2.9x)** | **-42.0% (-10.89 W)** |
| | **`powerd` Low-Power (`10W` PL1, 8C)** | **10.00 W** | **1,638.0 sign/s** | **163.8 sign/s/W** | **-61.4% (-15.90 W)** |
| **HP ProBook 440 G9**<br>*Intel Core i5-1235U (2P+8E / 12T)*<br>BookwormPup64 (`intel_pstate`) | Stock (12T) | 11.02 W | — | — | Baseline |
| | **`powerd` Low-Power** | **5.12 W** | — | — | **-53.5% (-5.90 W)** |
| **HP Pro Tower 280 G9**<br>*Intel Core i7-13700H (14C / 20T)*<br>Puppy Linux 6.1 (`intel_pstate`) | Performance (`35W` PL1) | 35.20 W | — | — | Baseline |
| | **`powerd` Low-Power (14C)** | **11.06 W** | — | — | **-68.6% (-24.14 W)** |
| **ASUS TUF Gaming A15 (FA507UV)**<br>*AMD Ryzen 7 8845HS (8C / 16T, H-Series, Zen 4+)*<br>CachyOS (`amd-pstate-epp` / `ryzenadj`) | Stock (Unclamped) | 28.67 W | 31,296.0 sign/s | 1,091.6 sign/s/W | Baseline |
| | **`powerd` Performance (`54W` Burst)** | **27.91 W** | **31,037.7 sign/s** | **1,112.1 sign/s/W** | **-2.7% (-0.76 W)** |
| | **`powerd` Balanced (`15.5W` STAPM + EPP)** | **15.35 W** | **22,338.3 sign/s** | **1,455.3 sign/s/W** | **-46.5% (-13.32 W)** |
| | **`powerd` Low-Power (`7.5W` STAPM, 6C)** | **7.45 W** | **7,118.7 sign/s** | **955.5 sign/s/W** | **-74.0% (-21.22 W)** |
| | **`powerd` Ultra-Low-Power (`5.5W` STAPM, 4C)** | **5.35 W** | **5,990.7 sign/s** | **1,119.8 sign/s/W** | **-81.3% (-23.32 W)** |

### 2. Why Dual RAPL (`MMIO`) + `1s` `tau` Matters on Intel H-Series

On 12th/13th Gen Intel H-series laptops, writing only `/sys/class/powercap/intel-rapl:0` leaves the processor pinned to its `25W` PL2 ceiling under sustained load due to OEM MMIO RAPL limits and long default `tau` windows. Mirroring limits to `intel-rapl-mmio:0` with `constraint_0_time_window_us = 1000000` clamps sustained wattage immediately:

```text
[powerd] CPU Vendor    : Intel (8c/12t, H-Series)
[powerd] Telemetry Path: /sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj

v1.1 (sysfs RAPL only, default tau) -> 25.04 W | 25.01 W | 25.02 W | 24.97 W | 25.00 W
v1.2.0 (MSR + MMIO RAPL, 1s tau)    -> 15.06 W | 15.02 W | 15.00 W | 15.00 W | 15.00 W
```

<details>
<summary>Additional Raw Hardware Verification Logs</summary>

### Intel Core i5-1235U (HP ProBook 440 G9 — BookwormPup64)
```text
[powerctl] Mode switched to: low-power
Online Cores : 0,2,4-11
RAPL PL1 uW  : 6000000
BEFORE powerd (Load) : 11016 mW
AFTER  powerd (Load) : 5118 mW
```

### Intel Core i7-13700H (HP Pro Tower 280 G9 — Puppy Linux 6.1.148)
```text
[low-power]   Cores=0,2,4,6,8,10,12,14,16-19 | PL1=6000000 uW  | 15s Load Avg=11.06 W
[performance] Cores=0-19                     | PL1=35000000 uW | EPP=performance | 15s Load Avg=35.20 W
```

### AMD Ryzen 7 8845HS (ASUS TUF FA507UV — CachyOS)
```text
[powerctl] Mode switched to: balanced
Active Mode    : balanced (15.5W STAPM, amd-pstate-epp)
Online Cores   : 16 / 16 threads
Ryzen 7 8845HS Stock Load   : 28,670 mW (31,296.0 sign/s)
Ryzen 7 8845HS Balanced     : 15,350 mW (22,338.3 sign/s | +33.3% perf/W)
Ryzen 7 8845HS Low-Power    :  7,450 mW ( 7,118.7 sign/s | 6C)
Ryzen 7 8845HS Ultra-Low    :  5,350 mW ( 5,990.7 sign/s | 4C, boost OFF)
```
</details>

---

## Quick Install

```sh
curl -4 -fL https://raw.githubusercontent.com/kushagra112007/powerd/main/scripts/install.sh | sh
```

## Usage

```sh
powerctl auto         # Automatic AC (balanced) / Battery (low-power) switching
powerctl ultra-low    # Force ultra-low-power profile (6W Intel PL1; 5.5W Zen 4+ STAPM, 4C)
powerctl low          # Force low-power profile (9W U / 10W H Intel PL1; 7.5W Zen 4+ STAPM, 6C)
powerctl balanced     # Force balanced profile (15W Intel PL1; 15.5W Ryzen H STAPM + EPP)
powerctl performance  # Force performance profile (full turbo & 54W/80W power limits)
powerctl status       # Display active mode, online cores, and live package wattage
```

*(Aliases supported: `ultra-low-power`, `ultralow`, `ulp`, `low-power`, `powersave`, `eco`, `bal`, `perf`.)*

You can also control the daemon directly without `powerctl` via `/run/powerd/`:

```sh
echo balanced > /run/powerd/mode   # Switch active mode
cat /run/powerd/watts               # Read live package power draw in Watts
```

## Building from Source

```sh
# Native GLIBC build
gcc -O2 -Wall -Wextra src/powerd.c -o /usr/local/bin/powerd

# Portable static musl build
musl-gcc -O2 -march=x86-64 -static src/powerd.c -o bin/powerd-x86_64
```

## License

Licensed under the [MIT License](LICENSE).
