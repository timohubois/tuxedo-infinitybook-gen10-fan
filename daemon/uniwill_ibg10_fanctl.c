/*
 * fanctl - TUXEDO InfinityBook Gen10 Silent Fan Control Daemon
 *
 * Userspace daemon for controlling fans via hwmon interface provided by
 * uniwill_ibg10_fanctl. On kernels 6.19+ (with in-tree uniwill-laptop),
 * it reads temperatures from upstream hwmon and writes PWM to the separate
 * hwmon device created by this module (uniwill_ibg10_fanctl).
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

/* Temperature thresholds (C) */
#define TEMP_OFF        54
#define TEMP_SILENT     61
#define TEMP_LOW        67
#define TEMP_MED        73
#define TEMP_HIGH       80
#define TEMP_MAX        90

/* Hysteresis - how much cooler before stepping down */
#define HYSTERESIS      3

/* Fan speeds (0-255 hwmon scale). EC uses 0-200; we convert. */
#define SPEED_OFF       0
#define SPEED_MIN       39   /* ~15% of 255 rounded */
#define SPEED_LOW       96
#define SPEED_MED       128
#define SPEED_HIGH      192
#define SPEED_MAX       255  /* 100% */

/* Timing */
#define POLL_INTERVAL   1       /* Seconds between updates */

/* Temperature smoothing to filter sensor spikes from localized chip heating */
#define TEMP_HISTORY_SIZE  8    /* Moving average window (samples) */

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

static volatile sig_atomic_t running = 1;
static int interactive = 0;
static struct fan_state unified_fan = {0, -1};
static struct temp_paths cpu_temp_src; /* k10temp or uniwill */
static struct temp_paths gpu_temp_src; /* amdgpu */
static struct pwm_paths pwm_sink;      /* writable PWM device (uniwill_ibg10_fanctl) */
static struct temp_history temp_smooth = {{0}, 0, 0};

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

/* Add temperature sample to history and return moving average */
static int smooth_temp(struct temp_history *hist, int temp)
{
    int i, sum = 0;

    /* Add new sample to circular buffer */
    hist->samples[hist->index] = temp;
    hist->index = (hist->index + 1) % TEMP_HISTORY_SIZE;
    if (hist->count < TEMP_HISTORY_SIZE)
        hist->count++;

    /* Calculate average of available samples */
    for (i = 0; i < hist->count; i++)
        sum += hist->samples[i];

    return sum / hist->count;
}

/* Linear interpolation for smooth fan curve */
static int interpolate_speed(int temp)
{
    int range, pos;

    if (temp <= TEMP_OFF) {
        return SPEED_OFF;
    } else if (temp <= TEMP_SILENT) {
        return SPEED_MIN;
    } else if (temp <= TEMP_LOW) {
        range = TEMP_LOW - TEMP_SILENT;
        pos = temp - TEMP_SILENT;
        return SPEED_MIN + (SPEED_LOW - SPEED_MIN) * pos / range;
    } else if (temp <= TEMP_MED) {
        range = TEMP_MED - TEMP_LOW;
        pos = temp - TEMP_LOW;
        return SPEED_LOW + (SPEED_MED - SPEED_LOW) * pos / range;
    } else if (temp <= TEMP_HIGH) {
        range = TEMP_HIGH - TEMP_MED;
        pos = temp - TEMP_MED;
        return SPEED_MED + (SPEED_HIGH - SPEED_MED) * pos / range;
    } else if (temp <= TEMP_MAX) {
        range = TEMP_MAX - TEMP_HIGH;
        pos = temp - TEMP_HIGH;
        return SPEED_HIGH + (SPEED_MAX - SPEED_HIGH) * pos / range;
    } else {
        return SPEED_MAX;
    }
}

static int calc_target(int temp, int previous_target)
{
    int target = interpolate_speed(temp);

    if (target < previous_target) {
        int cooler_target = interpolate_speed(temp + HYSTERESIS);
        if (cooler_target >= previous_target)
            target = previous_target;
    }

    return target;
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
    printf("Silent fan control for TUXEDO InfinityBook Gen10 (hwmon)\n");
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

    /* Fallback: if both empty, try uniwill as EC temp */
    if (!cpu_temp_src.temp[0] && !gpu_temp_src.temp[0]) {
        if (find_hwmon_by_name("uniwill", base, sizeof(base)) == 0)
            snprintf(cpu_temp_src.temp, sizeof(cpu_temp_src.temp), "%s/temp1_input", base);
    }

    return (cpu_temp_src.temp[0] || gpu_temp_src.temp[0]) ? 0 : -1;
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
    printf("  Fan off:    < %d C\n", TEMP_OFF);
    printf("  Fan silent: %d-%d C\n", TEMP_OFF, TEMP_SILENT);
    printf("  Low speed:  %d-%d C\n", TEMP_SILENT, TEMP_LOW);
    printf("  Med speed:  %d-%d C\n", TEMP_LOW, TEMP_MED);
    printf("  High speed: %d-%d C\n", TEMP_MED, TEMP_HIGH);
    printf("  Max speed:  > %d C\n", TEMP_MAX);
    printf("\n");
    printf("  Temp source (CPU): %s\n", cpu_temp_src.temp[0] ? cpu_temp_src.temp : "none");
    printf("  Temp source (GPU): %s\n", gpu_temp_src.temp[0] ? gpu_temp_src.temp : "none");
    printf("  PWM sink:          %s\n", pwm_sink.base[0] ? pwm_sink.base : "none");
    printf("  Mode: Unified (both fans follow max temp - shared heatpipes)\n");
    printf("\n");
    printf("  Trend: ^ = ramping up, v = slowing down, = = steady\n");
    printf("  Ctrl+C to stop and restore automatic control\n");
    printf("\n");
    printf("Time     | CPU | GPU | Fan\n");
    printf("---------|-----|-----|-------\n");
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
    int temp;
    int target = 0;
    //int fan_actual1, fan_actual2;
    //int fan_actual_avg;
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

    while (running) {
        int cpu_t = cpu_temp_src.temp[0] ? get_temp(&cpu_temp_src) : -1;
        int gpu_t = gpu_temp_src.temp[0] ? get_temp(&gpu_temp_src) : -1;
        int raw_temp;

        if (cpu_t < 0 && gpu_t < 0)
            raw_temp = 0;
        else if (cpu_t < 0)
            raw_temp = gpu_t;
        else if (gpu_t < 0)
            raw_temp = cpu_t;
        else
            raw_temp = (cpu_t > gpu_t) ? cpu_t : gpu_t;

        /* Smooth temperature to filter sensor spikes from localized heating */
        temp = smooth_temp(&temp_smooth, raw_temp);

	/*
        fan_actual1 = sysfs_read_int(pwm_sink.pwm1);
        if (fan_actual1 < 0)
            fan_actual1 = 0;
        fan_actual2 = pwm_sink.has_pwm2 ? sysfs_read_int(pwm_sink.pwm2) : fan_actual1;
        if (fan_actual2 < 0)
            fan_actual2 = fan_actual1;

        fan_actual_avg = (fan_actual1 + fan_actual2) / 2;
        unified_fan.current = fan_actual_avg;
	*/
        target = calc_target(temp, target);

        sysfs_write_int(pwm_sink.pwm1, target);
        if (pwm_sink.has_pwm2)
            sysfs_write_int(pwm_sink.pwm2, target);

        if (interactive) {
            now = time(NULL);
            tm_info = localtime(&now);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

            printf("\033[1A");
            printf("%s | %3d | %3d | %3d%% %s\n",
                   time_buf,
                   cpu_t >= 0 ? cpu_t : 0,
                   gpu_t >= 0 ? gpu_t : 0,
                   target * 100 / 255,
                   get_trend(target, &unified_fan.prev_target));
            fflush(stdout);
        }

        ts.tv_nsec = 0;
        ts.tv_sec = POLL_INTERVAL;
        nanosleep(&ts, NULL);
    }

    restore_auto();
    return 0;
}
