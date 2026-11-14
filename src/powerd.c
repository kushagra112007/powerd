#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define STATE_DIR  "/run/powerd"
#define PID_FILE   "/run/powerd/powerd.pid"
#define MODE_FILE  "/run/powerd/mode"
#define WATTS_FILE "/run/powerd/watts"

typedef enum { VENDOR_INTEL, VENDOR_AMD, VENDOR_UNKNOWN } CpuVendor;
typedef enum { AMD_ZEN_LEGACY, AMD_ZEN3, AMD_ZEN4_PLUS } AmdArch;
typedef enum { TELEM_NONE, TELEM_HWMON_UW, TELEM_RAPL_UJ, TELEM_BAT_UA_UV } TelemType;
typedef enum { MODE_LOW_POWER, MODE_BALANCED, MODE_PERFORMANCE } PowerMode;

static CpuVendor cpu_vendor = VENDOR_UNKNOWN;
static AmdArch amd_arch = AMD_ZEN_LEGACY;
static int total_logical_cpus = 1;
static int total_physical_cores = 1;
static char ryzenadj_path[64] = {0};

static TelemType telem_type = TELEM_NONE;
static char telemetry_path[128] = {0};
static unsigned long long prev_energy_uj = 0;

static int spawn_safe(const char *bin, char *const argv[]) {
    if (!bin || access(bin, X_OK) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        char *const envp[] = { "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", NULL };
        execve(bin, argv, envp);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int acquire_singleton_lock(void) {
    mkdir(STATE_DIR, 0755);
    chmod(STATE_DIR, 0755);

    int fd = open(PID_FILE, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }

    if (ftruncate(fd, 0) == 0) {
        char pid_buf[32];
        int len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
        if (len > 0) (void)write(fd, pid_buf, (size_t)len);
    }
    return fd;
}

int write_sysfs_safe(const char *path, const char *val) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;

    char cur[32] = {0};
    ssize_t r = read(fd, cur, sizeof(cur) - 1);
    if (r > 0) {
        cur[strcspn(cur, "\n")] = '\0';
        if (strcmp(cur, val) == 0) {
            close(fd);
            return 0;
        }
    }

    ssize_t bytes = pwrite(fd, val, strlen(val), 0);

    if (bytes < 0 && errno == EIO) {
        lseek(fd, 0, SEEK_SET);
        memset(cur, 0, sizeof(cur));
        if (read(fd, cur, sizeof(cur) - 1) > 0) {
            cur[strcspn(cur, "\n")] = '\0';
            if (strcmp(cur, val) == 0) {
                close(fd);
                return 0;
            }
        }
    }

    close(fd);
    return (bytes > 0) ? 0 : -1;
}

int is_smt_sibling(int cpu_id) {
    char path[128], buf[64] = {0};
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    int first_cpu = atoi(buf);
    return (first_cpu < cpu_id);
}

int is_intel_ecore(int cpu_id) {
    if (cpu_vendor != VENDOR_INTEL) return 0;

    char path[128], buf[64] = {0};
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/core_type", cpu_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    return (strstr(buf, "Atom") != NULL || strstr(buf, "Efficient") != NULL);
}

unsigned long long read_ull(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    return strtoull(buf, NULL, 10);
}

static void ensure_intel_rapl_loaded(void) {
    if (cpu_vendor != VENDOR_INTEL) return;
    if (access("/sys/class/powercap/intel-rapl:0/energy_uj", R_OK) == 0 ||
        access("/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj", R_OK) == 0) {
        return;
    }

    char *const argv[] = { "modprobe", "intel_rapl_msr", NULL };
    if (spawn_safe("/sbin/modprobe", argv) < 0) {
        (void)spawn_safe("/usr/bin/modprobe", argv);
    }
    usleep(100000);
}

static int count_physical_cores(int logical_cpus) {
    for (int i = 1; i < logical_cpus; i++) {
        char p[96];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/online", i);
        write_sysfs_safe(p, "1");
    }

    int seen[256] = {0};
    int unique = 0;
    for (int i = 0; i < logical_cpus && i < 256; i++) {
        char p[128], buf[32] = {0};
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            if (read(fd, buf, sizeof(buf) - 1) > 0) {
                int cid = atoi(buf);
                if (cid >= 0 && cid < 256 && !seen[cid]) {
                    seen[cid] = 1;
                    unique++;
                }
            }
            close(fd);
        }
    }
    return (unique > 0) ? unique : logical_cpus;
}

void detect_hardware(void) {
    total_logical_cpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (total_logical_cpus <= 0) total_logical_cpus = 16;

    int family = 0, model = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "vendor_id", 9) == 0) {
                if (strstr(line, "AuthenticAMD")) cpu_vendor = VENDOR_AMD;
                else if (strstr(line, "GenuineIntel")) cpu_vendor = VENDOR_INTEL;
            } else if (strncmp(line, "cpu family", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) family = atoi(c + 1);
            } else if (strncmp(line, "model\t", 6) == 0 || strncmp(line, "model :", 7) == 0) {
                char *c = strchr(line, ':');
                if (c) model = atoi(c + 1);
            }
            if (cpu_vendor != VENDOR_UNKNOWN && family > 0 && model > 0) break;
        }
        fclose(f);
    }

    total_physical_cores = count_physical_cores(total_logical_cpus);

    if (cpu_vendor == VENDOR_AMD) {
        if (family == 26 || (family == 25 && model >= 0x60)) {
            amd_arch = AMD_ZEN4_PLUS;
        } else if (family == 25 && model >= 0x20) {
            amd_arch = AMD_ZEN3;
        } else {
            amd_arch = AMD_ZEN_LEGACY;
        }

        if (access("/usr/bin/ryzenadj", X_OK) == 0)
            strncpy(ryzenadj_path, "/usr/bin/ryzenadj", sizeof(ryzenadj_path) - 1);
        else if (access("/usr/local/bin/ryzenadj", X_OK) == 0)
            strncpy(ryzenadj_path, "/usr/local/bin/ryzenadj", sizeof(ryzenadj_path) - 1);
    }

    ensure_intel_rapl_loaded();

    glob_t g;
    if (glob("/sys/class/hwmon/hwmon*/power1_input", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            if (read_ull(g.gl_pathv[i]) > 0) {
                strncpy(telemetry_path, g.gl_pathv[i], sizeof(telemetry_path) - 1);
                telem_type = TELEM_HWMON_UW;
                break;
            }
        }
        globfree(&g);
    }

    if (telem_type == TELEM_NONE) {
        const char *rapl_candidates[] = {
            "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
            "/sys/class/powercap/intel-rapl:0/energy_uj"
        };
        for (int i = 0; i < 2; i++) {
            if (access(rapl_candidates[i], R_OK) == 0) {
                strncpy(telemetry_path, rapl_candidates[i], sizeof(telemetry_path) - 1);
                telem_type = TELEM_RAPL_UJ;
                prev_energy_uj = read_ull(telemetry_path);
                break;
            }
        }
    }

    if (telem_type == TELEM_NONE) {
        if (glob("/sys/class/power_supply/BAT*/current_now", 0, NULL, &g) == 0) {
            if (g.gl_pathc > 0) {
                strncpy(telemetry_path, g.gl_pathv[0], sizeof(telemetry_path) - 1);
                char *slash = strrchr(telemetry_path, '/');
                if (slash) *slash = '\0';
                telem_type = TELEM_BAT_UA_UV;
            }
            globfree(&g);
        }
    }

    printf("[powerd] CPU Vendor    : %s (%dc/%dt%s)\n",
           (cpu_vendor == VENDOR_AMD) ? "AMD Ryzen" :
           (cpu_vendor == VENDOR_INTEL) ? "Intel" : "Unknown",
           total_physical_cores, total_logical_cpus,
           (cpu_vendor == VENDOR_AMD) ?
               (amd_arch == AMD_ZEN4_PLUS ? ", Zen 4+" :
                amd_arch == AMD_ZEN3 ? ", Zen 3" : ", Zen Legacy") : "");
    printf("[powerd] Telemetry Path: %s\n", telemetry_path[0] ? telemetry_path : "None");
    fflush(stdout);
}

void update_telemetry(void) {
    double watts = -1.0;

    if (telem_type == TELEM_HWMON_UW) {
        unsigned long long uw = read_ull(telemetry_path);
        if (uw > 0) watts = (double)uw / 1000000.0;
    } else if (telem_type == TELEM_RAPL_UJ) {
        unsigned long long now_uj = read_ull(telemetry_path);
        if (prev_energy_uj > 0 && now_uj >= prev_energy_uj) {
            watts = (double)(now_uj - prev_energy_uj) / 1000000.0;
        }
        prev_energy_uj = now_uj;
    } else if (telem_type == TELEM_BAT_UA_UV) {
        char cpath[160], vpath[160];
        snprintf(cpath, sizeof(cpath), "%s/current_now", telemetry_path);
        snprintf(vpath, sizeof(vpath), "%s/voltage_now", telemetry_path);
        unsigned long long ua = read_ull(cpath);
        unsigned long long uv = read_ull(vpath);
        if (ua > 0 && uv > 0) {
            watts = ((double)ua * (double)uv) / 1000000000000.0;
        }
    }

    if (watts >= 0.0) {
        FILE *wf = fopen(WATTS_FILE, "w");
        if (wf) {
            fprintf(wf, "%.2f\n", watts);
            fclose(wf);
            chmod(WATTS_FILE, 0644);
        }
    }
}

void set_core(int core_id, int state) {
    if (core_id == 0) return;
    char path[64];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", core_id);
    write_sysfs_safe(path, state ? "1" : "0");
}

void set_governor(int core_id, const char *gov) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", core_id);
    write_sysfs_safe(path, gov);
}

void set_epp(int core_id, const char *epp) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", core_id);
    write_sysfs_safe(path, epp);
}

void apply_intel_limits(PowerMode mode) {
    if (cpu_vendor != VENDOR_INTEL) return;

    const char *no_turbo = (mode == MODE_LOW_POWER) ? "1" : "0";
    const char *pl1_uw   = (mode == MODE_LOW_POWER) ? "6000000" :
                           (mode == MODE_BALANCED)  ? "15000000" : "35000000";
    const char *pl2_uw   = (mode == MODE_LOW_POWER) ? "8000000" :
                           (mode == MODE_BALANCED)  ? "25000000" : "55000000";

    write_sysfs_safe("/sys/devices/system/cpu/intel_pstate/no_turbo", no_turbo);

    if (write_sysfs_safe("/sys/class/powercap/intel-rapl/intel-rapl:0/constraint_0_power_limit_uw", pl1_uw) < 0) {
        write_sysfs_safe("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", pl1_uw);
    }
    if (write_sysfs_safe("/sys/class/powercap/intel-rapl/intel-rapl:0/constraint_1_power_limit_uw", pl2_uw) < 0) {
        write_sysfs_safe("/sys/class/powercap/intel-rapl:0/constraint_1_power_limit_uw", pl2_uw);
    }
}

void apply_amd_limits(PowerMode mode) {
    if (cpu_vendor != VENDOR_AMD) return;

    const char *boost = (mode == MODE_LOW_POWER) ? "0" : "1";
    write_sysfs_safe("/sys/devices/system/cpu/cpufreq/boost", boost);

    const char *max_khz = "5200000";
    if (mode == MODE_LOW_POWER) {
        if (amd_arch == AMD_ZEN4_PLUS) max_khz = "2800000";
        else if (amd_arch == AMD_ZEN3) max_khz = "2400000";
        else max_khz = "2000000";
    }

    for (int i = 0; i < total_logical_cpus; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
        write_sysfs_safe(path, max_khz);
    }

    if (ryzenadj_path[0] != '\0') {
        if (mode == MODE_LOW_POWER) {
            if (amd_arch == AMD_ZEN4_PLUS) {
                char *const argv[] = { "ryzenadj", "--stapm-limit=5000", "--slow-limit=5000", "--fast-limit=25000", "--stapm-time=3", NULL };
                spawn_safe(ryzenadj_path, argv);
            } else if (amd_arch == AMD_ZEN3) {
                char *const argv[] = { "ryzenadj", "--stapm-limit=8500", "--slow-limit=8500", "--fast-limit=25000", "--stapm-time=4", NULL };
                spawn_safe(ryzenadj_path, argv);
            } else {
                char *const argv[] = { "ryzenadj", "--stapm-limit=10000", "--slow-limit=10000", "--fast-limit=25000", "--stapm-time=5", NULL };
                spawn_safe(ryzenadj_path, argv);
            }
        } else if (mode == MODE_BALANCED) {
            char *const argv[] = { "ryzenadj", "--stapm-limit=8000", "--slow-limit=8000", "--fast-limit=35000", "--stapm-time=6", NULL };
            spawn_safe(ryzenadj_path, argv);
        } else {
            char *const argv[] = { "ryzenadj", "--stapm-limit=16000", "--slow-limit=16000", "--fast-limit=50000", "--stapm-time=20", NULL };
            spawn_safe(ryzenadj_path, argv);
        }
    }
}

static void apply_amd_cores(PowerMode mode, const char *gov, const char *epp) {
    for (int i = 1; i < total_logical_cpus; i++) {
        set_core(i, 1);
    }

    int target_phys_cores = total_physical_cores;
    int keep_smt = 1;

    if (mode == MODE_LOW_POWER) {
        keep_smt = 0;
        if (amd_arch == AMD_ZEN4_PLUS) {
            target_phys_cores = (total_physical_cores >= 4) ? 4 : total_physical_cores;
        } else if (amd_arch == AMD_ZEN3) {
            target_phys_cores = (total_physical_cores >= 8) ? 6 : 4;
        } else {
            target_phys_cores = total_physical_cores;
        }
    } else if (mode == MODE_BALANCED) {
        keep_smt = 0;
        target_phys_cores = (total_physical_cores >= 8) ? 6 : 4;
    }

    int active_phys = 0;
    for (int i = 0; i < total_logical_cpus; i++) {
        int smt = is_smt_sibling(i);
        int online = 0;

        if (!smt) {
            if (active_phys < target_phys_cores) {
                online = 1;
                active_phys++;
            }
        } else if (keep_smt) {
            online = 1;
        }

        if (i == 0 || online) {
            if (i > 0) set_core(i, 1);
            set_governor(i, gov);
            set_epp(i, epp);
        } else {
            set_core(i, 0);
        }
    }
}

void apply_mode(PowerMode mode) {
    apply_intel_limits(mode);
    apply_amd_limits(mode);

    const char *gov = (mode == MODE_LOW_POWER) ? "powersave" :
                      (mode == MODE_BALANCED)  ? "schedutil" : "performance";
    const char *epp = (mode == MODE_LOW_POWER) ? "power" :
                      (mode == MODE_BALANCED)  ? "balance_performance" : "performance";

    if (cpu_vendor == VENDOR_AMD) {
        apply_amd_cores(mode, gov, epp);
        return;
    }

    if (mode == MODE_LOW_POWER) {
        set_governor(0, gov);
        set_epp(0, epp);
        for (int i = 1; i < total_logical_cpus; i++) {
            if (cpu_vendor == VENDOR_INTEL && is_smt_sibling(i)) {
                set_core(i, 0);
            } else {
                set_core(i, 1);
                set_governor(i, gov);
                set_epp(i, epp);
            }
        }
    } else {
        set_governor(0, gov);
        set_epp(0, epp);
        for (int i = 1; i < total_logical_cpus; i++) {
            set_core(i, 1);
            set_governor(i, gov);
            set_epp(i, epp);
        }
    }
}

PowerMode get_current_mode(void) {
    int fd = open(MODE_FILE, O_RDONLY);
    if (fd < 0) return MODE_BALANCED;

    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return MODE_BALANCED;

    if (strncmp(buf, "low-power", 9) == 0) return MODE_LOW_POWER;
    if (strncmp(buf, "performance", 11) == 0) return MODE_PERFORMANCE;
    return MODE_BALANCED;
}

int main(void) {
    if (acquire_singleton_lock() < 0) {
        fprintf(stderr, "[powerd] Another instance is already running (or missing root privileges).\n");
        return 1;
    }

    detect_hardware();

    int first_run = 1;
    PowerMode last_mode = MODE_BALANCED;

    while (1) {
        PowerMode mode = get_current_mode();
        if (first_run || mode != last_mode) {
            apply_mode(mode);
            last_mode = mode;
            first_run = 0;
        }
        sleep(1);
        update_telemetry();
    }
    return 0;
}
