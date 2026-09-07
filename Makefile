# Top-level Makefile for lg-magic (kernel module + userspace tools).
#
# Targets:
#   all             - build the kernel module and the userspace tools (default)
#   modules         - build only the kernel module (used by DKMS)
#   tools           - build only the userspace tools (lg-magic + on Linux
#                     lg-magicd and the fake-device e2e harness)
#   check           - build the tools and run the test suite
#   install         - install lg-magic, lg-magicd (Linux), the systemd unit,
#                     tmpfiles, polkit policy, D-Bus config, config.toml and
#                     the udev rule
#   install-firmware- install a calibration blob into /lib/firmware (opt-in)
#   uninstall       - remove what install put in place (never the firmware)
#   clean
#
# dkms.conf at the repo root drives DKMS (PACKAGE_VERSION must stay in sync
# with debian/changelog and the spec/PKGBUILD pkgver). DKMS builds the module
# only; the tools are built by the packages, not by DKMS.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PREFIX ?= /usr
DESTDIR ?=

# Systemd directories (pkg-config with a fallback; `!=` works in BSD make
# and GNU make 4+). The `grep .` matters: pkgconf --variable exits 0 with
# EMPTY output when the .pc lacks the variable (ALARM's systemd.pc), and
# `|| echo` alone only fires on a nonzero exit - the fallback would never
# run and files would land in $(DESTDIR) itself.
UNIT_DIR     != pkg-config --variable=systemdsystemunitdir systemd 2>/dev/null | grep . || echo /usr/lib/systemd/system
TMPFILES_DIR != pkg-config --variable=systemdtmpfilesdir systemd 2>/dev/null | grep . || echo /usr/lib/tmpfiles.d
# The daemon lives in /usr/libexec (Arch patches the unit via the .install
# file instead and keeps /usr/lib).
LIBEXECDIR ?= $(PREFIX)/libexec
# Debian puts the admin rule in /etc/udev/rules.d (v1 behaviour); Fedora
# and Arch pass their package rule dir explicitly.
UDEV_DIR ?= /etc/udev/rules.d

# Calibration blob for the install-firmware target.
CALIB ?= lg_magic_calib.bin

all: modules tools

modules:
	$(MAKE) -C kernel KDIR=$(KDIR)

tools:
	$(MAKE) -C tools

check: tools
	$(MAKE) -C tools check

install: tools
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(LIBEXECDIR) \
		$(DESTDIR)$(UDEV_DIR) $(DESTDIR)/etc/lg-magic \
		$(DESTDIR)$(UNIT_DIR) $(DESTDIR)$(TMPFILES_DIR) \
		$(DESTDIR)/usr/share/polkit-1/actions \
		$(DESTDIR)/usr/share/dbus-1/system.d
	install -m 0755 tools/lg-magic $(DESTDIR)$(PREFIX)/bin/
	@if [ -x tools/lg-magicd ]; then \
		install -m 0755 tools/lg-magicd $(DESTDIR)$(LIBEXECDIR)/; \
	fi
	install -m 0644 51-lgimu.rules $(DESTDIR)$(UDEV_DIR)/
	install -m 0644 data/lg-magicd.service $(DESTDIR)$(UNIT_DIR)/
	install -m 0644 data/lg-magic.tmpfiles $(DESTDIR)$(TMPFILES_DIR)/lg-magic.conf
	install -m 0644 data/org.lgmagic.policy $(DESTDIR)/usr/share/polkit-1/actions/
	install -m 0644 data/org.lgmagic.conf $(DESTDIR)/usr/share/dbus-1/system.d/
	install -m 0644 data/config.toml $(DESTDIR)/etc/lg-magic/

# Deliberately opt-in: a zeroed blob would pass the kernel's validation and
# silently disable the airmouse, so firmware is never installed by default.
install-firmware:
	install -d $(DESTDIR)/lib/firmware
	install -m 0644 $(CALIB) $(DESTDIR)/lib/firmware/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lg-magic
	rm -f $(DESTDIR)$(LIBEXECDIR)/lg-magicd
	rm -f $(DESTDIR)$(UDEV_DIR)/51-lgimu.rules
	rm -f $(DESTDIR)$(UNIT_DIR)/lg-magicd.service
	rm -f $(DESTDIR)$(TMPFILES_DIR)/lg-magic.conf
	rm -f $(DESTDIR)/usr/share/polkit-1/actions/org.lgmagic.policy
	rm -f $(DESTDIR)/usr/share/dbus-1/system.d/org.lgmagic.conf
	rm -f $(DESTDIR)/etc/lg-magic/config.toml

clean:
	# Kernel clean needs KDIR; without kernel headers (containers, CI
	# builders that never build the module) it must not fail the build.
	-$(MAKE) -C kernel clean || true
	-$(MAKE) -C tools clean || true

.PHONY: all modules tools check install install-firmware uninstall clean
