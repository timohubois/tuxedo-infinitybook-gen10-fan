# TUXEDO InfinityBook Gen10 Fan Control

Minimal, silent fan control for TUXEDO InfinityBook Pro Gen10.

> **Hardware Notice:** This project has only been tested on a **TUXEDO InfinityBook Pro AMD Gen10** with a **Ryzen AI 9 HX 370** processor. It may work on other InfinityBook Gen10 variants, but this is untested. Use at your own risk.

## Why?

The stock kernel has no fan control for Uniwill-based laptops. TUXEDO provides their Control Center and custom kernel modules, but Control Center is a heavy Electron app and the tuxedo-drivers caused issues on my system - including the CPU randomly getting stuck at 600MHz. 

This project provides just fan control with no other baggage, keeping the rest native:

- **Minimal footprint** - ~17KB daemon + ~400KB kernel module
- **No dependencies** - works standalone without TUXEDO Control Center or tuxedo-drivers
- **Compatible with power-profiles-daemon** - handles only fan control, nothing else
- **Silent by default** - fans stay quiet when idle, ramp smoothly under load

## Recommended Setup

- Stock Arch Linux kernel (no tuxedo-drivers)
- `power-profiles-daemon` for power management
- This project for fan control

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                      User Space                             │
│                                                             │
│  ibg10-fanctl (daemon)                                      │
│      │                                                      │
│      ├──reads temps──▶ /sys/class/hwmon/ (k10temp, amdgpu) │
│      │                                                      │
│      ├──writes CPU fan─▶ /sys/class/tuxedo_infinitybook_gen10_fan/fan1_speed │
│      └──writes GPU fan─▶ /sys/class/tuxedo_infinitybook_gen10_fan/fan2_speed │
│                              │                              │
└──────────────────────────────│──────────────────────────────┘
                               │ sysfs
┌──────────────────────────────│──────────────────────────────┐
│  Kernel                      ▼                              │
│                    tuxedo_infinitybook_gen10_fan.ko         │
│                         │                                   │
│                         │ WMI calls                         │
│                         ▼                                   │
│                 ACPI WMI Interface ──▶ Embedded Controller  │
│                                              │              │
└──────────────────────────────────────────────│──────────────┘
                                               ▼
                                     CPU Fan & GPU Fan
```

**The daemon loop:**

1. Read CPU temp from `k10temp`, GPU temp from `amdgpu` (EC fallback for CPU if unavailable)
2. Calculate target speed for each fan independently using interpolated fan curve
3. Apply hysteresis (6°C gap prevents oscillation)
4. Write target speeds to sysfs (CPU fan follows CPU temp, GPU fan follows GPU temp)
5. Sleep 1s

**Fan curve:**

```
Fan %
100│                                    ┌────
 75│                            ┌───────┘
 50│                    ┌───────┘
 25│            ┌───────┘
~13│────────────┘ (minimum, prevents EC fighting)
    └────────┬───────┬───────┬───────┬───────┬──▶ Temp °C
            62      70      78      86      92
```

## Features

- **Silent fan curve**: Smooth, quiet operation with hysteresis
- **Direct EC control**: Communicates with EC via WMI interface
- **Independent dual fan control**: CPU fan follows CPU temp, GPU fan follows GPU temp
- **Real hwmon integration**: Reads temps from k10temp and amdgpu sensors
- **EC fallback**: Uses EC temperature sensor if hwmon unavailable
- **Systemd service**: Runs automatically on boot
- **No runtime dependencies**: Single binary, only links to libc

## Compatibility

**Tested on:**

- TUXEDO InfinityBook Pro AMD Gen10 (Ryzen AI 9 HX 370)
- Arch Linux, kernel 6.x
- WMI GUID `ABBC0F6F-8EA1-11D1-00A0-C90629100000`

## Installation

### Prerequisites

```bash
sudo pacman -S base-devel linux-headers dkms
```

### Step 1: Build and Test the Module

```bash
git clone https://github.com/timohubois/tuxedo-infinitybook-gen10-fan.git
cd tuxedo-infinitybook-gen10-fan

# Build
make

# Load module for testing
sudo make load

# Verify it works
cat /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/temp1      # EC temp (degrees C)
cat /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed # Fan speed (0-200)

# Test manual control
echo 100 | sudo tee /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed  # Set 50%
echo 50 | sudo tee /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed   # Set 25%
echo 1 | sudo tee /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan_auto      # Restore auto
```

If this works, proceed to step 2.

### Step 2: Test the Daemon

```bash
# Run daemon manually (Ctrl+C to stop)
sudo ./ibg10-fanctl
```

You should see temperature and fan speed updating. Run `./ibg10-fanctl -h` for help. If this works, proceed to step 3.

### Step 3: Install Permanently

#### Option A: DKMS (Recommended)

DKMS automatically rebuilds the module when you update your kernel:

```bash
sudo make tuxedo-infinitybook-gen10-fan-install-dkms
sudo systemctl enable --now tuxedo-infinitybook-gen10-fan.service
```

#### Option B: Manual Installation

Without DKMS, you'll need to rebuild manually after kernel updates:

```bash
sudo make install-all
sudo systemctl enable --now tuxedo-infinitybook-gen10-fan.service
```

### Manual Installation (Step-by-Step)

If you prefer step-by-step:

```bash
# Install module via DKMS (or use 'make install' for non-DKMS)
sudo make tuxedo-infinitybook-gen10-fan-dkms-install

# Auto-load on boot
sudo make install-autoload

# Install and enable service
sudo make install-service
sudo systemctl enable --now tuxedo-infinitybook-gen10-fan.service
```

## Usage

### Manual Control

```bash
# Load module
sudo modprobe tuxedo_infinitybook_gen10_fan

# Check current values
cat /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/temp1      # EC temp (degrees C)
cat /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed # Fan speed (0-200)

# Set fan speed (0-200, where 200 = 100%)
echo 100 | sudo tee /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed

# Restore automatic control
echo 1 | sudo tee /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan_auto
```

### Fan Curve Daemon

```bash
# Run manually (interactive mode with status display)
sudo ./ibg10-fanctl

# Show help and configuration
./ibg10-fanctl -h

# Or use the systemd service
sudo systemctl start tuxedo-infinitybook-gen10-fan.service
sudo systemctl status tuxedo-infinitybook-gen10-fan.service
```

### Configuration

The fan curve thresholds are compiled into the binary. To customize, edit `ibg10-fanctl.c` and rebuild:

```c
/* Temperature thresholds (C) */
#define TEMP_SILENT     62      /* Below this: minimum speed */
#define TEMP_LOW        70      /* Start of low speed */
#define TEMP_MED        78      /* Medium speed */
#define TEMP_HIGH       86      /* High speed */
#define TEMP_MAX        92      /* Maximum speed */

/* Fan speeds (0-200) */
#define SPEED_MIN       25      /* 12.5% - minimum to prevent EC fighting */
#define SPEED_LOW       50      /* 25% */
#define SPEED_MED       100     /* 50% */
#define SPEED_HIGH      150     /* 75% */
#define SPEED_MAX       200     /* 100% */
```

> **Note:** `SPEED_MIN` is 25 because values below 25 cause the EC's safety logic to periodically override the fan speed, resulting in annoying start/stop cycling.

Then rebuild and reinstall:

```bash
make ibg10-fanctl
sudo make install-service
```

## Uninstallation

### DKMS

```bash
sudo make tuxedo-infinitybook-gen10-fan-uninstall-dkms
```

### Non-DKMS

```bash
sudo make uninstall-all
```

Or manually:

```bash
sudo systemctl disable --now tuxedo-infinitybook-gen10-fan.service
sudo make uninstall-service
sudo make uninstall-autoload
sudo make uninstall
```

## Troubleshooting

### Module won't load

Check if WMI interface exists:

```bash
ls /sys/bus/wmi/devices/ | grep ABBC0F6
```

Check kernel logs:

```bash
dmesg | grep tuxedo_infinitybook
```

### Fans not responding

Verify the sysfs interface exists:

```bash
ls /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/
cat /sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed
```

### Service not starting

Check if module is loaded:

```bash
lsmod | grep tuxedo_infinitybook_gen10_fan
```

Check service status:

```bash
sudo systemctl status tuxedo-infinitybook-gen10-fan.service
sudo journalctl -u tuxedo-infinitybook-gen10-fan.service
```

### Fan never fully stops

This is intentional. The daemon keeps the fan at a minimum of 12.5% to prevent the EC from fighting for control, which would cause annoying start/stop cycling. The minimum speed is barely audible.

## Technical Details

### Sysfs Interface

| Path | Access | Description |
|------|--------|-------------|
| `/sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan1_speed` | RW | CPU fan speed (0-200) |
| `/sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan2_speed` | RW | GPU fan speed (0-200) |
| `/sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/temp1` | RO | EC CPU temperature sensor |
| `/sys/class/tuxedo_infinitybook_gen10_fan/tuxedo_infinitybook_gen10_fan/fan_auto` | WO | Write 1 to restore auto mode |

### EC Registers

The module uses the Uniwill WMI interface to communicate with the EC:

- Custom fan table: `0x0f00-0x0f5f`
- Direct fan control: `0x1804`, `0x1809`
- Fan mode: `0x0751`
- Custom profile mode: `0x0727` (bit 6) - required for IBP Gen10
- Manual mode: `0x0741`
- Custom fan table enable: `0x07c5` (bit 7)

### Fan Speed Values

The fan speed range is 0-200 (where 200 = 100%):

| Value | Behavior |
|-------|----------|
| 0 | Fan off |
| 1-24 | Clamped to 25 (minimum running speed) |
| 25-200 | Direct PWM control |

**Note:** The EC ramps fan speed smoothly, so changes aren't instant.

## License

GPL-2.0+

## Credits

- Based on reverse engineering of [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers)
- WMI interface discovery from TUXEDO Control Center
