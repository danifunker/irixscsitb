#!/bin/sh
# Local end-to-end build: cross-compile bstoolbox for IRIX from a sysroot tarball
# (built by scripts/make-irix-sysroot.sh), then package the EFS CD + SGI HDD
# images with rb-cli. This is the same flow the release workflow runs, but on
# your machine. Requires Docker running.
#
# Usage:
#   scripts/build-local.sh --sysroot irix-5.3-sysroot.tgz [--rb-cli PATH]
#                          [--version VER] [--outdir DIR]
#
# The toolchain image is built locally and never pushed (it embeds the
# proprietary sysroot - keep it on this machine).
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
SYSROOT=""
RB="${RB_CLI:-rb-cli}"
VERSION="${VERSION:-local-$(date -u +%Y%m%d-%H%M)}"
OUTDIR="$REPO/dist"
IMAGE_TAG="irixtoolbox-cross:local"

die() { echo "build-local: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--sysroot) SYSROOT="$2"; shift 2 ;;
		--rb-cli)  RB="$2"; shift 2 ;;
		--version) VERSION="$2"; shift 2 ;;
		--outdir)  OUTDIR="$2"; shift 2 ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

[ -n "$SYSROOT" ] || die "missing --sysroot (e.g. irix-5.3-sysroot.tgz)"
[ -f "$SYSROOT" ] || die "sysroot tarball not found: $SYSROOT"
docker info >/dev/null 2>&1 || die "Docker isn't running - start Docker Desktop and retry"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

TC="$REPO/docker/irix-toolchain"

echo "==> [1/3] staging sysroot into the toolchain build context"
cp "$SYSROOT" "$TC/sysroot.tgz"

echo "==> [2/3] building the IRIX cross-toolchain image (first run takes a while)"
docker build -t "$IMAGE_TAG" "$TC"

echo "==> [3/3] cross-compiling bstoolbox (o32, runs on IRIX 5.3-6.5)"
# Compile inside the toolchain container; gcc finds the sysroot via its baked-in
# --with-sysroot default. -mabi=32 -mips2 = o32.
docker run --rm -v "$REPO":/work -w /work "$IMAGE_TAG" sh -c '
	set -eux
	mips-sgi-irix6.5-gcc -DOS_IRIX -O2 -mabi=32 -mips2 \
		-o bstoolbox-o32 bstoolbox.c irix.c
	file bstoolbox-o32
'

echo "==> packaging EFS CD + SGI HDD images"
"$REPO/scripts/package-efs.sh" \
	--bin "$REPO/bstoolbox-o32" \
	--version "$VERSION" \
	--outdir "$OUTDIR" \
	--rb-cli "$RB" \
	--extra "$REPO/README.md"

echo
echo "Local build complete:"
ls -la "$REPO/bstoolbox-o32" "$OUTDIR"/*.img
