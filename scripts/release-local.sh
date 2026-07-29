#!/bin/sh
# Build, package, and publish a GitHub release entirely from THIS machine —
# the local twin of .github/workflows/release.yml, for setups where the boot
# disks can't (or shouldn't) be hosted anywhere a CI runner could fetch them:
# both native builds run in the IRIS emulator here, then `gh release create`
# uploads exactly the artifact set the Actions pipeline would have.
#
# What it runs, in order — every step is the SAME script the GitHub Actions
# workflow runs, so the two paths cannot drift:
#   1. preflight    git tree clean (release maps to a commit), HEAD pushed,
#                   gh authenticated; fetch-image.sh --check-only per flavor
#   2. o32 build    fetch-image.sh + iris-build.sh --flavor o32  (5.3 guest)
#   3. n32 build    fetch-image.sh + iris-build.sh --flavor n32  (6.5 guest)
#   4. package      scripts/package-dist.sh -> .iso / .hda / .tar.gz
#   5. release      scripts/publish-release.sh (gh release create)
#
# Boot disks come from ci/local.conf, $IRIX53_IMAGE/$IRIX65_IMAGE, or
# even a $IRIX53_DISK_URL/$IRIX65_DISK_URL download — the same resolution the
# workflow uses (scripts/fetch-image.sh). rb-cli is auto-provided by
# scripts/ensure-rbcli.sh if not installed.
#
# Usage:
#   scripts/release-local.sh [--version V] [--draft] [--dry-run]
#                            [--skip-o32] [--skip-n32] [--allow-dirty]
#                            [--outdir DIR] [--iris-dir DIR] [--rb-cli PATH]
#
#   --version V    release version [UTC date stamp, e.g. 2026-07-28-15-04]
#   --draft        create the GitHub release as a draft
#   --dry-run      build + package, then PRINT the gh command instead of
#                  publishing (also skips the git-pushed preflight)
#   --skip-o32     skip the 5.3/o32 build (the images then carry n32 — note
#                  that binary is 6.x-only)
#   --skip-n32     release with the o32 pair only (no 6.5 image available);
#                  the .iso/.hda always carry o32 anyway
#   --skip-inst    skip the Software Manager distribution (iris-gendist.sh);
#                  it is also skipped automatically when either flavor is,
#                  since the product wants both subsystems (BUILD_INST=0 in
#                  ci/local.conf disables it durably)
#   --allow-dirty  permit uncommitted changes (binaries stamp <rev>-dirty)
#
# A flavor can also be switched off durably with BUILD_O32=0 / BUILD_N32=0 in
# ci/local.conf (or the environment) — same knobs the workflow exposes as the
# build_o32/build_n32 dispatch inputs and BUILD_O32/BUILD_N32 repo variables.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"
VERSION=""
DRAFT=0
DRYRUN=0
SKIP_O32=0
SKIP_N32=0
SKIP_INST=0
ALLOW_DIRTY=0
OUTDIR=""
IRIS_DIR_ARG=""
RB="${RB_CLI:-}"               # empty = let ensure-rbcli.sh find/fetch one

die() { echo "release-local: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version)     VERSION="$2"; shift 2 ;;
		--draft)       DRAFT=1; shift ;;
		--dry-run)     DRYRUN=1; shift ;;
		--skip-o32)    SKIP_O32=1; shift ;;
		--skip-n32)    SKIP_N32=1; shift ;;
		--skip-inst)   SKIP_INST=1; shift ;;
		--allow-dirty) ALLOW_DIRTY=1; shift ;;
		--outdir)      OUTDIR="$2"; shift 2 ;;
		--iris-dir)    IRIS_DIR_ARG="$2"; shift 2 ;;
		--rb-cli)      RB="$2"; shift 2 ;;
		-h|--help)     sed -n '2,38p' "$0"; exit 0 ;;
		*)             die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf fills in whatever flags/env didn't set

# Which flavors run: --skip-* flags > BUILD_O32/BUILD_N32 (env or conf).
DO_O32=1; DO_N32=1
[ "$SKIP_O32" = 1 ] && DO_O32=0
[ "$SKIP_N32" = 1 ] && DO_N32=0
if [ "$DO_O32" = 1 ] && ! flavor_enabled o32; then
	echo "release-local: o32 disabled (BUILD_O32 is off)"; DO_O32=0
fi
if [ "$DO_N32" = 1 ] && ! flavor_enabled n32; then
	echo "release-local: n32 disabled (BUILD_N32 is off)"; DO_N32=0
fi
[ "$DO_O32" = 1 ] || [ "$DO_N32" = 1 ] || die "nothing to build — both flavors are disabled"

# The inst distribution wants both subsystems' binaries.
DO_INST=1
[ "$SKIP_INST" = 1 ] && DO_INST=0
case "${BUILD_INST:-1}" in 0|[Nn][Oo]|[Ff][Aa][Ll][Ss][Ee]|[Oo][Ff][Ff]) DO_INST=0 ;; esac
if [ "$DO_INST" = 1 ] && { [ "$DO_O32" = 0 ] || [ "$DO_N32" = 0 ]; }; then
	echo "release-local: inst distribution skipped (needs both flavors built)"
	DO_INST=0
fi

[ -n "$VERSION" ] || VERSION=$(date -u +%Y-%m-%d-%H-%M)
[ -n "$OUTDIR" ] || OUTDIR="$REPO/dist/release-$VERSION"
TAG="v$VERSION"

# ---- 1. preflight ------------------------------------------------------------
echo "==> [1/6] preflight"
command -v gh >/dev/null 2>&1 || die "gh not found — needed to create the release"
# Same provisioning path as the Actions jobs: explicit choice > PATH >
# release download (ensure-rbcli.sh).
if [ -n "$RB" ]; then RB_CLI="$RB"; export RB_CLI; fi
RB=$("$REPO/scripts/ensure-rbcli.sh")
[ "$DO_O32" = 0 ] || "$REPO/scripts/fetch-image.sh" --flavor o32 --check-only
[ "$DO_N32" = 0 ] || "$REPO/scripts/fetch-image.sh" --flavor n32 --check-only

cd "$REPO"
if [ "$ALLOW_DIRTY" = 0 ] && [ -n "$(git status --porcelain)" ]; then
	die "working tree is dirty — commit first so the release maps to a real
  revision (binaries would stamp <rev>-dirty), or pass --allow-dirty"
fi

if [ "$DRYRUN" = 0 ]; then
	gh auth status >/dev/null 2>&1 || die "gh is not authenticated (gh auth login)"
	# The tag must point at a commit the remote actually has.
	git fetch -q origin
	HEAD_SHA=$(git rev-parse HEAD)
	git branch -r --contains "$HEAD_SHA" 2>/dev/null | grep -q . \
		|| die "HEAD ($(git rev-parse --short HEAD)) is not on any remote branch — push first"
	gh release view "$TAG" >/dev/null 2>&1 && die "release $TAG already exists"
fi

# Image + emulator presence is preflighted by iris-build.sh itself (clear
# errors, incl. the ci/local.conf guidance) — nothing to duplicate here.

IRIS_ARGS=""
[ -z "$IRIS_DIR_ARG" ] || IRIS_ARGS="--iris-dir $IRIS_DIR_ARG"

mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)

# ---- 2 + 3. native builds ------------------------------------------------------
# Sequential on purpose: two emulator instances would fight for CPU and the
# combined wall time barely differs. fetch-image resolves a local path (conf/
# env) or downloads from a *_DISK_URL — identical to the Actions build jobs.
if [ "$DO_O32" = 1 ]; then
	echo "==> [2/6] native o32 build (IRIX 5.3 guest)"
	IMG=$("$REPO/scripts/fetch-image.sh" --flavor o32 --dest "$OUTDIR/guest-disk-o32.chd")
	# shellcheck disable=SC2086 # IRIS_ARGS is deliberately word-split
	"$REPO/scripts/iris-build.sh" --flavor o32 --image "$IMG" $IRIS_ARGS --rb-cli "$RB" \
		--no-package --fresh --outdir "$OUTDIR"
else
	echo "==> [2/6] o32 build skipped"
fi

if [ "$DO_N32" = 1 ]; then
	echo "==> [3/6] native n32 build (IRIX 6.5 guest)"
	IMG=$("$REPO/scripts/fetch-image.sh" --flavor n32 --dest "$OUTDIR/guest-disk-n32.chd")
	# shellcheck disable=SC2086
	"$REPO/scripts/iris-build.sh" --flavor n32 --image "$IMG" $IRIS_ARGS --rb-cli "$RB" \
		--no-package --fresh --outdir "$OUTDIR"
else
	echo "==> [3/6] n32 build skipped"
fi

# ---- 4. Software Manager distribution ----------------------------------------------
if [ "$DO_INST" = 1 ]; then
	echo "==> [4/6] inst distribution (gendist in the 5.3 guest)"
	# shellcheck disable=SC2086
	"$REPO/scripts/iris-gendist.sh" --dir "$OUTDIR" --version "$VERSION" \
		$IRIS_ARGS --rb-cli "$RB"
else
	echo "==> [4/6] inst distribution skipped"
fi

# ---- 5. package -----------------------------------------------------------------
echo "==> [5/6] packaging .iso / .hda / .tar.gz"
"$REPO/scripts/package-dist.sh" --version "$VERSION" --dir "$OUTDIR" --rb-cli "$RB"

# ---- 6. release ------------------------------------------------------------------
# publish-release.sh is the same script the Actions release job runs, so the
# notes text and artifact set cannot drift between the two paths.
echo "==> [6/6] publishing $TAG"
set -- --version "$VERSION" --dist "$OUTDIR"
[ "$DRAFT" = 0 ]  || set -- "$@" --draft
[ "$DRYRUN" = 0 ] || set -- "$@" --dry-run
"$REPO/scripts/publish-release.sh" "$@"

if [ "$DRYRUN" = 1 ]; then
	echo
	echo "Artifacts staged in $OUTDIR:"
	ls -la "$OUTDIR"
fi
