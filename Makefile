obj-m += uniwill_ibg10_fanctl.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr/local

# DKMS variables
DKMS_NAME := uniwill-ibg10-fanctl
DKMS_VERSION := 0.2.1
DKMS_SRC := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all clean install uninstall install-service uninstall-service load unload daemon \
        uniwill-ibg10-fanctl-dkms-install uniwill-ibg10-fanctl-dkms-uninstall uniwill-ibg10-fanctl-dkms-status \
        uniwill-ibg10-fanctl-install-dkms uniwill-ibg10-fanctl-uninstall-dkms \
        tuxedo-infinitybook-gen10-fan-install-dkms tuxedo-infinitybook-gen10-fan-uninstall-dkms

all: modules daemon

daemon:
	$(MAKE) -C daemon

obj-m += uniwill_ibg10_fanctl.o

modules:
	make -C $(KDIR) M=$(CURDIR) modules

clean:
	make -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C daemon clean

# Module installation (run 'make' first as normal user)
install:
	install -d /lib/modules/$(KVER)/extra/
	install -m 644 uniwill_ibg10_fanctl.ko /lib/modules/$(KVER)/extra/
	depmod -a
	@echo "Module installed. Load with: modprobe uniwill_ibg10_fanctl"

uninstall:
	rm -f /lib/modules/$(KVER)/extra/uniwill_ibg10_fanctl.ko
	depmod -a
	@echo "Module uninstalled"

# Service installation (run 'make' first as normal user)
install-service: daemon
	install -m 755 daemon/uniwill_ibg10_fanctl $(PREFIX)/bin/uniwill_ibg10_fanctl
	install -m 644 uniwill-ibg10-fanctl.service /etc/systemd/system/
	sed -i 's|ExecStart=.*|ExecStart=$(PREFIX)/bin/uniwill_ibg10_fanctl|' /etc/systemd/system/uniwill-ibg10-fanctl.service
	systemctl daemon-reload
	systemctl enable --now uniwill-ibg10-fanctl.service
	@echo ""
	@echo "Service installed, enabled, and started"

uninstall-service:
	-systemctl stop uniwill-ibg10-fanctl.service 2>/dev/null
	-systemctl disable uniwill-ibg10-fanctl.service 2>/dev/null
	rm -f /etc/systemd/system/uniwill-ibg10-fanctl.service
	rm -f $(PREFIX)/bin/uniwill_ibg10_fanctl
	systemctl daemon-reload
	@echo "Service uninstalled"

# Module loading (for testing)
load: all
	-rmmod uniwill_ibg10_fanctl 2>/dev/null
	insmod ./uniwill_ibg10_fanctl.ko
	@echo "Module loaded"

unload:
	-rmmod uniwill_ibg10_fanctl 2>/dev/null
	@echo "Module unloaded"

# Auto-load on boot
install-autoload:
	echo "uniwill_ibg10_fanctl" > /etc/modules-load.d/uniwill-ibg10-fanctl.conf
	@echo "Module will load on boot"

uninstall-autoload:
	rm -f /etc/modules-load.d/uniwill-ibg10-fanctl.conf
	@echo "Auto-load disabled"

# Full install (module + autoload + service)
install-all: install install-autoload install-service
	@echo ""
	@echo "=== Installation Complete ==="
	@echo "The module will load on boot."
	@echo "Fan control is now running and will start automatically on boot."

uninstall-all: uninstall-service uninstall-autoload uninstall
	@echo ""
	@echo "=== Uninstallation Complete ==="

# DKMS installation (auto-rebuilds on kernel updates)
uniwill-ibg10-fanctl-dkms-install: daemon
	@echo "Installing DKMS module source..."
	install -d $(DKMS_SRC)
	install -m 644 uniwill_ibg10_fanctl.c $(DKMS_SRC)/
	install -m 644 dkms.conf $(DKMS_SRC)/
	@echo "obj-m += uniwill_ibg10_fanctl.o" > $(DKMS_SRC)/Makefile
	dkms add -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms build -m $(DKMS_NAME) -v $(DKMS_VERSION)
	dkms install -m $(DKMS_NAME) -v $(DKMS_VERSION) --force
	@echo ""
	@echo "=== DKMS Installation Complete ==="
	@echo "Module will auto-rebuild on kernel updates."
	@echo "Load with: modprobe uniwill_ibg10_fanctl"

uniwill-ibg10-fanctl-dkms-uninstall:
	-dkms remove -m $(DKMS_NAME) -v $(DKMS_VERSION) --all 2>/dev/null
	rm -rf $(DKMS_SRC)
	@echo "DKMS module removed"

uniwill-ibg10-fanctl-dkms-status:
	dkms status $(DKMS_NAME)

# Full DKMS install (module + autoload + service)
uniwill-ibg10-fanctl-install-dkms: uniwill-ibg10-fanctl-dkms-install install-autoload install-service
	@echo ""
	@echo "=== Full DKMS Installation Complete ==="
	@echo "The module will auto-rebuild on kernel updates."
	@echo "Fan control is now running and will start automatically on boot."

# Backward-compatible aliases (old target names)
# (retain for users of old docs)
tuxedo-infinitybook-gen10-fan-install-dkms: uniwill-ibg10-fanctl-install-dkms
	@echo "(alias) DKMS install via new target"

uniwill-ibg10-fanctl-uninstall-dkms: uninstall-service uninstall-autoload uniwill-ibg10-fanctl-dkms-uninstall
	@echo ""
	@echo "=== Full DKMS Uninstallation Complete ==="

tuxedo-infinitybook-gen10-fan-uninstall-dkms: uniwill-ibg10-fanctl-uninstall-dkms
	@echo "(alias) DKMS uninstall via new target"


