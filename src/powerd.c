#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>

#define MODE_FILE "/tmp/powerd.mode"

typedef enum { VENDOR_INTEL, VENDOR_AMD, VENDOR_UNKNOWN } CpuVendor;
typedef enum { MODE_LOW_POWER, MODE_BALANCED, MODE_PERFORMANCE } PowerMode;

static CpuVendor cpu_vendor = VENDOR_UNKNOWN;
static char telemetry_path[128] = {0};

void detect_hardware(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "vendor_id", 9) == 0) {
                if (strstr(line, "AuthenticAMD")) cpu_vendor = VENDOR_AMD;
                else if (strstr(line, "GenuineIntel")) cpu_vendor = VENDOR_INTEL;
                break;
            }
        }
        fclose(f);
    }

    glob_t g;
    if (glob("/sys/class/hwmon/hwmon*/power1_input", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            int fd = open(g.gl_pathv[i], O_RDONLY);
            if (fd >= 0) {
                char buf[32] = {0};
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0 && strtoull(buf, NULL, 10) > 0) {
                    strncpy(telemetry_path, g.gl_pathv[i], sizeof(telemetry_path) - 1);
                    break;
                }
            }
        }
        globfree(&g);
    }

    if (telemetry_path[0] == '\0') {
        if (access("/sys/class/powercap/intel-rapl:0/energy_uj", R_OK) == 0) {
            strncpy(telemetry_path, "/sys/class/powercap/intel-rapl:0/energy_uj", sizeof(telemetry_path) - 1);
        }
    }

    printf("[powerd] CPU Vendor    : %s\n", (cpu_vendor == VENDOR_AMD) ? "AMD Ryzen" : "Intel");
    printf("[powerd] Telemetry Path: %s\n", telemetry_path[0] ? telemetry_path : "None");
}

int set_core(int core_id, int state) {
    if (core_id == 0) return 0; // Boot core must stay active

    char path[64];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", core_id);

    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;

    char cur;
    if (read(fd, &cur, 1) > 0 && (cur - '0') == state) {
        close(fd);
        return 0;
    }

    char val = state ? '1' : '0';
    ssize_t n = pwrite(fd, &val, 1, 0);

    if (n < 0) {
        usleep(2000);
        n = pwrite(fd, &val, 1, 0);
    }

    close(fd);
    return (n > 0) ? 0 : -1;
}

void apply_mode(PowerMode mode) {
    long total_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (total_cpus <= 0) total_cpus = 16;

    if (mode == MODE_PERFORMANCE) {
        for (int i = 1; i < total_cpus; i++) {
            set_core(i, 1);
        }
    } else if (mode == MODE_LOW_POWER) {
        if (cpu_vendor == VENDOR_AMD) {
            for (int i = 1; i < total_cpus; i += 2) {
                set_core(i, 0);
                usleep(1000);
            }
            for (int i = 2; i < total_cpus; i += 2) {
                set_core(i, 1);
            }
        } else {
            for (int i = 4; i < total_cpus; i++) {
                set_core(i, 0);
                usleep(1000);
            }
        }
    }
}

PowerMode get_current_mode(void) {
    int fd = open(MODE_FILE, O_RDONLY);
    if (fd < 0) return MODE_BALANCED;

    char buf[16] = {0};
    read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (strncmp(buf, "low-power", 9) == 0) return MODE_LOW_POWER;
    if (strncmp(buf, "performance", 11) == 0) return MODE_PERFORMANCE;
    return MODE_BALANCED;
}

int main(void) {
    detect_hardware();

    while (1) {
        PowerMode mode = get_current_mode();
        apply_mode(mode);
        sleep(1);
    }
    return 0;
}
