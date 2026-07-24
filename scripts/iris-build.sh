#!/bin/sh
# Build irixscsitb natively inside IRIX via the IRIS emulator, transferring source
# in and the compiled binary out over IRIS's built-in NFS server, then package
# the distributable artifacts with rb-cli. This is the headless-CI build path for
# the o32 / IRIX 5.3 binary.
#
# WHY native-in-IRIS instead of a cross-compiler: GNU binutils (2.17 and 2.35)
# cannot link against IRIX 5.3's o32 shared libs (sgi1.0 RLD format) and 5.3 has
# no static libc.a, so a GNU cross-toolchain can compile but not LINK for 5.3.
# IRIX's own MIPSpro cc + ld (already on the disk; the Makefile targets it) do it
# correctly.
#
# WHY NFS instead of the old scratch-volume push/pull: IRIS now ships a built-in,
# pure-Rust NFS server (no host unfsd, nothing to install). We export a host
# directory, mount it inside IRIX with a single `mount`, build there, and the
# binary lands straight back in the host folder — no tar staging, no scratch
# device, no `dd bs=512` sector math, no iris-ci push/pull round-trip.
#
# Prereqs:
#   - iris + iris-ci built (CHD support only needed if scsi.1 is a .chd):
#       (cd ../iris && cargo build --release --features chd)
#   - an installed IRIX 5.3 (or 6.5) boot disk with the IDO dev option (cc/make +
#     headers) AND networking up so it can mount NFS. For the recommended
#     ULTRA64 5.3 image, enable DHCP once with scripts/irix-enable-dhcp.sh; the
#     IRIS NAT then hands it 192.168.0.2 and answers NFS at the gateway
#     192.168.0.1. Real hardware just needs a working IP.
#   - rb-cli with the `optical new sgi-efs` + `new hd sgi-efs` verbs
#
# Usage:
#   scripts/iris-build.sh --version 2026-06-18 [--iris-dir ../iris]
#       [--config ci/iris-irix53.toml] [--rb-cli ./rb-cli] [--outdir dist]
#       [--gateway 192.168.0.1] [--bin-out PATH] [--no-package]
#
# --no-package builds only the o32 binary (skip rb-cli packaging); --bin-out PATH
# also copies it to PATH. CI uses both: build the authoritative 5.3 o32 here, then
# package it together with the cross-built n32 in a separate step.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
IRIS_DIR="$REPO/../iris"
CONFIG="$REPO/ci/iris-irix53.toml"
RB="${RB_CLI:-rb-cli}"
VERSION=""
OUTDIR="$REPO/dist"
GATEWAY="192.168.0.1"          # IRIS NAT gateway = the in-core NFS server address
DO_PACKAGE=1                   # --no-package: just emit the binary (CI packages separately)
BIN_OUT=""                     # --bin-out PATH: also copy the built o32 binary here

die() { echo "iris-build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version)    VERSION="$2"; shift 2 ;;
		--iris-dir)   IRIS_DIR="$2"; shift 2 ;;
		--config)     CONFIG="$2"; shift 2 ;;
		--rb-cli)     RB="$2"; shift 2 ;;
		--outdir)     OUTDIR="$2"; shift 2 ;;
		--gateway)    GATEWAY="$2"; shift 2 ;;
		--bin-out)    BIN_OUT="$2"; shift 2 ;;
		--no-package) DO_PACKAGE=0; shift ;;
		-h|--help)    sed -n '2,36p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

# --version is only needed for the packaging step; --no-package builds can skip it.
[ "$DO_PACKAGE" = 0 ] || [ -n "$VERSION" ] || die "missing --version"
IRIS_DIR=$(cd "$IRIS_DIR" 2>/dev/null && pwd) || die "iris dir not found"
IRIS="$IRIS_DIR/target/release/iris"
CI="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ] && [ -x "$CI" ] || die "build iris first: (cd $IRIS_DIR && cargo build --release)"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

WORK=$(mktemp -d)
SHARE="$WORK/share"                 # exported to IRIX over NFS as "/"
BIN="$WORK/irixscsitb-o32"
mkdir -p "$SHARE/src" "$SHARE/out"

cleanup() { "$CI" quit >/dev/null 2>&1 || true; kill "$(cat "$WORK/iris.pid" 2>/dev/null)" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# 1. Stage the IRIX-relevant sources into the NFS share (plain files, no tar).
#    The Makefile auto-detects uname IRIX -> the o32/mips2 `cc` build, so a plain
#    `make` inside the guest is all we need.
echo ">>> staging source into $SHARE/src"
cp "$REPO/irixscsitb.c" "$REPO/irix.c" "$REPO/irixscsitb.h" "$REPO/os.h" "$REPO/Makefile" "$SHARE/src/"

# 2. Launch iris headless with the CI control socket and the NFS export.
echo ">>> launching IRIS (headless, --ci, --nfs-dir $SHARE)"
( cd "$IRIS_DIR" && "$IRIS" --ci --config "$CONFIG" --nfs-dir "$SHARE" >"$WORK/iris.log" 2>&1 & echo $! > "$WORK/iris.pid" )

echo ">>> waiting for control socket"
i=0; until "$CI" ping >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 60 ] || die "iris socket never came up (see $WORK/iris.log)"; sleep 1; done

# 3. Boot to a root shell.
echo ">>> booting IRIX + login (root)"
"$CI" boot
"$CI" login

# 4. Mount the NFS share, build natively in /tmp, copy the binary back onto the
#    share. When `run` returns, the binary is already on the host at
#    $SHARE/out/irixscsitb-o32 (NFS writes are synchronous through the mount) —
#    no pull step. Building in /tmp (local disk) avoids any make-over-NFS mtime
#    quirks. If the mount fails, networking almost certainly isn't up in the
#    guest (see irix-enable-dhcp.sh); older clients may need `-o vers=2`.
echo ">>> mounting NFS ($GATEWAY:/) + building with native cc"
"$CI" run --shell sh "set -e; mkdir -p /mnt && mount $GATEWAY:/ /mnt && rm -rf /tmp/bsbuild && mkdir /tmp/bsbuild && cp /mnt/src/* /tmp/bsbuild/ && cd /tmp/bsbuild && make && cp irixscsitb /mnt/out/irixscsitb-o32 && umount /mnt"

# 5. The binary is on the host via NFS. Copy it out of the (soon-to-be-deleted)
#    work dir so downstream steps have a stable path.
[ -s "$SHARE/out/irixscsitb-o32" ] || die "no binary on the NFS share (build failed? mount failed? see $WORK/iris.log)"
cp "$SHARE/out/irixscsitb-o32" "$BIN"
echo ">>> built:"; file "$BIN" || true

# Optional: also drop the binary at a caller-chosen path (CI uploads this).
if [ -n "$BIN_OUT" ]; then
	cp "$BIN" "$BIN_OUT"
	echo ">>> o32 binary: $BIN_OUT"
fi

# 6. Package the .iso / .hda / .tar.gz with rb-cli (unless --no-package, e.g. CI
#    that packages o32 + the cross-built n32 together in a separate step).
if [ "$DO_PACKAGE" = 1 ]; then
	echo ">>> packaging distributable artifacts"
	"$REPO/scripts/package.sh" \
		--bin "$BIN" --version "$VERSION" --outdir "$OUTDIR" \
		--rb-cli "$RB" --extra "$REPO/README.md"
	echo
	echo "Done. o32/IRIX-5.3 binary built natively in IRIS and packaged:"
	ls -la "$OUTDIR"/irixscsitb-"$VERSION".* 2>/dev/null || true
else
	echo
	echo "Done. o32/IRIX-5.3 binary built natively in IRIS (packaging skipped)."
fi
