# powerd — Low-Level Intel & AMD Power Management Daemon

\`powerd\` is a zero-dependency C daemon designed for Linux systems where hardware power limits (\`intel-rapl\` sysfs) and frequency governors are hard-locked by OEM BIOS firmware (HP, Lenovo, etc.).

## Technical Mechanics

Modern OEM laptops and All-in-Ones lock MSR \`0x610\` (\`MSR_PKG_POWER_LIMIT\`) during POST and enforce Hardware-Autonomous P-States (\`intel_pstate\` HWP mode). Software writes to standard frequency/power limit nodes fail with \`Input/output error\` (\`EIO\`).

\`powerd\` bypasses locked register paths by combining two unmasked kernel features:
1. **Telemetry:** Samples unmasked microjoule package power via \`/sys/class/powercap/*/energy_uj\`.
2. **Power Clamping:** Offloads secondary execution pipelines (E-cores / SMT threads) via \`/sys/devices/system/cpu/cpu*/online\`, forcing package power down under load.

## Quick Start

\`\`\`bash
sudo ./scripts/install.sh
powerctl status
\`\`\`

## Profiles

- **low-power**: Clamps execution to primary cores (\`cpu0-3\`), holding package power to ~4W under 100% load.
- **balanced**: Dynamically offloads cores based on real-time Wattage thresholds.
- **performance**: Restores all physical cores and hyperthreads.
