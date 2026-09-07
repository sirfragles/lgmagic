# lgmagic.spec - Fedora package: DKMS kernel module + daemon + tools.
#
# DKMS on Fedora requires kernel-devel matching the running kernel on the
# target machine (noted in README.md and the setup wizard). The daemon is
# not enabled on install - the wizard does `systemctl enable --now`.

Name:           lgmagic
Version:        0.0.1
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

The userspace side is a system daemon (lgmagicd) that takes over the
input devices, applies profiles, calibration, button mapping and the
airmouse pipeline, and exposes one virtual mouse and keyboard; writes are
gated by polkit. The lgmagic CLI (libc/libm only) covers the IMU reader,
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
install -d %{buildroot}%{_prefix}/src/lgmagic-%{version}
cp -r kernel include %{buildroot}%{_prefix}/src/lgmagic-%{version}/
install -m 644 Makefile COPYING dkms.conf \
    %{buildroot}%{_prefix}/src/lgmagic-%{version}/

%post
# DKMS build+install; each step best-effort (missing kernel-devel or an
# unsigned Secure Boot module must never break the package install).
if [ -x /usr/sbin/dkms ]; then
    /usr/sbin/dkms add -m lgmagic -v %{version} || :
    /usr/sbin/dkms build -m lgmagic -v %{version} || :
    /usr/sbin/dkms install -m lgmagic -v %{version} || :
fi
%systemd_post lgmagicd.service
%tmpfiles_create lgmagic.conf
udevadm control --reload-rules || :
udevadm trigger || :

%preun
%systemd_preun lgmagicd.service
if [ "$1" -eq 0 ] && [ -x /usr/sbin/dkms ]; then
    /usr/sbin/dkms remove -m lgmagic -v %{version} --all || :
fi

%postun
%systemd_postun_with_restart lgmagicd.service

%files
%{_bindir}/lgmagic
%{_libexecdir}/lgmagicd
%{_unitdir}/lgmagicd.service
%{_tmpfilesdir}/lgmagic.conf
%{_datadir}/polkit-1/actions/org.lgmagic.policy
%{_datadir}/dbus-1/system.d/org.lgmagic.conf
%config(noreplace) /etc/lgmagic/config.toml
%{_udevrulesdir}/51-lgimu.rules
%{_prefix}/src/lgmagic-%{version}/kernel
%{_prefix}/src/lgmagic-%{version}/include
%{_prefix}/src/lgmagic-%{version}/Makefile
%{_prefix}/src/lgmagic-%{version}/COPYING
%{_prefix}/src/lgmagic-%{version}/dkms.conf

%changelog
* Mon Sep 07 2026 Ilya Chelyadin <sirfragles@users.noreply.github.com> - 0.0.1-1
- Fresh start: renamed to lgmagic; version line restarts at 0.0.1;
- VERSION repository variable is the single source of truth.
