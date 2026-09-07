# lg-magic.spec - Fedora package: DKMS kernel module + daemon + tools.
#
# DKMS on Fedora requires kernel-devel matching the running kernel on the
# target machine (noted in README.md and the setup wizard). The daemon is
# not enabled on install - the wizard does `systemctl enable --now`.

Name:           lg-magic
Version:        2.0.1
Release:        1%{?dist}
Summary:        LG Magic Remote MR20 driver, daemon and tools
License:        GPL-2.0-or-later
URL:            https://github.com/sirfragles/lgmagic
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  systemd-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig

Requires:       dkms
Requires:       polkit
Requires:       systemd
%{?systemd_requires}

%description
Linux kernel driver for the LG Magic Remote MR20 (Bluetooth 000f:3412):
a raw decoder of the remote's HID reports (buttons and wheel) plus a raw
IMU evdev device, with per-device calibration loaded from firmware blobs.

The userspace side is a system daemon (lg-magicd) that takes over the
input devices, applies profiles, calibration, button mapping and the
airmouse pipeline, and exposes one virtual mouse and keyboard; writes are
gated by polkit. The lg-magic CLI (libc/libm only) covers the IMU reader,
HID report analyzer, calibration utilities, configuration handling,
daemon control and the interactive setup wizard.

%prep
%setup -q

%build
# Only the tools here; the module itself is built by DKMS on the target
# machine (like the Debian package).
make tools

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix} UDEV_DIR=%{_udevrulesdir}
# The DKMS tree: dkms.conf + the module sources.
install -d %{buildroot}%{_prefix}/src/lg-magic-%{version}
cp -r kernel include %{buildroot}%{_prefix}/src/lg-magic-%{version}/
install -m 644 Makefile COPYING dkms.conf \
    %{buildroot}%{_prefix}/src/lg-magic-%{version}/

%post
# DKMS build+install; each step best-effort (missing kernel-devel or an
# unsigned Secure Boot module must never break the package install).
if [ -x /usr/sbin/dkms ]; then
    /usr/sbin/dkms add -m lg-magic -v %{version} || :
    /usr/sbin/dkms build -m lg-magic -v %{version} || :
    /usr/sbin/dkms install -m lg-magic -v %{version} || :
fi
%systemd_post lg-magicd.service
%tmpfiles_create lg-magic.conf
udevadm control --reload-rules || :
udevadm trigger || :

%preun
%systemd_preun lg-magicd.service
if [ "$1" -eq 0 ] && [ -x /usr/sbin/dkms ]; then
    /usr/sbin/dkms remove -m lg-magic -v %{version} --all || :
fi

%postun
%systemd_postun_with_restart lg-magicd.service

%files
%{_bindir}/lg-magic
%{_libexecdir}/lg-magicd
%{_unitdir}/lg-magicd.service
%{_tmpfilesdir}/lg-magic.conf
%{_datadir}/polkit-1/actions/org.lgmagic.policy
%{_datadir}/dbus-1/system.d/org.lgmagic.conf
%config(noreplace) /etc/lg-magic/config.toml
%{_udevrulesdir}/51-lgimu.rules
%{_prefix}/src/lg-magic-%{version}/kernel
%{_prefix}/src/lg-magic-%{version}/include
%{_prefix}/src/lg-magic-%{version}/Makefile
%{_prefix}/src/lg-magic-%{version}/COPYING
%{_prefix}/src/lg-magic-%{version}/dkms.conf

%changelog
* Mon Sep 07 2026 Ilya Chelyadin <sirfragles@users.noreply.github.com> - 2.0.1-1
- Per-remote virtual keyboard/mouse pairs named after the device
  identity; held keys are released on a live remap (no stuck keys);
  single calibration source per mode (daemon: /var/lib/lg-magic, no
  firmware blob); device identity validation on the bus; ApiVersion
  property; O_EXCL state writes; IMU udev rule restricted to the LG
  vendor/product.

* Sun Sep 06 2026 Ilya Chelyadin <sirfragles@users.noreply.github.com> - 2.0-1
- v2.0: system daemon (lg-magicd) with full input takeover, sd-bus
  interface and polkit-gated writes; raw_only=1 kernel default;
  JSON->TOML configuration; device/profile/button/scroll/diagnose CLI.
