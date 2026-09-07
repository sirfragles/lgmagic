# Testing lgmagic

How the LG Magic Remote driver, the daemon and the userspace tools are
tested.

There are five layers:

1. **Portable unit tests** - the fourteen numeric/parse/IO modules under
   `tools/`. They compile and run on any host with a C99 compiler (macOS
   included) via `make -C tools check`.
2. **CLI smoke tests** - `tools/tests/cli_smoke.sh`, run automatically by
   `make -C tools check` once the Linux-only `lgmagic` binary has been
   built.
3. **Reference-data parity checks** - the C tools re-run the reference
   Python pipeline (`scripts/`) over the committed fixtures in `testdata/`
   and must agree with the committed reference outputs.
4. **Fake-device end-to-end tests** - a uinput harness
   (`tools/tests/fake_devices.c`) that plays the remote without hardware;
   `tools/tests/daemon_e2e.sh` drives the whole daemon surface (bus,
   polkit, mapping, profiles, calibration, scroll, airmouse, reconnect,
   grab release). Needs root and `/dev/uinput`; runs in CI.
5. **Packaging / hardware checks** - .deb/.rpm/Arch package gates in
   containers, and finally the remote itself.

Fixtures are committed on purpose: **a missing fixture is a test FAILURE,
never a reason to embed invented golden data in a test.**

---

## 1. Portable unit tests

```sh
cd tools
make check          # build and run all 14 test binaries
```

Each test binary prints one `PASS`/`FAIL` line per check, a
`test_xxx: N passed, M failed` summary, and exits nonzero if anything
failed. `make check` fails if any binary fails.

Current state (all fourteen binaries green, 475 checks):

| test binary            | module(s) under test                     | checks |
|------------------------|------------------------------------------|-------:|
| `tests/test_json`      | `src/json.c`                             |     59 |
| `tests/test_matrix`    | `src/matrix.c`                           |     36 |
| `tests/test_csv`       | `src/csv.c`                              |     10 |
| `tests/test_madgwick`  | `src/madgwick.c`                         |      2 |
| `tests/test_lm`        | `src/lm.c`                               |      9 |
| `tests/test_calib_blob`| `src/calib.c` + `include/lgmagic_calib.h`|    27 |
| `tests/test_config`    | `src/config.c` (TOML)                    |     33 |
| `tests/test_cube_math` | `src/cube.c`                             |     19 |
| `tests/test_toml`      | `src/toml.c`                             |     50 |
| `tests/test_keymap`    | `src/keymap.c`                           |     56 |
| `tests/test_airmouse`  | `src/airmouse.c` (v1 parity)             |     18 |
| `tests/test_profiles`  | `src/profiles.c`                         |     65 |
| `tests/test_pairing`   | `src/pairing.c`                          |     30 |
| `tests/test_dbus_client`| `src/dbus_client.c`                     |     61 |

Notes and conventions:

- Every test compiles against **all** portable sources
  (`src/{matrix,json,csv,calib,madgwick,lm,cube,config,toml,keymap,
  airmouse,profiles,pairing,dbus_client}.c`), so an API change in one
  module breaks every test that uses it - by design.
- Tests run with the working directory `tools/`; fixtures are found at
  `../testdata/` (the harness `tu_fixture_path()` also tries `testdata/`).
  Helper code is shared in `tests/test_util.h`.
- Tests are hermetic and deterministic: fixed seeds (no
  `rand()`/time-based data), and temporary files only under
  `${TMPDIR:-/tmp}` via `tu_temp_path()`, removed on exit.
- `test_config` points `HOME` at a scratch directory so the real user
  config is never touched. One assumption cannot be neutralised: the
  system-wide `/etc/lgmagic/config.toml` is read by `config_load()`, so
  the test suite assumes **no `/etc/lgmagic/config.toml` exists on the
  host**.
- `test_madgwick` replays `../testdata/madgwick_ref.csv` (500 rows, 50 Hz
  recordings) through `madgwick_init()`/`madgwick_update_imu()` and
  compares every quaternion component against the reference within
  `1e-6`, allowing for a sign flip (q and -q are the same rotation).
- `test_lm` builds its own synthetic ellipsoid data with a known bias, so
  bias recovery is checkable in absolute terms; the matrix M is compared
  only through the gauge-invariant product M^T M (see section 3 for why
  raw parameters are never comparable).
- `test_calib_blob` compile-time checks the blob layout
  (`_Static_assert`, 32 bytes, offsets 0/12/24/28) and byte-compares
  `calib_to_blob()` output against the golden `ref_calib.bin`.
- `test_airmouse` asserts the v2 `airmouse.c` pipeline reproduces the v1
  `imu --mouse` output **bit for bit** (LPF + scale, `int` truncation,
  signs) on synthetic gyro frames - the v1-parity contract.
- `test_dbus_client` marshals/unmarshals against golden bytes, exercises
  the AUTH EXTERNAL handshake and full round-trips against a fake bus on
  a unix socket, including chunked replies, oversized replies, rejected
  auth and connection errors. The client is what `lgmagic` uses to talk
  to `lgmagicd`, so it must work without linking libsystemd (asserted by
  the CI `ldd` step).

Adding a test: a new `tests/test_x.c` needs one explicit rule in
`tools/Makefile` (pattern rules are avoided for BSD make).

## 2. CLI smoke tests

`make -C tools check` runs `sh tests/cli_smoke.sh ./lgmagic` only when the
binary exists. The binary is Linux-only (evdev/hidraw/uinput); build it
with:

```sh
cd tools
make lgmagic      # needs a Linux kernel build tree environment
```

The smoke script then covers:

- top-level handling in `src/main.c`: `--help`/`-h` (usage on stdout,
  exit 0), no arguments (usage on stderr, exit 1), `--version`
  (`lgmagic 0.0.1`), unknown subcommand, `--config FILE` argument handling
  and the global-flag-in-any-position rule;
- the documented contract that every subcommand (`analyze`, `imu`,
  `calibrate`, `calib2bin`, `config`, `setup`, `device`, `profile`,
  `button`, `scroll`, `diagnose`) prints its own usage on `--help` and
  exits 0;
- the `config` subcommand end to end **against a scratch `HOME`** (it
  must never touch the real user config): defaults, `path`, `set` with
  save + reload round trip, error paths (unknown key, non-numeric value,
  missing value, unknown argument, extra arguments), and `migrate`
  converting a v1 JSON fixture to TOML;
- the `--config FILE` merge (extra file overrides the user file) with the
  flag in both positions, and a missing `--config` file being skipped
  rather than fatal. v2 config files are TOML; `--config` with a v1 JSON
  file is rejected with a pointer to `lgmagic config migrate`.

On hosts where the binary does not exist (macOS, or Linux before the
userspace build is complete) the script prints `SKIP` and exits 0 so
`make check` still passes.

`make -C tools check` also runs two POSIX-sh repo guards on every host
(macOS included - neither needs a Linux binary), and a failure of
either fails the check:

- `tests/check_naming.sh` - zero tolerance for the pre-rename token
  spellings of the project name anywhere in the repo tree (excluding
  `.git/`, `testdata/` and `receipts/`) and in tracked filenames. There
  is **no allowlist**; every hit is printed as `path:linenr: text` and
  the guard exits 1.
- `tests/check_version_sync.sh` - asserts `scripts/sync-version.sh
  --check` exits 0, re-verifies the five version stamps plus
  `LG_API_VERSION` with an **independent** parser, re-runs the sync's
  idempotency and drift-heal behaviour hermetically in a temp copy of
  the stamped files (the real tree is never written to), and
  cross-checks `lgmagic --version` against the dkms.conf version when a
  Linux binary is present.

## 3. Reference-data parity checks (no hardware needed)

`testdata/` contains outputs of the reference Python implementation
(`scripts/`), produced once and committed. The C tools must reproduce
them:

| fixture            | produced by            | purpose |
|--------------------|------------------------|---------|
| `madgwick_ref.csv` | Madgwick replay at 50 Hz (ahrs `updateIMU`, beta 0.1), seeded with `accel_to_quat()` of the first row | replay parity of `madgwick.c`, covered by `test_madgwick` (1e-6) |
| `ref_accel.json`   | `scripts/calibrate.py` (scipy) on `sample_imu.csv` | accel calibration reference |
| `ref_gyro.json`    | gyro-only calibration (empty accel arrays) | gyro bias reference; also the empty-array robustness case in `test_calib_blob` |
| `ref_calib.json`   | hand-built calibration | `calib_load()` values in `test_calib_blob` |
| `ref_calib.bin`    | `scripts/convert_calib.py` output | 32-byte LE float blob golden |
| `sample_imu.csv`   | raw IMU recording (`counter,dt,ax,ay,az,gx,gy,gz`, dt empty when unknown) | input for calibrate/parity runs |

Checks that run on a Linux host with the full binary:

1. **Gyro calibration parity (gauge-free).** Fit the gyro calibration
   from `sample_imu.csv` with the C `calibrate` command and compare the
   mean gyro bias to `ref_gyro.json` (`[1.5115, -2.199, 0.798]`). A
   mean/offset bias is invariant under the rotation gauge, so it must
   agree within `1e-6` in the same normalized units.

2. **Accel calibration parity - corrected norms, not parameters.**
   `ref_accel.json` holds the parameters of a scipy TRF fit, which is
   **only identifiable up to a left rotation**: `r(x) = ||M(a - b)|| -
   9.80665` is unchanged by `M' = R M`, and scipy and the C LM solver
   legitimately land on different rotated minima with identical cost.
   Raw parameter comparison is therefore meaningless. Check instead:
   run the C `calibrate` on `sample_imu.csv` and verify every corrected
   sample norm `||M(a_i - b)||` stays within `0.05 m/s^2` of `9.80665`
   (recording noise is ~15 counts ~ 0.015 m/s^2, so this is a wide
   margin). Equivalently, the final mean-squared cost of both solvers
   must be `~3e-4` (C: 0.000283777, scipy: 0.000298288 on this fixture).

3. **Blob byte parity.** Converting the C calibration JSON with
   `alpha = 0.2` and `mouse_k = 0.5` must produce a 32-byte blob
   byte-identical to `ref_calib.bin` (little-endian floats:
   `gyro_bias[3] @ 0`, `gyro_scale[3] @ 12`, `alpha @ 24`,
   `mouse_k @ 28`). Also covered at unit level by `test_calib_blob`
   (`memcmp` against the committed binary fixture), including the range
   checks the kernel applies on load (`|bias| <= 100`, `|scale| <= 10`,
   `alpha, mouse_k in [0, 1]`).

4. **Madgwick replay parity.** Replay `madgwick_ref.csv` through the C
   filter as described above - covered continuously by `test_madgwick`.
   Note the fixture convention: rows with exactly zero gyro (rows 0 and 3)
   are the ahrs "zero gyro" early-return rows, so they record the
   unchanged quaternion and every implementation agrees on them.

## 4. Fake-device end-to-end tests (Linux, no hardware)

`tools/tests/fake_devices.c` creates a uinput keyboard named
`LG Magic Remote` (vendor/product 0x000f:3412) and a uinput ABS device
named `LG Magic Remote IMU` - the exact devices the daemon pairs and
grabs. Each remote gets its **own** virtual pair named after its
identity (`lgmagicd keyboard <identity>` / `lgmagicd mouse
<identity>`; the fake remote has no BT MAC, so its identity is
`unknown`). Two drivers run the assertions:

- `tools/tests/daemon_e2e.sh` - the **full bus + polkit e2e**. Needs root,
  `/dev/uinput`, `dbus-daemon` and `polkitd`; starts a throwaway system
  bus, installs the policy files, runs `lgmagicd` and asserts:
  - `lgmagic device list` / `device status` render as root **and as
    `nobody`** - read-only methods stay polkit-free for everyone; the
    `ApiVersion` property reads `"0.0"` and a bogus identity is rejected
    with `org.lgmagic.Error.InvalidArguments`;
  - `button map` as root maps `KEY_UP -> KEY_VOLUMEUP` and the fake
    remote's KEY_UP then arrives at `lgmagicd keyboard unknown` as
    KEY_VOLUMEUP **immediately, without a daemon restart**; the same
    call as `nobody` is **denied** (polkit
    `org.lgmagic.modify-input`), and so is `profile set` as `nobody`
    (`org.lgmagic.profile-set`);
  - **held key across a remap**: KEY_UP is pressed (maps to
    KEY_VOLUMEUP), remapped to KEY_HOME while still held, then released -
    the daemon must release KEY_VOLUMEUP when the map changes (no stuck
    key) and the held press must not leak into the new map;
  - `profile set` updates `/var/lib/lgmagic/state.toml` and changes the
    active map/scroll through the daemon;
  - scroll: a wheel byte of +2 at `scroll_speed 2.0` yields REL_WHEEL 4
    (+480 on REL_WHEEL_HI_RES) on `lgmagicd mouse unknown`;
  - calibration: a calib JSON in /var/lib + `Reload()` takes effect
    (airmouse behaviour changes); a **broken calib JSON is rejected** -
    the daemon logs the failure, keeps the previous calibration and
    stays alive;
  - airmouse: nonzero gyro moves `lgmagicd mouse unknown` with the v1
    signs; at rest there is no motion;
  - reconnect: destroying and recreating the fake device is rediscovered;
  - grab release: `kill -9` on the daemon releases EVIOCGRAB - raw
    events flow to an ordinary reader again; the same check repeats after
    **SIGTERM and SIGINT** (the daemon is restarted between the checks);
  - standalone `lgmagic imu --csv --device <fake IMU>` streams in
    parallel (the IMU is never grabbed).
- `tools/tests/daemon_e2e_busless.sh` - the pipeline assertions without a
  bus (used on machines without polkitd).

Both scripts **SKIP (exit 0)** when a prerequisite is missing - the CI
runner must have uinput, which is the main risk (reported by the SKIP
line in the log).

## 5. Packaging gates (Linux containers)

Each distro package is built and sanity-checked in a clean container:

- **Debian/Ubuntu**: `dpkg-buildpackage -us -uc -b`, then `dpkg -i` in a
  fresh container. The postinst runs `dkms add/build/install` with every
  step **best-effort** (`|| true`): a machine without matching kernel
  headers (or with Secure Boot rejecting the unsigned module) must still
  get a clean install - `dkms status` then shows `lgmagic/0.0.1: added`.
  With headers present the module builds and `dkms status` shows
  `installed`. Verify installed files (`/usr/bin/lgmagic`,
  `/usr/libexec/lgmagicd`, the unit, policy, dbus conf, tmpfiles,
  `config.toml`, the udev rule, `/usr/src/lgmagic-0.0.1/dkms.conf`) and
  the `ldd` split: `lgmagic` without libsystemd, `lgmagicd` with it.
- **Fedora**: `rpmbuild -bb rpm/lgmagic.spec` in a fedora container
  (Source0 pre-seeded with a snapshot tarball named for the dkms.conf
  version - the tag tarball serves direct builds); `rpm -qlp` content
  checks. Same best-effort DKMS policy in
  `%post`. Note: DKMS on Fedora needs a matching `kernel-devel` on the
  target machine.
- **Arch**: `makepkg -sf --noconfirm --nodeps` in an
  `archlinux:base-devel` container **as a non-root builder** (dkms is
  AUR-only, hence `--nodeps`); `tar -tf` content checks plus the
  `ExecStart=/usr/lib/lgmagicd` assertion (Arch has no /usr/libexec).
  On arm64 hosts use the `lopsided/archlinux:devel` image and
  `DisableSandbox` in pacman.conf (pacman 7 Landlock vs the Apple
  container VM).

At 0.0.1 the three artifacts are `lgmagic-dkms_0.0.1-1_amd64.deb`,
`lgmagic-0.0.1-1.fc*.x86_64.rpm` and
`lgmagic-0.0.1-1-x86_64.pkg.tar.zst` (the DKMS source tree installs to
`/usr/src/lgmagic-0.0.1/` and `dkms status` reports `lgmagic/0.0.1`).

CI runs all three (see `.github/workflows/ci.yml`); the release workflow
attaches all three artifacts to the tag release.

## 6. Manual hardware checklist

With the LG Magic Remote (MR20) paired over Bluetooth. The acceptance
criteria split into two stages: **v1 - compatibility with the current
driver** (points 1-5) and **v2 - the daemon and target architecture**
(points 6-12). Point 8 (immediate map), the grab-release half of 10, the
read-only half of 11 and the invalid-calibration rejection are also
covered automatically by `daemon_e2e.sh` (section 4); the rest need the
real remote.

**Definition of done:** the remote works standalone without the daemon;
the daemon only *adds* airmouse, profiles, mapping, calibration and
diagnostics.

### Stage v1 - compatibility with the current driver

```sh
sudo lgmagic setup          # wizard: mode choice, configure, calibrate, install
```

1. **Setup and upgrade.** `setup` finds the remote and both input
   devices and offers the mode choice (daemon = v2 default `raw_only=1
   imu_evdev=1`, kernel airmouse = v1 behaviour `raw_only=0 airmouse=1`).
   The v1 flows survive either mode. **One calibration source per mode:**
   in daemon mode the wizard writes only
   `/var/lib/lgmagic/<MAC>/calibration.json` (no firmware blob, no
   `/etc/lgmagic/calib.json`); in kernel airmouse mode it writes the
   blob to `/lib/firmware/` as in v1. No calibration data may land in
   two places.
2. **Buttons decode identically to v1.** Every physical key arrives
   with the same keycodes (the static `lg_btn_map` decode is
   mode-independent) - check with `evtest` or `lgmagic analyze`.
3. **Wheel.** In daemon mode scroll arrives as `REL_WHEEL` from the
   kernel device; with `raw_only=0` the v1 behaviour (key emulation /
   BTN_LEFT in airmouse mode) is unchanged.
4. **IMU + v1 tools.** The IMU evdev device is present; `analyze`,
   `imu --csv`, `calibrate` and `calib2bin` work as in v1 (blob
   byte-compatible, airmouse output parity).
5. **Decoder parity on a saved trace.** Record **one** HID trace from
   the remote once, e.g.
   `sudo cat /dev/hidrawN > /tmp/trace.bin` while pressing a fixed
   sequence of buttons and wheel notches. Replay the **same file**
   through the v1 decode (module loaded with `raw_only=0`) and the v2
   decode (`raw_only=1`) - e.g. with a uhid replayer feeding the
   recorded reports - and diff the evdev outputs: keys and wheel must
   match exactly; only airmouse motion may differ (v1 emits REL_X/REL_Y,
   raw_only does not). Never compare two parallel live reads - both
   decoders must see the identical bytes.

### Stage v2 - the daemon and target architecture

6. **Takeover order.** The wizard enables `lgmagicd`; the daemon
   creates the uinput mouse and keyboard **before** taking EVIOCGRAB.
   Restarting the daemon must never leave the remote dead. Each remote
   owns one pair named after its identity (`lgmagicd keyboard <MAC>` /
   `lgmagicd mouse <MAC>`) - two remotes must never share or mix
   devices.
7. **Airmouse through the daemon.** The pointer tracks hand motion with
   the v1 signs/scale; gyro rest drift is small; buttons keep clicking.
8. **Immediate profile / map.** `profile set` and `button map` change
   behaviour at once - **without restarting the daemon**. Hold a key
   and remap it mid-press: the old virtual key must be released the
   moment the map changes (no stuck key), and the held press must not
   appear under the new mapping.
9. **Scroll speed / sensitivity.** `scroll speed` and `sensitivity`
   changes are applied immediately too.
10. **Fallback without the daemon (the key safety condition).** With
    `raw_only=1` and the daemon dead - `systemctl stop`, SIGINT, SIGTERM
    or a crash - the remote keeps working from the kernel: buttons and
    wheel flow straight from the kernel evdev device, only the airmouse
    rests until the daemon returns. EVIOCGRAB must be released on every
    exit path.
11. **polkit model.** Read-only methods (`device list`/`status`,
    `profile list`) work without any authorization for any user. From an
    active desktop session `lgmagic profile set <MAC> tv` succeeds
    **without** a password prompt (`org.lgmagic.profile-set`,
    `allow_active=yes`), while `lgmagic button map …` may ask
    (`org.lgmagic.modify-input`, `auth_admin_keep`). From a
    non-graphical session both are denied. (Cannot be automated in CI -
    no login session there; the root/nobody halves are in the e2e.)
12. **Diagnostics.** `device list`/`status` render without sudo;
    `lgmagic analyze` still decodes reports in parallel with the daemon
    (nothing is consumed); `lgmagic diagnose` collects version, uname,
    dmesg, module parameters, devices, config and daemon status into a
    report for issues.
