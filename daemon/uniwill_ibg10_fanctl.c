/*
 * fanctl - TUXEDO InfinityBook Gen10 Silent Fan Control Daemon
 *
 * Userspace daemon for controlling fans via hwmon interface provided by
 * uniwill_ibg10_fanctl. On kernels 6.19+ (with in-tree uniwill-laptop),
 * it reads temperatures from upstream hwmon and writes PWM to the separate
 * hwmon device created by this module (uniwill_ibg10_fanctl).
 *
 * Control model (see daemon design notes):
 *   speed = interp( step_speed, cpu_load_steps + ram_load_steps )
 *           then floored on battery by the chassis comfort trickle.
 * Each heat source maps its temperature to a fractional number of thermal-load
 * steps; the steps are SUMMED, so simultaneous CPU + RAM load reaches higher
 * speeds than either alone (and the top of the curve is reachable at all - the
 * CPU self-limits ~70C). The step->speed table is the original non-linear CPU
 * curve, so with RAM idle the CPU maps exactly as before; output is interpolated
 * so it stays smooth. Measured: CPU-bound load self-limits ~67C; memory-bound
 * load (LLM) drives the DDR5 DIMMs toward ~85C while the CPU stays cool, but the
 * fan has little direct authority over the DIMMs, so RAM only nudges the total.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#define HWMON_BASE "/sys/class/hwmon"
#define PSU_BASE   "/sys/class/power_supply"

/* CPU/GPU curve temperature thresholds (C) - tuned, do not change lightly */
#define TEMP_OFF        55
#define TEMP_SILENT     60
#define TEMP_LOW        67
#define TEMP_MED        73
#define TEMP_HIGH       80
#define TEMP_MAX        90

/* Fan speeds (0-255 hwmon scale). EC uses 0-200; the module converts.
 * SPEED_SILENT is the lowest *quiet* speed: going slower is actually louder
 * (EC start/stop fighting + low-RPM resonance), so the controller never emits
 * 1..38 - it is either off or >= SPEED_SILENT. */
#define SPEED_OFF       0
#define SPEED_SILENT    39   /* ~15% - minimum running speed, barely audible */
#define SPEED_LOW       96
#define SPEED_MED       156
#define SPEED_HIGH      194
#define SPEED_MAX       255  /* 100% */

/* Combined-load "step" model.
 * Each heat source maps its (smoothed) temperature to a *fractional* number of
 * thermal-load steps; the steps are SUMMED and the total is looked up - with
 * interpolation - in step_speed[]. Summing (instead of max) means simultaneous
 * CPU + RAM load reaches higher speeds than either source alone, and it revives
 * the upper speed range the CPU curve never reached on its own (the CPU tops
 * out ~70C). Everything is piecewise-linear, so the output stays smooth.
 *
 * CPU/GPU contributes 0..5 steps across the TEMP_* thresholds (TEMP_OFF=0 ..
 * TEMP_MAX=5). RAM contributes only 0..2 steps and reaches the 2nd step only at
 * very high temps: the fan has little direct authority over the DIMMs (not in
 * the airflow path), so RAM mostly just nudges the combined total up. Real RAM
 * protection is the DDR5 self-throttle (~85C) and the workload, not the fan. */
#define RAM_T_OFF       68     /* idle RAM is ~50-58C; 0 steps below here */
#define RAM_T_STEP1     75     /* 1 step  - RAM warm */
#define RAM_T_STEP2     82     /* 2 steps - fully engaged BELOW the 85C spd5118 crit */

/* Chassis comfort floor: on battery (portable/lap) the fan always runs at the
 * quiet minimum. With the fan fully off there is zero airflow and the aluminium
 * soaks heat - it gets uncomfortably warm to the touch well before any component
 * is stressed (measured ~4C cooler surface with a trickle than fully off). The
 * RAM sensor is the only airflow-sensitive one but a poor idle proxy for skin
 * temperature, so we don't gate on it - we just keep air moving on battery. On
 * AC (desk) this is off, so the machine still goes fully silent at idle. */

/* Anti-cycling: the fan may only stop once demand has been OFF continuously
 * for this long. Any cooling demand re-arms the timer, so each activation
 * runs >= this many seconds and the on<->off edge can never bounce. */
#define OFF_DWELL_SEC   60

/* Timing */
#define POLL_INTERVAL   1       /* Seconds between updates */

/* Temperature smoothing to filter sensor spikes from localized chip heating */
#define TEMP_HISTORY_SIZE  8    /* Moving average window (samples) */

#define MAX_RAM_SENSORS 4

struct fan_state {
    int current;        /* Current speed (0-255) */
    int prev_target;    /* Previous target for trend */
};

struct temp_history {
    int samples[TEMP_HISTORY_SIZE];
    int index;
    int count;
};

struct temp_paths {
    char temp[512];
};

struct pwm_paths {
    char base[512];
    char pwm1[512];
    char pwm2[512];
    char pwm1_enable[512];
    char pwm2_enable[512];
    char ec_temp[512];
    int has_pwm2;
};

/* (temperature, step-value) anchor for a piecewise-linear load curve */
struct curve_point {
    int temp;
    int val;
};

static volatile sig_atomic_t running = 1;
static int interactive = 0;
static struct fan_state unified_fan = {0, -1};
static struct temp_paths cpu_temp_src;          /* k10temp or uniwill */
static struct temp_paths gpu_temp_src;          /* amdgpu */
static char ram_paths[MAX_RAM_SENSORS][512];    /* spd5118 DIMM sensors */
static int ram_count = 0;
static char battery_status_path[512];           /* power_supply Battery status */
static struct pwm_paths pwm_sink;               /* writable PWM device */
static struct temp_history cpugpu_smooth = {{0}, 0, 0};
static struct temp_history ram_smooth = {{0}, 0, 0};

/* CPU/GPU temperature -> fractional thermal-load steps (0..5) */
static const struct curve_point cpu_load_curve[] = {
    { TEMP_OFF,    0 },
    { TEMP_SILENT, 1 },
    { TEMP_LOW,    2 },
    { TEMP_MED,    3 },
    { TEMP_HIGH,   4 },
    { TEMP_MAX,    5 },
};

/* RAM temperature -> fractional thermal-load steps (0..2) */
static const struct curve_point ram_load_curve[] = {
    { RAM_T_OFF,   0 },
    { RAM_T_STEP1, 1 },
    { RAM_T_STEP2, 2 },
};

/* Combined steps (cpu 0..5 + ram 0..2 = 0..7) -> fan speed, interpolated.
 * Indices 0..5 are EXACTLY the original CPU SPEED_* curve points, so with RAM
 * idle the CPU maps identically to the old non-linear curve; RAM steps just
 * advance further along the same table. The 0..1 segment is special-cased in
 * steps_to_speed() to honor the quiet floor. */
static const int step_speed[] = {
    SPEED_OFF,     /* 0 - off */
    SPEED_SILENT,  /* 1 - 39  (~15%) quiet trickle */
    SPEED_LOW,     /* 2 - 96  (~38%) */
    SPEED_MED,     /* 3 - 156 (~61%) */
    SPEED_HIGH,    /* 4 - 194 (~76%) */
    SPEED_MAX,     /* 5 - 255 (100%) */
    SPEED_MAX,     /* 6 - 255 */
    SPEED_MAX,     /* 7 - 255 */
};

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* Read integer from sysfs file */
static int sysfs_read_int(const char *path)
{
    FILE *f;
    int val = -1;

    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &val) != 1)
            val = -1;
        fclose(f);
    }
    return val;
}

/* Write integer to sysfs file */
static int sysfs_write_int(const char *path, int val)
{
    FILE *f;
    int ret = -1;

    f = fopen(path, "w");
    if (f) {
        if (fprintf(f, "%d", val) > 0)
            ret = 0;
        fclose(f);
    }
    return ret;
}

/* Read string from sysfs file */
static int sysfs_read_str(const char *path, char *buf, size_t len)
{
    FILE *f;

    f = fopen(path, "r");
    if (!f)
        return -1;

    if (!fgets(buf, len, f)) {
        fclose(f);
        return -1;
    }

    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 0;
}

static int is_writable(const char *path)
{
    return access(path, W_OK) == 0;
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int find_hwmon_by_name(const char *name, char *out, size_t len)
{
    DIR *dir;
    struct dirent *ent;
    char path[512];
    char hwmon_name[128];

    dir = opendir(HWMON_BASE);
    if (!dir)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s/name", HWMON_BASE, ent->d_name);
        if (sysfs_read_str(path, hwmon_name, sizeof(hwmon_name)) == 0) {
            if (strcmp(hwmon_name, name) == 0) {
                snprintf(out, len, "%s/%s", HWMON_BASE, ent->d_name);
                closedir(dir);
                return 0;
            }
        }
    }

    closedir(dir);
    return -1;
}

/* Collect temp1_input paths for *all* hwmon devices with the given name
 * (e.g. there are two spd5118 DIMM sensors). Returns count found. */
static int find_all_temp_by_name(const char *name, char paths[][512], int max)
{
    DIR *dir;
    struct dirent *ent;
    char path[512];
    char hwmon_name[128];
    int count = 0;

    dir = opendir(HWMON_BASE);
    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL && count < max) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s/name", HWMON_BASE, ent->d_name);
        if (sysfs_read_str(path, hwmon_name, sizeof(hwmon_name)) == 0 &&
            strcmp(hwmon_name, name) == 0) {
            snprintf(paths[count], 512, "%s/%s/temp1_input", HWMON_BASE, ent->d_name);
            count++;
        }
    }

    closedir(dir);
    return count;
}

static int find_hwmon_with_pwm(char *out, size_t len)
{
    DIR *dir;
    struct dirent *ent;
    char base[512];
    char path[512];
    int i;

    dir = opendir(HWMON_BASE);
    if (!dir)
        return -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        snprintf(base, sizeof(base), "%s/%s", HWMON_BASE, ent->d_name);
        for (i = 1; i <= 3; i++) {
            int n = snprintf(path, sizeof(path), "%s/pwm%d", base, i);
            if (n < 0 || n >= (int)sizeof(path))
                continue;
            if (is_writable(path)) {
                n = snprintf(out, len, "%s", base);
                if (n >= 0 && n < (int)len) {
                    closedir(dir);
                    return 0;
                }
            }
        }
    }

    closedir(dir);
    return -1;
}

static void build_pwm_paths(struct pwm_paths *pp, const char *base)
{
    snprintf(pp->base, sizeof(pp->base), "%s", base);
    snprintf(pp->pwm1, sizeof(pp->pwm1), "%s/pwm1", base);
    snprintf(pp->pwm1_enable, sizeof(pp->pwm1_enable), "%s/pwm1_enable", base);
    snprintf(pp->pwm2, sizeof(pp->pwm2), "%s/pwm2", base);
    snprintf(pp->pwm2_enable, sizeof(pp->pwm2_enable), "%s/pwm2_enable", base);
    pp->has_pwm2 = exists(pp->pwm2) && exists(pp->pwm2_enable);
}

/* Get temperature in degrees C from temp1_input (millidegrees) */
static int get_temp(const struct temp_paths *src)
{
    int temp = sysfs_read_int(src->temp);
    if (temp < 0)
        return -1;
    return temp / 1000;
}

/* Hottest of the DIMM sensors in degrees C, or -1 if none readable */
static int get_ram_temp(void)
{
    int i, max = -1;

    for (i = 0; i < ram_count; i++) {
        int t = sysfs_read_int(ram_paths[i]);
        if (t < 0)
            continue;
        t /= 1000;
        if (t > max)
            max = t;
    }
    return max;
}

/* True when running off the battery (portable / lap). "Discharging" is the
 * one status that unambiguously means external power is absent; everything
 * else (Charging, Not charging, Full, or no battery at all) is treated as
 * on-mains. This also catches USB-C PD, not just the barrel jack. */
static int on_battery_power(void)
{
    char buf[32];

    if (!battery_status_path[0])
        return 0;
    if (sysfs_read_str(battery_status_path, buf, sizeof(buf)) < 0)
        return 0;
    return strcmp(buf, "Discharging") == 0;
}

/* Add temperature sample to history and return moving average */
static int smooth_temp(struct temp_history *hist, int temp)
{
    int i, sum = 0;

    hist->samples[hist->index] = temp;
    hist->index = (hist->index + 1) % TEMP_HISTORY_SIZE;
    if (hist->count < TEMP_HISTORY_SIZE)
        hist->count++;

    for (i = 0; i < hist->count; i++)
        sum += hist->samples[i];

    return sum / hist->count;
}

/* Piecewise-linear interpolation of a (temp -> value) anchor curve */
static double lerp_curve(int t, const struct curve_point *pts, int n)
{
    int i;

    if (t <= pts[0].temp)
        return pts[0].val;

    for (i = 1; i < n; i++) {
        if (t <= pts[i].temp) {
            int dt = pts[i].temp - pts[i - 1].temp;
            double dv = pts[i].val - pts[i - 1].val;
            return pts[i - 1].val + dv * (t - pts[i - 1].temp) / dt;
        }
    }
    return pts[n - 1].val;
}

/* Fractional thermal-load steps contributed by each source */
static double cpu_load(int temp)
{
    return lerp_curve(temp, cpu_load_curve, ARRAY_SIZE(cpu_load_curve));
}

static double ram_load(int temp)
{
    return lerp_curve(temp, ram_load_curve, ARRAY_SIZE(ram_load_curve));
}

/* Interpolate the summed step total through step_speed[]. The 0..1 segment
 * would land in the forbidden 1..38 band, so any nonzero result below
 * SPEED_SILENT is lifted to it: the fan is off, or at least the quiet minimum. */
static int steps_to_speed(double steps)
{
    int n = ARRAY_SIZE(step_speed);
    int i, s;
    double frac;

    if (steps <= 0)
        return step_speed[0];
    if (steps >= n - 1)
        return step_speed[n - 1];

    i = (int)steps;
    frac = steps - i;
    s = step_speed[i] + (int)((step_speed[i + 1] - step_speed[i]) * frac + 0.5);

    if (s > SPEED_OFF && s < SPEED_SILENT)
        s = SPEED_SILENT;
    return s;
}

/* On battery the fan always runs at the quiet minimum (see comment above);
 * on AC it contributes nothing, leaving idle fully silent. */
static int chassis_floor(int on_battery)
{
    return on_battery ? SPEED_SILENT : SPEED_OFF;
}

static const char *get_trend(int target, int *prev_target)
{
    const char *trend;

    if (*prev_target < 0)
        trend = " ";
    else if (target > *prev_target)
        trend = "^";
    else if (target < *prev_target)
        trend = "v";
    else
        trend = "=";

    *prev_target = target;
    return trend;
}

static void usage(const char *prog)
{
    printf("Usage: %s [-h]\n", prog);
    printf("\n");
    printf("Power-aware silent fan control for TUXEDO InfinityBook Gen10 (hwmon)\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h    Show this help message\n");
}

static int select_temp_sources(void)
{
    char base[384];

    /* CPU temp: prefer uniwill (if it exposes CPU temp), else k10temp */
    if (find_hwmon_by_name("uniwill", base, sizeof(base)) == 0)
        snprintf(cpu_temp_src.temp, sizeof(cpu_temp_src.temp), "%s/temp1_input", base);
    else if (find_hwmon_by_name("k10temp", base, sizeof(base)) == 0)
        snprintf(cpu_temp_src.temp, sizeof(cpu_temp_src.temp), "%s/temp1_input", base);
    else
        cpu_temp_src.temp[0] = '\0';

    /* GPU temp: amdgpu */
    if (find_hwmon_by_name("amdgpu", base, sizeof(base)) == 0)
        snprintf(gpu_temp_src.temp, sizeof(gpu_temp_src.temp), "%s/temp1_input", base);
    else
        gpu_temp_src.temp[0] = '\0';

    /* RAM temp: every spd5118 DIMM sensor (optional - degrades to CPU-only) */
    ram_count = find_all_temp_by_name("spd5118", ram_paths, MAX_RAM_SENSORS);

    /* Fallback: if both empty, try uniwill as EC temp */
    if (!cpu_temp_src.temp[0] && !gpu_temp_src.temp[0]) {
        if (find_hwmon_by_name("uniwill", base, sizeof(base)) == 0)
            snprintf(cpu_temp_src.temp, sizeof(cpu_temp_src.temp), "%s/temp1_input", base);
    }

    return (cpu_temp_src.temp[0] || gpu_temp_src.temp[0]) ? 0 : -1;
}

/* Locate the Battery power supply's status file (not fatal if absent) */
static void select_power_source(void)
{
    DIR *dir;
    struct dirent *ent;
    char path[512];
    char type[32];

    battery_status_path[0] = '\0';

    dir = opendir(PSU_BASE);
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s/type", PSU_BASE, ent->d_name);
        if (sysfs_read_str(path, type, sizeof(type)) == 0 &&
            strcmp(type, "Battery") == 0) {
            snprintf(battery_status_path, sizeof(battery_status_path),
                     "%s/%s/status", PSU_BASE, ent->d_name);
            break;
        }
    }
    closedir(dir);
}

static int select_pwm_sink(void)
{
    char base[384];

    /* Prefer our standalone hwmon device name */
    if (find_hwmon_by_name("uniwill_ibg10_fanctl", base, sizeof(base)) == 0) {
        build_pwm_paths(&pwm_sink, base);
        return 0;
    }

    /* Otherwise any writable hwmon with pwm1 */
    if (find_hwmon_with_pwm(base, sizeof(base)) == 0) {
        build_pwm_paths(&pwm_sink, base);
        return 0;
    }

    return -1;
}

static void print_banner(void)
{
    printf("\n");
    printf("  TUXEDO InfinityBook Gen10 Silent Fan Control (hwmon)\n");
    printf("  ----------------------------------------------------\n");
    printf("  CPU/GPU steps:  0 at %dC .. 5 at %dC (original non-linear curve)\n",
           TEMP_OFF, TEMP_MAX);
    printf("  RAM steps:      0 at %dC .. 2 at %dC (weak; nudges the total up)\n",
           RAM_T_OFF, RAM_T_STEP2);
    printf("  Chassis floor:  on battery the fan always runs (>= min, lap comfort)\n");
    printf("  Off dwell:      fan holds %ds after demand ends (anti-cycle)\n",
           OFF_DWELL_SEC);
    printf("\n");
    printf("  Temp source (CPU): %s\n", cpu_temp_src.temp[0] ? cpu_temp_src.temp : "none");
    printf("  Temp source (GPU): %s\n", gpu_temp_src.temp[0] ? gpu_temp_src.temp : "none");
    printf("  Temp source (RAM): %d sensor(s)\n", ram_count);
    printf("  Power source:      %s\n", battery_status_path[0] ? battery_status_path : "none (assume AC)");
    printf("  PWM sink:          %s\n", pwm_sink.base[0] ? pwm_sink.base : "none");
    printf("  Mode: speed = steps(cpu/gpu) + steps(ram)%s, interpolated\n",
           battery_status_path[0] ? ", floored on battery" : "");
    printf("\n");
    printf("  Trend: ^ = ramping up, v = slowing down, = = steady\n");
    printf("  Ctrl+C to stop and restore automatic control\n");
    printf("\n");
    printf("Time     | CPU | GPU | RAM | Load | Fan       | Pwr\n");
    printf("---------|-----|-----|-----|------|-----------|----\n");
}

static int set_manual_mode(void)
{
    /* 1 = manual, 2 = auto */
    int ret = 0;

    if (pwm_sink.pwm1_enable[0])
        ret |= sysfs_write_int(pwm_sink.pwm1_enable, 1);
    if (pwm_sink.has_pwm2 && pwm_sink.pwm2_enable[0])
        ret |= sysfs_write_int(pwm_sink.pwm2_enable, 1);

    return ret;
}

static void restore_auto(void)
{
    if (pwm_sink.pwm1_enable[0])
        sysfs_write_int(pwm_sink.pwm1_enable, 2);
    if (pwm_sink.has_pwm2 && pwm_sink.pwm2_enable[0])
        sysfs_write_int(pwm_sink.pwm2_enable, 2);
}

int main(int argc, char *argv[])
{
    int target = 0;
    time_t last_demand;
    time_t now;
    struct tm *tm_info;
    struct timespec ts;
    char time_buf[16];
    int opt;

    while ((opt = getopt(argc, argv, "h")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    interactive = isatty(STDOUT_FILENO);

    if (select_temp_sources() < 0) {
        fprintf(stderr, "Error: no temperature sensor (uniwill/k10temp/amdgpu) found under %s\n", HWMON_BASE);
        return 1;
    }

    select_power_source();

    if (select_pwm_sink() < 0) {
        fprintf(stderr, "Error: no writable PWM device found under %s (expected uniwill_ibg10_fanctl)\n", HWMON_BASE);
        return 1;
    }

    if (set_manual_mode() < 0) {
        fprintf(stderr, "Error: failed to set manual mode on %s\n", pwm_sink.base);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (interactive) {
        print_banner();
        printf("\n");
    } else {
        printf("Starting fan control daemon...\n");
    }

    last_demand = time(NULL);

    while (running) {
        int cpu_t = cpu_temp_src.temp[0] ? get_temp(&cpu_temp_src) : -1;
        int gpu_t = gpu_temp_src.temp[0] ? get_temp(&gpu_temp_src) : -1;
        int ram_raw = get_ram_temp();
        int cpugpu_raw, cpugpu, ram, on_bat, prot, floor, demand;
        double load;

        /* CPU/GPU follow the hotter of the two (shared heatpipes) */
        if (cpu_t < 0 && gpu_t < 0)
            cpugpu_raw = 0;
        else if (cpu_t < 0)
            cpugpu_raw = gpu_t;
        else if (gpu_t < 0)
            cpugpu_raw = cpu_t;
        else
            cpugpu_raw = (cpu_t > gpu_t) ? cpu_t : gpu_t;

        cpugpu = smooth_temp(&cpugpu_smooth, cpugpu_raw);
        ram = (ram_raw >= 0) ? smooth_temp(&ram_smooth, ram_raw) : 0;
        on_bat = on_battery_power();

        /* Summed thermal-load steps -> interpolated speed, + battery floor */
        load = cpu_load(cpugpu) + ram_load(ram);
        prot = steps_to_speed(load);

        floor = chassis_floor(on_bat);
        demand = (prot > floor) ? prot : floor;

        /* Re-arming off-dwell: the fan may only stop after demand has been
         * OFF for OFF_DWELL_SEC continuously; any demand resets the clock. */
        now = time(NULL);
        if (demand > 0) {
            last_demand = now;
            target = demand;
        } else if (now - last_demand >= OFF_DWELL_SEC) {
            target = SPEED_OFF;
        } else {
            target = SPEED_SILENT;   /* hold trickle until the minute elapses */
        }

        sysfs_write_int(pwm_sink.pwm1, target);
        if (pwm_sink.has_pwm2)
            sysfs_write_int(pwm_sink.pwm2, target);

        unified_fan.current = target;

        if (interactive) {
            now = time(NULL);
            tm_info = localtime(&now);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

            printf("\033[1A");
            printf("%s | %3d | %3d | %3d | %4.1f | %3d%% %-3s | %s\n",
                   time_buf,
                   cpu_t >= 0 ? cpu_t : 0,
                   gpu_t >= 0 ? gpu_t : 0,
                   ram_raw >= 0 ? ram_raw : 0,
                   load,
                   target * 100 / 255,
                   get_trend(target, &unified_fan.prev_target),
                   on_bat ? "BAT" : "AC");
            fflush(stdout);
        }

        ts.tv_nsec = 0;
        ts.tv_sec = POLL_INTERVAL;
        nanosleep(&ts, NULL);
    }

    restore_auto();
    return 0;
}
