/*
 * fanctl - TUXEDO InfinityBook Gen10 Silent Fan Control Daemon
 *
 * Userspace daemon for controlling fans via the tuxedo_infinitybook_gen10_fan kernel module.
 * Implements a smooth, silent fan curve with hysteresis.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>

/* Sysfs paths */
#define SYSFS_BASE      "/sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan"
#define SYSFS_FAN1      SYSFS_BASE "/fan1_speed"
#define SYSFS_FAN2      SYSFS_BASE "/fan2_speed"
#define SYSFS_FAN_AUTO  SYSFS_BASE "/fan_auto"
#define SYSFS_TEMP1     SYSFS_BASE "/temp1"
#define HWMON_BASE      "/sys/class/hwmon"

/* Temperature thresholds (C) */
#define TEMP_SILENT     62
#define TEMP_LOW        70
#define TEMP_MED        78
#define TEMP_HIGH       86
#define TEMP_MAX        92

/* Hysteresis - how much cooler before stepping down */
#define HYSTERESIS      6

/*
 * Fan speeds (0-200)
 *
 * SPEED_MIN is set to 25 (12.5%) instead of 0 to prevent EC fighting.
 * When the fan is set to 0 (off), the EC's safety logic periodically
 * tries to spin it back up, causing annoying start/stop cycling.
 * Keeping a minimum speed prevents this and is also better for
 * component longevity.
 */
#define SPEED_MIN       25      /* 12.5% - minimum to prevent EC fighting */
#define SPEED_LOW       50      /* 25% */
#define SPEED_MED       100     /* 50% */
#define SPEED_HIGH      150     /* 75% */
#define SPEED_MAX       200     /* 100% */

/* Timing */
#define POLL_INTERVAL   1       /* Seconds between updates */

/* Per-fan state for independent control */
struct fan_state {
    int current;        /* Current speed */
    int prev_target;    /* Previous target for trend */
};

/* State */
static volatile sig_atomic_t running = 1;
static int interactive = 0;
static struct fan_state cpu_fan = {0, -1};
static struct fan_state gpu_fan = {0, -1};
static char hwmon_cpu[384] = {0};
static char hwmon_gpu[384] = {0};

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

    /* Remove trailing newline */
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 0;
}

/* Find hwmon path by name (e.g., "k10temp", "amdgpu") */
static int find_hwmon(const char *name, char *out, size_t len)
{
    DIR *dir;
    struct dirent *ent;
    char path[384];
    char hwmon_name[64];

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

/* Get temperature from hwmon (returns millidegrees, we convert to degrees) */
static int get_hwmon_temp(const char *hwmon_path)
{
    char path[512];
    int temp;

    if (!hwmon_path[0])
        return 0;

    snprintf(path, sizeof(path), "%s/temp1_input", hwmon_path);
    temp = sysfs_read_int(path);
    if (temp < 0)
        return 0;

    return temp / 1000;
}

/* Get current temperatures with fallback logic */
static void get_temp(int *cpu_temp, int *gpu_temp)
{
    int cpu_hwmon, gpu_hwmon, ec_temp;

    /* Read from hwmon sensors */
    cpu_hwmon = get_hwmon_temp(hwmon_cpu);
    gpu_hwmon = get_hwmon_temp(hwmon_gpu);

    /* Read EC temp as fallback */
    ec_temp = sysfs_read_int(SYSFS_TEMP1);
    if (ec_temp <= 0)
        ec_temp = 0;

    /* CPU temp: k10temp -> EC -> GPU temp (APU shares die) */
    if (cpu_hwmon > 0)
        *cpu_temp = cpu_hwmon;
    else if (ec_temp > 0)
        *cpu_temp = ec_temp;
    else
        *cpu_temp = gpu_hwmon;  /* APU fallback */

    /* GPU temp: amdgpu -> CPU temp (APU shares die) */
    if (gpu_hwmon > 0)
        *gpu_temp = gpu_hwmon;
    else
        *gpu_temp = *cpu_temp;  /* APU fallback - use CPU temp */
}

/* Linear interpolation for smooth fan curve */
static int interpolate_speed(int temp)
{
    int range, pos;

    if (temp <= TEMP_SILENT) {
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

/* Calculate target speed with hysteresis for a specific fan */
static int calc_target(int temp, struct fan_state *fan)
{
    int target;

    target = interpolate_speed(temp);

    /* Apply hysteresis - only step down if significantly cooler */
    if (target < fan->current) {
        int cooler_target = interpolate_speed(temp + HYSTERESIS);
        if (cooler_target >= fan->current) {
            /* Not cool enough to step down yet */
            target = fan->current;
        }
    }

    return target;
}

/* Restore automatic fan control */
static void restore_auto(void)
{
    if (interactive)
        printf("\nRestoring automatic fan control...\n");
    sysfs_write_int(SYSFS_FAN_AUTO, 1);
    if (interactive)
        printf("Done.\n");
}

static void print_banner(void)
{
	printf("\n");
	printf("  TUXEDO InfinityBook Gen10 Silent Fan Control\n");
	printf("  ---------------------------------------------\n");
	printf("  Fan off:    < %d C\n", TEMP_SILENT);
	printf("  Low speed:  %d-%d C\n", TEMP_SILENT, TEMP_LOW);
	printf("  Med speed:  %d-%d C\n", TEMP_LOW, TEMP_MED);
	printf("  High speed: %d-%d C\n", TEMP_MED, TEMP_HIGH);
	printf("  Max speed:  > %d C\n", TEMP_MAX);
	printf("\n");
	printf("  CPU sensor: %s\n", hwmon_cpu[0] ? hwmon_cpu : "EC fallback");
	printf("  GPU sensor: %s\n", hwmon_gpu[0] ? hwmon_gpu : "none");
	printf("  Mode: Independent (CPU fan follows CPU temp, GPU fan follows GPU temp)\n");
	printf("\n");
	printf("  Trend: ^ = ramping up, v = slowing down, = = steady\n");
	printf("  Ctrl+C to stop and restore automatic control\n");
	printf("\n");
	printf("Time     |     | Tmp | Fan\n");
	printf("---------|-----|-----|-------\n");
}

static const char *get_trend(int target, int *prev_target)
{
    const char *trend;

    if (*prev_target < 0)
        trend = " ";        /* First reading */
    else if (target > *prev_target)
        trend = "^";        /* Ramping up */
    else if (target < *prev_target)
        trend = "v";        /* Slowing down */
    else
        trend = "=";        /* Steady */

    *prev_target = target;
    return trend;
}

static void usage(const char *prog)
{
	printf("Usage: %s [-h]\n", prog);
	printf("\n");
	printf("TUXEDO InfinityBook Gen10 Silent Fan Control Daemon\n");
	printf("\n");
	printf("Controls laptop fans via the tuxedo_infinitybook_gen10_fan kernel module.\n");
	printf("Runs interactively with status display, or as a background daemon.\n");
	printf("\n");
	printf("Options:\n");
	printf("  -h    Show this help message\n");
	printf("\n");
	printf("Temperature thresholds:\n");
	printf("  Fan off:    < %d C\n", TEMP_SILENT);
	printf("  Low speed:  %d-%d C (25%%)\n", TEMP_SILENT, TEMP_LOW);
	printf("  Med speed:  %d-%d C (50%%)\n", TEMP_LOW, TEMP_MED);
	printf("  High speed: %d-%d C (75%%)\n", TEMP_MED, TEMP_HIGH);
	printf("  Max speed:  > %d C (100%%)\n", TEMP_MAX);
}

int main(int argc, char *argv[])
{
    int cpu_temp, gpu_temp;
    int cpu_target, gpu_target;
    int cpu_actual, gpu_actual;
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

    /* Check if running interactively */
    interactive = isatty(STDOUT_FILENO);

	/* Check if kernel module is loaded */
	if (access(SYSFS_BASE, F_OK) != 0) {
		fprintf(stderr, "Error: tuxedo_infinitybook_gen10_fan module not loaded\n");
		return 1;
	}

    /* Find hwmon sensors */
    find_hwmon("k10temp", hwmon_cpu, sizeof(hwmon_cpu));
    find_hwmon("amdgpu", hwmon_gpu, sizeof(hwmon_gpu));

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (interactive) {
        print_banner();
        /* Print initial blank lines for cursor-up to work */
        printf("\n\n");
    } else {
        printf("Starting fan control daemon...\n");
    }

    /* Take over fan control */
    sysfs_write_int(SYSFS_FAN_AUTO, 0);

    /* Main loop */
    while (running) {
        get_temp(&cpu_temp, &gpu_temp);

        /* Read current fan speeds */
        cpu_actual = sysfs_read_int(SYSFS_FAN1);
        if (cpu_actual < 0)
            cpu_actual = 0;
        gpu_actual = sysfs_read_int(SYSFS_FAN2);
        if (gpu_actual < 0)
            gpu_actual = 0;

        /* Update fan state with current readings */
        cpu_fan.current = cpu_actual;
        gpu_fan.current = gpu_actual;

        /* Calculate independent targets */
        cpu_target = calc_target(cpu_temp, &cpu_fan);
        gpu_target = calc_target(gpu_temp, &gpu_fan);

        /* Write to fans independently */
        sysfs_write_int(SYSFS_FAN1, cpu_target);
        sysfs_write_int(SYSFS_FAN2, gpu_target);

        /* Display */
        if (interactive) {
            now = time(NULL);
            tm_info = localtime(&now);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

            /* Move cursor up 2 lines and overwrite */
            printf("\033[2A");
            printf("%s | CPU | %3d | %3d%% %s\n",
                   time_buf,
                   cpu_temp,
                   cpu_target * 100 / 200,
                   get_trend(cpu_target, &cpu_fan.prev_target));
            printf("         | GPU | %3d | %3d%% %s\n",
                   gpu_temp,
                   gpu_target * 100 / 200,
                   get_trend(gpu_target, &gpu_fan.prev_target));
            fflush(stdout);
        }

        /* Sleep until next update */
        ts.tv_nsec = 0;
        ts.tv_sec = POLL_INTERVAL;
        nanosleep(&ts, NULL);
    }

    restore_auto();
    return 0;
}
