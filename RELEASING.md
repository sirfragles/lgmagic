# Releasing lgmagic

One source of truth: the GitHub repository variable **VERSION**
(`gh variable get VERSION --repo sirfragles/lgmagic`). CI (the
`drift-gate` job) and the release pipeline (the `gate` job) both compare
the tree against it, so a release is a small, deterministic sequence:

**variable leads -> bump commit -> PR -> merge -> tag -> automatic release -> verify**

Never move a tag. A wrong tag is deleted and re-created; a wrong release
is deleted and re-run (rollback below).

## 1. Set the VERSION variable (leads)

```sh
gh variable set VERSION --repo sirfragles/lgmagic --body "X.Y.Z"
```

Do this FIRST and only when no code PR is in flight: between this step
and the merge in step 4 the `drift-gate` job on master is red (the tree
still carries the old version) - keep that window to minutes. A bump
commit is the only change that can turn it green again, which is exactly
the check the bump PR below must pass.

## 2. Stamp and write the changelog locally

```sh
cd <your-clone-of-lgmagic>
bash scripts/sync-version.sh X.Y.Z
```

`sync-version.sh` rewrites only version tokens - `dkms.conf`,
`tools/include/build_version.h`, `tools/include/daemon_bus.h`
(`LG_API_VERSION` = MAJ.MIN; bump it deliberately only when the D-Bus
API changes), the `debian/changelog` top line, `rpm/lgmagic.spec`
(`Version:`) and `arch/PKGBUILD` (`pkgver`). The changelog *entries*
are yours to write by hand:

- `debian/changelog` - a new top entry (the sync step already set its
  version line), date in RFC 5322 format as in the file.
- `rpm/lgmagic.spec` under `%changelog` - one line per release:
  `* Mon Sep 07 2026 Ilya Chelyadin <sirfragles@users.noreply.github.com> - X.Y.Z-1`
  (the weekday must be the real weekday of that date or rpmbuild
  refuses the spec).

Sanity-check before committing:

```sh
bash scripts/sync-version.sh --check   # exit 0 = all stamp sites agree
git diff --stat                        # exactly the stamp files + changelog entries
```

## 3. One atomic commit on a bump branch

```sh
git checkout -b chore/bump-to-X.Y.Z
git add -A
git commit -m "chore: bump to vX.Y.Z"
git push -u origin chore/bump-to-X.Y.Z
gh pr create --fill
```

`drift-gate` is the defining check on this PR: it stamps from the
VERSION variable and requires a clean diff, so green = the branch
provably carries the variable. Do not mix code changes into the bump
commit - a version bump is the one change that should never be rolled
back or reverted piecemeal.

## 4. Squash-merge and watch master

```sh
gh pr merge --squash
```

Post-merge CI on master must be green, including `drift-gate` (the red
window from step 1 closes here) and the `dkms-matrix` container legs.

## 5. Tag (immutable) and push

```sh
git tag -a vX.Y.Z -m "lgmagic vX.Y.Z"
git push origin vX.Y.Z
```

The tag is created once and never moved. If anything about the tag is
wrong after the push: delete it and re-create it locally and on the
remote (`git push origin :vX.Y.Z`) BEFORE any release artifact exists on
it - and fix the underlying cause (the variable, the tree) first. The
old project's habit of re-tagging in place is gone: `release.yml`'s
`gate` job compares the tag to the variable, but the *tree* at the tag
is whatever commit it points at.

## 6. The release pipeline

Tag push fires `release.yml` only (ci.yml no longer runs on tags):

```
gate -> deb | rpm | arch -> release -> verify
```

- `gate` - the tag must be exactly `v<VERSION>` and the variable
  non-empty; loud failure otherwise.
- `deb`, `rpm`, `arch` - each stamps from the variable (no-op on a
  correct tree), builds one package and uploads it as an artifact.
- `release` - downloads the three artifacts and attaches them to the
  Release. `fail_on_unmatched_files: true`: nothing silently attaches
  nothing, and the digit-after-dash globs keep
  `-debuginfo/-debugsource/-debug` packages off the Release.
- `verify` - reads the Release assets back and requires EXACTLY three,
  matching `^lgmagic-dkms_.*\.deb$`, `^lgmagic-[0-9].*\.rpm$` and
  `^lgmagic-[0-9].*\.pkg\.tar\.zst$`.

```sh
gh run watch   # or poll `gh run view` - watch's exit code is unreliable
```

## 7. Rollback

Two failure classes, two answers:

- **Bad or missing asset, correct tag** (e.g. `verify` failed on one of
  the three files): delete the Release and re-run it against the same,
  correct tag - the tag and its commit are fine:
  ```sh
  gh release delete vX.Y.Z --yes          # NO --cleanup-tag: the tag stays
  gh workflow run release.yml --ref vX.Y.Z
  ```
- **Code defect in the release** (the tag itself is bad): fix forward,
  bump a patch (`X.Y.Z+1`), run steps 1-6 again. Never patch a released
  tag in place - tags are immutable, and the variable/tree/commit are
  the only states that can be changed safely.

## Required CI checks

Branch protection on `master` should require, for every PR: `drift-gate`,
`linux`, `fedora`, `arch`, `macos`, `dkms-matrix`, `shellcheck`.

## Weekly drift detection

The `schedule` trigger in ci.yml (Monday 23:00 UTC) re-runs the full
pipeline on master. It exists so that a variable bump that never got its
commit - or a version that stopped matching after some other change -
fails loudly instead of drifting silently until the next release.
