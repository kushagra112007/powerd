#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <glob.h>

#define MODE_FILE  "/tmp/powerd.mode"
#define WATTS_FILE "/tmp/powerd.watts"

typedef enum { VENDOR_INTEL, VENDOR_AMD, VENDOR_UNKNOWN } CpuVendor;
typedef enum { TELEM_NONE, TELEM_HWMON_UW, TELEM_RAPL_UJ } TelemType;
typedef enum { MODE_LOW_POWER, MODE_BALANCED, MODE_PERFORMANCE } PowerMode;

static CpuVendor cpu_vendor = VENDOR_UNKNOWN;
static TelemType telem_type = TELEM_NONE;
static char telemetry_path[128] = {0};
static unsigned long long prev_energy_uj = 0;

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

printf("[powerd] CPU Vendor    : %s\n",
(cpu_vendor == VENDOR_AMD) ? "AMD Ryzen" :
(cpu_vendor == VENDOR_INTEL) ? "Intel" : "Unknown");
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
}

if (watts >= 0.0) {
FILE *wf = fopen(WATTS_FILE, "w");
if (wf) {
fprintf(wf, "%.2f\n", watts);
fclose(wf);
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

void apply_intel_limits(PowerMode mode) {
    if (cpu_vendor != VENDOR_INTEL) return;

    const char *no_turbo = (mode == MODE_LOW_POWER) ? "1" : "0";
    const char *pl1_uw = (mode == MODE_LOW_POWER)   ? "6000000" :
                         (mode == MODE_BALANCED)    ? "15000000" : "35000000";

    write_sysfs_safe("/sys/devices/system/cpu/intel_pstate/no_turbo", no_turbo);

    if (write_sysfs_safe("/sys/class/powercap/intel-rapl/intel-rapl:0/constraint_0_power_limit_uw", pl1_uw) < 0) {
        write_sysfs_safe("/sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw", pl1_uw);
    }
}

void apply_mode(PowerMode mode) {
    apply_intel_limits(mode);
long total_cpus = sysconf(_SC_NPROCESSORS_CONF);
if (total_cpus <= 0) total_cpus = 16;

if (mode == MODE_LOW_POWER) {
set_governor(0, "powersave");
for (int i = 1; i < total_cpus; i++) {
if (is_smt_sibling(i)) {
set_core(i, 0);
} else if (0) {
set_core(i, 0);
} else {
set_core(i, 1);
set_governor(i, "powersave");
}
}
} else if (mode == MODE_BALANCED) {
set_governor(0, "schedutil");
for (int i = 1; i < total_cpus; i++) {
set_core(i, 1);
set_governor(i, "schedutil");
}
} else if (mode == MODE_PERFORMANCE) {
set_governor(0, "performance");
for (int i = 1; i < total_cpus; i++) {
set_core(i, 1);
set_governor(i, "performance");
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
update_telemetry();
}
return 0;
}
