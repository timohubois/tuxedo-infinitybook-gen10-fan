obj-m += tuxedo_infinitybook_gen10_fan.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr/local

# DKMS variables
DKMS_NAME := tuxedo-infinitybook-gen10-fan
DKMS_VERSION := 0.1.0
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all clean install uninstall install-service uninstall-service load unload ibg10-fanctl \
        tuxedo-infinitybook-gen10-fan-dkms-install tuxedo-infinitybook-gen10-fan-dkms-uninstall tuxedo-infinitybook-gen10-fan-dkms-status

all: modules ibg10-fanctl

modules:
	make -C $(KDIR) M=$(PWD) modules

ibg10-fanctl: ibg10-fanctl.c
	$(CC) $(CFLAGS) -o ibg10-fanctl ibg10-fanctl.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f ibg10-fanctl

# Module installation (run 'make' first as normal user)
install:
	install -d /lib/modules/$(KVER)/extra/
	install -m 644 tuxedo_infinitybook_gen10_fan.ko /lib/modules/$(KVER)/extra/
	depmod -a
	@echo "Module installed. Load with: modprobe tuxedo_infinitybook_gen10_fan"

uninstall:
	rm -f /lib/modules/$(KVER)/extra/tuxedo_infinitybook_gen10_fan.ko
	depmod -a
	@echo "Module uninstalled"

# Service installation (run 'make' first as normal user)
install-service:
	install -m 755 ibg10-fanctl $(PREFIX)/bin/ibg10-fanctl
	install -m 644 tuxedo-infinitybook-gen10-fan.service /etc/systemd/system/
	sed -i 's|ExecStart=.*|ExecStart=$(PREFIX)/bin/ibg10-fanctl|' /etc/systemd/system/tuxedo-infinitybook-gen10-fan.service
	systemctl daemon-reload
	@echo ""
	@echo "Service installed. Enable with:"
	@echo "  systemctl enable --now tuxedo-infinitybook-gen10-fan.service"

uninstall-service:
	-systemctl stop tuxedo-infinitybook-gen10-fan.service 2>/dev/null
	-systemctl disable tuxedo-infinitybook-gen10-fan.service 2>/dev/null
	rm -f /etc/systemd/system/tuxedo-infinitybook-gen10-fan.service
	rm -f $(PREFIX)/bin/ibg10-fanctl
	systemctl daemon-reload
	@echo "Service uninstalled"

# Module loading (for testing)
load: all
	-rmmod tuxedo_infinitybook_gen10_fan 2>/dev/null
	insmod ./tuxedo_infinitybook_gen10_fan.ko
	@echo "Module loaded"

unload:
	-rmmod tuxedo_infinitybook_gen10_fan 2>/dev/null
	@echo "Module unloaded"

# Auto-load on boot
install-autoload:
	echo "tuxedo_infinitybook_gen10_fan" > /etc/modules-load.d/tuxedo-infinitybook-gen10-fan.conf
	@echo "Module will load on boot"

uninstall-autoload:
	rm -f /etc/modules-load.d/tuxedo-infinitybook-gen10-fan.conf
	@echo "Auto-load disabled"

# Full install (module + autoload + service)
install-all: install install-autoload install-service
	@echo ""
	@echo "=== Installation Complete ==="
	@echo "The module will load on boot and fan control will start automatically."

uninstall-all: uninstall-service uninstall-autoload uninstall
	@echo ""
	@echo "=== Uninstallation Complete ==="

# DKMS installation (auto-rebuilds on kernel updates)
tuxedo-infinitybook-gen10-fan-dkms-install: ibg10-fanctl
	@echo "Installing DKMS module source..."
	install -d $(DKMS_SRC)
	install -m 644 tuxedo_infinitybook_gen10_fan.c $(DKMS_SRC)/
	install -m 644 dkms.conf $(DKMS_SRC)/
	@echo "obj-m += tuxedo_infinitybook_gen10_fan.o" > $(DKMS_SRC)/Makefile
	dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION)
	@echo ""
	@echo "=== DKMS Installation Complete ==="
	@echo "Module will auto-rebuild on kernel updates."
	@echo "Load with: modprobe tuxedo_infinitybook_gen10_fan"

tuxedo-infinitybook-gen10-fan-dkms-uninstall:
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all 2>/dev/null
	rm -rf $(DKMS_SRC)
	@echo "DKMS module removed"

tuxedo-infinitybook-gen10-fan-dkms-status:
	dkms status $(DKMS_NAME)

# Full DKMS install (module + autoload + service)
tuxedo-infinitybook-gen10-fan-install-dkms: tuxedo-infinitybook-gen10-fan-dkms-install install-autoload install-service
	@echo ""
	@echo "=== Full DKMS Installation Complete ==="
	@echo "The module will auto-rebuild on kernel updates and fan control starts on boot."

tuxedo-infinitybook-gen10-fan-uninstall-dkms: uninstall-service uninstall-autoload tuxedo-infinitybook-gen10-fan-dkms-uninstall
	@echo ""
	@echo "=== Full DKMS Uninstallation Complete ==="
