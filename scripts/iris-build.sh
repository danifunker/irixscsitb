#!/bin/sh
# Build bstoolbox natively inside IRIX via the IRIS emulator's CI socket, then
# package the EFS images with rb-cli. This is the headless-CI build path for the
# o32 / IRIX 5.3 binary.
#
# WHY native-in-IRIS instead of a cross-compiler: GNU binutils (2.17 and 2.35)
# cannot link against IRIX 5.3's o32 shared libs (sgi1.0 RLD format) and 5.3 has
# no static libc.a, so a GNU cross-toolchain can compile but not LINK for 5.3.
# IRIX's own MIPSpro cc + ld (already on the disk; the Makefile targets it) do it
# correctly. IRIS exposes a CI socket (iris-ci) with push/pull/run, so this whole
# thing runs headless.
#
# Prereqs:
#   - iris + iris-ci built:  (cd ../iris && cargo build --release)
#   - an IRIX 5.3 (or 6.5) machine config with a `scratch = true` SCSI volume and
#     the IDO dev option installed (provides cc/make + headers)
#   - rb-cli with the new-sgi-hdd verb
#
# Usage:
#   scripts/iris-build.sh --version 2026-06-18 [--iris-dir ../iris]
#       [--config iris-irix53.toml] [--rb-cli ./rb-cli] [--outdir dist]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
IRIS_DIR="$REPO/../iris"
CONFIG="iris-irix53.toml"
RB="${RB_CLI:-rb-cli}"
VERSION=""
OUTDIR="$REPO/dist"

die() { echo "iris-build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version)  VERSION="$2"; shift 2 ;;
		--iris-dir) IRIS_DIR="$2"; shift 2 ;;
		--config)   CONFIG="$2"; shift 2 ;;
		--rb-cli)   RB="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		-h|--help)  sed -n '2,30p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
IRIS_DIR=$(cd "$IRIS_DIR" 2>/dev/null && pwd) || die "iris dir not found"
IRIS="$IRIS_DIR/target/release/iris"
CI="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ] && [ -x "$CI" ] || die "build iris first: (cd $IRIS_DIR && cargo build --release)"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

WORK=$(mktemp -d)
SRC_TAR="$WORK/bstoolbox-src.tar"
BIN="$WORK/bstoolbox-o32"

# 1. Tar the IRIX-relevant sources. The Makefile auto-detects uname IRIX -> the
#    o32/mips2 `cc` build, so a plain `make` inside the guest is all we need.
echo ">>> staging source"
tar cf "$SRC_TAR" -C "$REPO" bstoolbox.c irix.c bstoolbox.h os.h Makefile

# 2. Launch iris headless with the CI control socket.
echo ">>> launching IRIS (headless, --ci)"
( cd "$IRIS_DIR" && "$IRIS" --ci --config "$CONFIG" >"$WORK/iris.log" 2>&1 & echo $! > "$WORK/iris.pid" )
cleanup() { "$CI" quit >/dev/null 2>&1 || true; kill "$(cat "$WORK/iris.pid" 2>/dev/null)" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

echo ">>> waiting for control socket"
i=0; until "$CI" ping >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 60 ] || die "iris socket never came up (see $WORK/iris.log)"; sleep 1; done

# 3. Boot to a root shell.
echo ">>> booting IRIX + login (root)"
"$CI" boot
"$CI" login

# 4. Push source, build natively, pull the binary back. push/pull handle the
#    scratch-volume tar round-trip; run returns the guest exit code.
echo ">>> pushing source + building with native cc"
"$CI" push "$SRC_TAR" --to /tmp/bstoolbox-src.tar
"$CI" run --shell sh "cd /tmp && rm -rf bsbuild && mkdir bsbuild && cd bsbuild && tar xf ../bstoolbox-src.tar && make"
echo ">>> pulling the o32 binary"
"$CI" pull /tmp/bsbuild/bstoolbox --to "$BIN"

[ -s "$BIN" ] || die "no binary came back from the guest (build failed? see $WORK/iris.log)"
echo ">>> built:"; file "$BIN" || true

# 5. Package the EFS CD + SGI HDD images with rb-cli.
echo ">>> packaging EFS images"
"$REPO/scripts/package-efs.sh" \
	--bin "$BIN" --version "$VERSION" --outdir "$OUTDIR" \
	--rb-cli "$RB" --extra "$REPO/README.md"

echo
echo "Done. o32/IRIX-5.3 binary built natively in IRIS and packaged:"
ls -la "$OUTDIR"/*.img
