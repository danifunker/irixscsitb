#!/bin/sh
# Standalone Software Manager product generation — normally NOT needed:
# scripts/iris-build.sh already runs the guest's own gendist in the same
# session that compiles (each OS packages its own build). This script exists
# for two cases:
#
#   1. Regenerating a product from existing binaries without rebuilding
#      (e.g. after editing inst/irixscsitb.spec).
#   2. PROVISIONING a 5.3 guest that lacks gendist: the tool ships in the
#      `inst_dev.sw` subsystem ("Software Packager") of the IRIS Development
#      Option 5.3 CD. With IRIX53_IDO_ISO set in ci/local.conf, this script
#      attaches that CD and installs the subsystem via a scripted inst
#      session — into the guest's copy-on-write overlay only.
#
# The provisioning session deliberately does NOT quit inst, resolve
# conflicts, or verify the installation afterwards: once `go` reports
# success the files are on disk, and inst is simply suspended (Ctrl-Z) to
# get the shell back. inst's quit path runs a machine-incompatibility check
# that can misfire under an emulated/headless hinv and push removals of
# legitimately installed software (a Newport graphics patch, in one painful
# case) — nothing here will ever touch it.
#
# Each invocation produces the product for ONE flavor, packaged by the
# matching OS: --guest 53 packages irixscsitb-o32 (5.3-format product,
# readable by every inst 5.3-6.5) into OUTDIR; --guest 65 packages
# irixscsitb-n32 (6.5 format).
#
# Usage:
#   scripts/iris-gendist.sh --dir DIR --version V [--guest 53|65]
#       [--image PATH] [--iris-dir DIR] [--rb-cli PATH] [--outdir DIR]
#       [--dist-version N] [--workdir DIR] [--fresh]
#
#   --dir DIR          where the built binaries live (irixscsitb-o32 or
#                      irixscsitb-n32 per --guest; GUI included when present)
#   --outdir DIR       where the product trio lands [DIR/inst53 or DIR/inst65]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

GUEST="53"
DIR=""
OUTDIR=""
VERSION=""
DISTVER=""
IMAGE=""
IRIS_DIR="${IRIS_DIR:-}"
RB=""
WORKDIR=""
FRESH=0
ROOT_PW="${IRIX_ROOT_PASSWORD:-}"

die() { echo "iris-gendist: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--guest)        GUEST="$2"; shift 2 ;;
		--dir)          DIR="$2"; shift 2 ;;
		--outdir)       OUTDIR="$2"; shift 2 ;;
		--version)      VERSION="$2"; shift 2 ;;
		--dist-version) DISTVER="$2"; shift 2 ;;
		--image)        IMAGE="$2"; shift 2 ;;
		--iris-dir)     IRIS_DIR="$2"; shift 2 ;;
		--rb-cli)       RB="$2"; shift 2 ;;
		--workdir)      WORKDIR="$2"; shift 2 ;;
		--fresh)        FRESH=1; shift ;;
		-h|--help)      sed -n '2,36p' "$0"; exit 0 ;;
		*)              die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$RB" ] || RB="${RB_CLI:-rb-cli}"

case "$GUEST" in
	53) FLAVOR="o32"; INST_NAME="inst53" ;;
	65) FLAVOR="n32"; INST_NAME="inst65" ;;
	*)  die "--guest must be 53 or 65" ;;
esac

[ -n "$DIR" ] || die "missing --dir (the directory with irixscsitb-$FLAVOR)"
DIR=$(cd "$DIR" 2>/dev/null && pwd) || die "not a directory: $DIR"
[ -f "$DIR/irixscsitb-$FLAVOR" ] || die "missing $DIR/irixscsitb-$FLAVOR (build that flavor first)"
[ -n "$OUTDIR" ] || OUTDIR="$DIR/$INST_NAME"

[ -n "$VERSION" ] || [ -n "$DISTVER" ] || die "missing --version (or --dist-version)"
if [ -z "$DISTVER" ]; then
	DISTVER=$(dist_version_from "$VERSION")
	[ -n "$DISTVER" ] || die "cannot derive a numeric inst version from '$VERSION'; pass --dist-version"
fi

if [ -z "$IMAGE" ]; then
	IMAGE=$(resolve_local_image "$FLAVOR")
	[ -n "$IMAGE" ] || die "no boot disk for guest $GUEST — set $(flavor_img_key "$FLAVOR") (ci/local.conf) or pass --image"
fi
[ -f "$IMAGE" ] || die "boot disk not found: $IMAGE"
IMAGE=$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")

case "$GUEST" in
	53) CONFIG="$REPO/ci/iris-irix53.toml" ;;
	65) CONFIG="$REPO/ci/iris-irix65.toml" ;;
esac

if [ -z "$IRIS_DIR" ]; then IRIS_DIR="$REPO/../iris"; fi
if ! _r=$(cd "$IRIS_DIR" 2>/dev/null && pwd); then
	_r=$(cd "$REPO/$IRIS_DIR" 2>/dev/null && pwd) || die "iris dir not found: $IRIS_DIR"
fi
IRIS_DIR="$_r"
IRIS="$IRIS_DIR/target/release/iris"
CI_BIN="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ] && [ -x "$CI_BIN" ] || die "need $IRIS + iris-ci (scripts/fetch-iris.sh)"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

# ---- work area -----------------------------------------------------------------
[ -n "$WORKDIR" ] || WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/irisgendist.XXXXXX")
mkdir -p "$WORKDIR"; WORKDIR=$(cd "$WORKDIR" && pwd)
SOCK="/tmp/iris-gd.$$.sock"
STAGE="$WORKDIR/stage"
HDA="$WORKDIR/work.hda"
CONSOLE="$WORKDIR/console.log"

CI() { IRIS_SOCKET="$SOCK" "$CI_BIN" "$@"; }
cleanup() {
	CI quit >/dev/null 2>&1 || true; sleep 1
	kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true
	rm -f "$SOCK"
}
trap cleanup EXIT INT TERM

ser_send() { CI serial-send "$1"; }
ser_wait() { CI -q serial-wait "$1" --timeout "$2" >/dev/null 2>&1; }
ser_drain() { CI serial-read >/dev/null 2>&1 || true; }
ser_wait_long() {
	_p="$1"; _n="$2"; _l="$3"
	while [ "$_n" -gt 0 ]; do
		ser_wait "$_p" 170 && return 0
		_n=$(($_n - 1)); echo "    ... still waiting for $_l"
	done
	echo "iris-gendist: timed out waiting for $_l; console tail:" >&2
	tail -25 "$CONSOLE" >&2 || true
	return 1
}

# ---- 1. stage: spec+idb (via ci-lib) + this flavor's binaries --------------------
echo ">>> staging gendist inputs ($FLAVOR, inst version $DISTVER)"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/out"
stage_inst_inputs "$FLAVOR" "$DISTVER" "$STAGE"
cp "$DIR/irixscsitb-$FLAVOR" "$STAGE/bin/irixscsitb"
[ -f "$DIR/scsitbgui-$FLAVOR" ] && cp "$DIR/scsitbgui-$FLAVOR" "$STAGE/bin/scsitbgui"

echo ">>> assembling work disk"
rm -f "$HDA"
"$RB" -q --progress never new hd sgi-efs "$HDA" --size 64M --heads 16 --sectors 63 --from-dir "$STAGE"

# ---- 2. launch -------------------------------------------------------------------
if [ "$FRESH" = 1 ]; then rm -f "${IMAGE}.diff.chd"; fi
IDO_ARGS=""
if [ "$GUEST" = 53 ] && [ -n "${IRIX53_IDO_ISO:-}" ]; then
	[ -f "$IRIX53_IDO_ISO" ] || die "IRIX53_IDO_ISO not found: $IRIX53_IDO_ISO"
	IDO_ARGS="--cdrom4 $IRIX53_IDO_ISO"
fi
echo ">>> launching IRIS ($GUEST guest, boot=$IMAGE)"
# shellcheck disable=SC2086 # IDO_ARGS is deliberately word-split
( cd "$WORKDIR" && "$IRIS" --ci --config "$CONFIG" --ci-socket "$SOCK" \
	--scsi1 "$IMAGE" --scsi2 "$HDA" --serial-log "$CONSOLE" $IDO_ARGS \
	> "$WORKDIR/iris.log" 2>&1 & echo $! > "$WORKDIR/iris.pid" )
i=0; until CI ping >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 30 ] || die "iris socket never came up"; sleep 1; done

# ---- 3. boot ---------------------------------------------------------------------
CI start
ser_wait "Option?" 90 || die "PROM menu never appeared (see $CONSOLE)"
if [ "$GUEST" = 53 ]; then
	echo ">>> booting single-user"
	ser_send "5";  ser_wait ">>" 30 || die "command monitor not reached"
	ser_send "boot -f dksc(0,1,8)sash"; ser_wait "sash" 60 || die "sash never loaded"
	sleep 1
	ser_send "boot -f dksc(0,1,0)unix initstate=s"
	ser_wait_long "Single User Mode" 2 "single-user prompt" || exit 1
	ser_send "$ROOT_PW"; sleep 2
else
	echo ">>> booting multiuser"
	ser_send "1"
	ser_wait_long "console login" 3 "console login" || exit 1
	if [ -n "$ROOT_PW" ]; then CI -q login root --password "$ROOT_PW"; else CI -q login root; fi
fi

# ---- 4. mount work disk + provision gendist if needed ------------------------------
# (5.3's mkdir -p errors when the directory already exists, hence test -d.)
ser_send "test -d /mnt || mkdir /mnt ; mount /dev/dsk/dks0d2s0 /mnt && echo GD-'MNT'-OK"
ser_wait "GD-MNT-OK" 60 || { echo "work disk mount failed:" >&2; tail -8 "$CONSOLE" >&2; exit 1; }

ser_send "test -x /usr/sbin/gendist && echo GD-'HAVE'-TOOL || echo GD-'NEED'-TOOL"
if ! ser_wait "GD-HAVE-TOOL" 20; then
	[ "$GUEST" = 53 ] || die "gendist missing in the 6.5 guest (its inst_dev.sw.base normally ships it)"
	[ -n "${IRIX53_IDO_ISO:-}" ] || die "gendist is not installed in the 5.3 guest.
  Set IRIX53_IDO_ISO in ci/local.conf to the 'IRIS Development Option 5.3'
  CD image and this script will install inst_dev.sw (Software Packager) into
  the guest's overlay automatically."
	echo ">>> installing inst_dev.sw from the IDO CD (one-time per overlay)"
	ser_send "test -d /cd || mkdir /cd ; mount -t efs -o ro /dev/dsk/dks0d4s7 /cd && echo GD-'CD'-OK"
	ser_wait "GD-CD-OK" 60 || die "IDO CD mount failed (see $CONSOLE)"
	# stty rows 1000 keeps inst from paginating; syncing on a fresh Inst>
	# after each selection keeps waits honest (they stream progress for many
	# seconds). After `go` succeeds the files are installed — inst is then
	# SUSPENDED (Ctrl-Z), never quit: its quit path runs a machine check that
	# can demand removals under an emulated hinv, and no verification or
	# conflict resolution happens here by design.
	ser_send "stty rows 1000 ; inst -f /cd/dist"
	ser_wait_long "Inst>" 2 "inst main menu" || exit 1
	inst_sync() {
		ser_drain
		ser_send ""
		CI -q serial-wait "Inst>" --timeout 60 >/dev/null 2>&1 || true
		sleep 1
		ser_drain
	}
	inst_sync
	ser_send "keep *"
	ser_wait "Inst>" 60 || true
	inst_sync
	ser_send "install inst_dev.sw"
	ser_wait "Inst>" 170 || true
	inst_sync
	ser_send "go"
	GO_OUT=""; _n=3
	while [ "$_n" -gt 0 ]; do
		if _chunk=$(CI serial-wait "Inst>" --timeout 170 2>/dev/null); then
			GO_OUT="$GO_OUT$_chunk"; break
		fi
		_n=$(($_n - 1)); echo "    ... still waiting for inst go"
	done
	case "$GO_OUT" in
	*successful*) : ;;
	*) printf '%s\n' "$GO_OUT" | tail -20 >&2; die "inst go did not report success (see above / $CONSOLE)" ;;
	esac
	# Suspend inst; the shell prompt comes back and gendist is on disk.
	ser_send "$(printf '\032')"
	sleep 3; ser_drain
	ser_send "umount /cd ; echo GD-'CD'-DONE"
	ser_wait "GD-CD-DONE" 30 || true
fi

# ---- 5. gendist --------------------------------------------------------------------
echo ">>> running gendist in the guest"
ser_send "rm -rf /tmp/gd && mkdir /tmp/gd /tmp/gd/dist && cp -r /mnt/bin /mnt/irixscsitb.spec /mnt/irixscsitb.idb /tmp/gd && echo GD-'STAGE'-OK"
ser_wait "GD-STAGE-OK" 90 || { tail -8 "$CONSOLE" >&2; exit 1; }
ser_send "cd /tmp/gd && (test -f bin/scsitbgui && cp irixscsitb.idb idb.f || grep -v scsitbgui irixscsitb.idb > idb.f) && gendist -verbose -sbase /tmp/gd -idb /tmp/gd/idb.f -spec /tmp/gd/irixscsitb.spec -dist /tmp/gd/dist -all && echo GD-'GEN'-OK || echo GD-'GEN'-FAIL"
ser_wait_long "GD-GEN-OK" 2 "gendist" || { tail -20 "$CONSOLE" >&2; exit 1; }
ser_send "cp /tmp/gd/dist/* /mnt/out/ && cd / && umount /mnt && sync && echo GD-'XFER'-OK"
ser_wait "GD-XFER-OK" 60 || { tail -8 "$CONSOLE" >&2; exit 1; }
CI quit >/dev/null 2>&1 || true
sleep 2

# ---- 6. extract ---------------------------------------------------------------------
mkdir -p "$OUTDIR"
for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
	"$RB" -q get --force "$HDA@1" "/out/$f" "$OUTDIR/$f"
	[ -s "$OUTDIR/$f" ] || die "missing $f on the work disk (see $CONSOLE)"
done
echo ">>> Software Manager product ($FLAVOR, packaged by the $GUEST guest):"
ls -la "$OUTDIR"
echo "Work dir kept for inspection: $WORKDIR"
