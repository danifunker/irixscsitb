#!/bin/sh
# Build, package, and publish a GitHub release entirely from THIS machine —
# the local twin of .github/workflows/release.yml, for setups where the boot
# disks can't (or shouldn't) be hosted anywhere a CI runner could fetch them:
# all three native builds run in the IRIS emulator here, then `gh release create`
# uploads exactly the artifact set the Actions pipeline would have.
#
# What it runs, in order — every step is the SAME script the GitHub Actions
# workflow runs, so the two paths cannot drift:
#   1. preflight    git tree clean (release maps to a commit), HEAD pushed,
#                   gh authenticated; fetch-image.sh --check-only per flavor
#   2. builds       fetch-image.sh + iris-build.sh, once per enabled flavor:
#                   o32 (5.3 guest), mips1 (5.3 guest), n32 (6.5 guest)
#   3. package      scripts/package-dist.sh -> .iso / .hda / .tar.gz
#   4. release      scripts/publish-release.sh (gh release create)
#
# Boot disks come from ci/local.conf, $IRIX53_IMAGE/$IRIX65_IMAGE, or
# even a $IRIX53_DISK_URL/$IRIX65_DISK_URL download — the same resolution the
# workflow uses (scripts/fetch-image.sh). rb-cli is auto-provided by
# scripts/ensure-rbcli.sh if not installed.
#
# Usage:
#   scripts/release-local.sh [--version V] [--draft] [--dry-run]
#                            [--skip-o32] [--skip-mips1] [--skip-n32]
#                            [--allow-dirty]
#                            [--outdir DIR] [--iris-dir DIR] [--rb-cli PATH]
#
#   --version V    release version [UTC date stamp, e.g. 2026-07-28-15-04]
#   --draft        create the GitHub release as a draft
#   --dry-run      build + package, then PRINT the gh command instead of
#                  publishing (also skips the git-pushed preflight)
#   --skip-o32     skip the 5.3 o32/mips2 build
#   --skip-mips1   skip the 5.3 o32/mips1 build (nothing then runs on an R3000)
#   --skip-n32     skip the 6.5 n32 build (no 6.5 image available)
#   --skip-inst    skip the Software Manager products (passes --no-gendist to
#                  the builds; BUILD_INST=0 in ci/local.conf disables durably)
#   --allow-dirty  permit uncommitted changes (binaries stamp <rev>-dirty)
#
# A flavor can also be switched off durably with BUILD_O32=0 / BUILD_MIPS1=0 /
# BUILD_N32=0 in ci/local.conf (or the environment) — same knobs the workflow
# exposes as the build_* dispatch inputs and BUILD_* repo variables.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"
VERSION=""
DRAFT=0
DRYRUN=0
SKIP=""                         # flavors named by --skip-<flavor>
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
		--skip-o32)    SKIP="$SKIP o32"; shift ;;
		--skip-mips1)  SKIP="$SKIP mips1"; shift ;;
		--skip-n32)    SKIP="$SKIP n32"; shift ;;
		--skip-inst)   SKIP_INST=1; shift ;;
		--allow-dirty) ALLOW_DIRTY=1; shift ;;
		--outdir)      OUTDIR="$2"; shift 2 ;;
		--iris-dir)    IRIS_DIR_ARG="$2"; shift 2 ;;
		--rb-cli)      RB="$2"; shift 2 ;;
		-h|--help)     sed -n '2,39p' "$0"; exit 0 ;;
		*)             die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf fills in whatever flags/env didn't set

# Which flavors run: --skip-* flags > BUILD_O32/BUILD_MIPS1/BUILD_N32 (env or
# conf). DO is the enabled subset of $FLAVORS, in table order.
DO=""
for fl in $FLAVORS; do
	case " $SKIP " in *" $fl "*) echo "release-local: $fl skipped (--skip-$fl)"; continue ;; esac
	if ! flavor_enabled "$fl"; then
		echo "release-local: $fl disabled (BUILD_$(echo "$fl" | tr a-z A-Z) is off)"
		continue
	fi
	DO="$DO $fl"
done
[ -n "$DO" ] || die "nothing to build — every flavor is disabled"

# Software Manager products: each guest packages its OWN build with its own
# gendist inside the build session (iris-build.sh). --skip-inst is folded into
# BUILD_INST so every downstream script sees ONE switch — in particular
# package-dist.sh, which otherwise (rightly) refuses to package a built flavor
# that has no product.
[ "$SKIP_INST" = 0 ] || BUILD_INST=0
export BUILD_INST="${BUILD_INST:-1}"
GD_ARG="--require-gendist"
inst_enabled || GD_ARG="--no-gendist"

[ -n "$VERSION" ] || VERSION=$(date -u +%Y-%m-%d-%H-%M)
[ -n "$OUTDIR" ] || OUTDIR="$REPO/dist/release-$VERSION"
TAG="v$VERSION"

# ---- 1. preflight ------------------------------------------------------------
echo "==> [1/4] preflight (flavors:$DO)"
command -v gh >/dev/null 2>&1 || die "gh not found — needed to create the release"
# Same provisioning path as the Actions jobs: explicit choice > PATH >
# release download (ensure-rbcli.sh).
if [ -n "$RB" ]; then RB_CLI="$RB"; export RB_CLI; fi
RB=$("$REPO/scripts/ensure-rbcli.sh")
for fl in $DO; do
	"$REPO/scripts/fetch-image.sh" --flavor "$fl" --check-only
done

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

# ---- 2. native builds ------------------------------------------------------------
# Sequential on purpose: two emulator instances would fight for CPU and the
# combined wall time barely differs. fetch-image resolves a local path (conf/
# env) or downloads from a *_DISK_URL — identical to the Actions build jobs.
# The download lands per GUEST, so o32 and mips1 share one 5.3 disk.
for fl in $DO; do
	guest=$(flavor_guest "$fl")
	echo "==> [2/4] native $fl build + packaging (IRIX $(echo "$guest" | sed 's/./&./') guest)"
	IMG=$("$REPO/scripts/fetch-image.sh" --flavor "$fl" --dest "$OUTDIR/guest-disk-$guest.chd")
	# shellcheck disable=SC2086 # IRIS_ARGS/GD_ARG are deliberately word-split
	"$REPO/scripts/iris-build.sh" --flavor "$fl" --image "$IMG" $IRIS_ARGS $GD_ARG \
		--rb-cli "$RB" --version "$VERSION" --no-package --fresh --outdir "$OUTDIR"
done

# ---- 3. package -----------------------------------------------------------------
echo "==> [3/4] packaging media (.iso/.hda + gz, .tar.gz, .tardists)"
"$REPO/scripts/package-dist.sh" --version "$VERSION" --dir "$OUTDIR" --rb-cli "$RB"

# ---- 4. release ------------------------------------------------------------------
# publish-release.sh is the same script the Actions release job runs, so the
# notes text and artifact set cannot drift between the two paths.
echo "==> [4/4] publishing $TAG"
set -- --version "$VERSION" --dist "$OUTDIR"
[ "$DRAFT" = 0 ]  || set -- "$@" --draft
[ "$DRYRUN" = 0 ] || set -- "$@" --dry-run
"$REPO/scripts/publish-release.sh" "$@"

if [ "$DRYRUN" = 1 ]; then
	echo
	echo "Artifacts staged in $OUTDIR:"
	ls -la "$OUTDIR"
fi
