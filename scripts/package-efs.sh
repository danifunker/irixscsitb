#!/bin/sh
# Package the IRIX bstoolbox binary into distributable EFS images with rb-cli
# (https://github.com/danifunker/rusty-backup). Produces two artifacts for the
# IRIS emulator (and real BlueSCSI hardware):
#
#   1. EFS CD image   - a bare EFS superfloppy. Served as the emulated CD-ROM;
#                       IRIX mounts it directly with `mount -t efs`.
#   2. SGI EFS HDD     - an SGI disk volume header (dvh) + partition table
#                       wrapping an EFS root partition. Served as a SCSI hard
#                       disk; recognised by IRIX `fx` / `prtvtoc`.
#
# Both are verified with `fsck` and a round-trip extract before the script exits.
#
# Usage:
#   scripts/package-efs.sh --bin PATH/bstoolbox --version VER [options]
#
# Options (defaults in brackets):
#   --bin PATH        IRIX bstoolbox binary to package (required)
#   --version VER     version string used in output filenames (required)
#   --outdir DIR      where to write the images        [dist]
#   --rb-cli PATH     rb-cli binary    [$RB_CLI, then `rb-cli` on PATH]
#   --name LABEL      EFS volume label, max 6 bytes     [BSTOOL]
#   --cd-size SIZE    EFS CD image size                 [8M]
#   --hdd-size SIZE   SGI HDD image size                [50M]
#   --heads N         HDD geometry heads (IRIS = 16)    [16]
#   --sectors N       HDD geometry sectors/track (IRIS = 63) [63]
#   --extra PATH      additional file to drop at image root (repeatable)
#
# rb-cli must be a build that has the `new-sgi-hdd` verb.
set -eu

BIN=""
VERSION=""
OUTDIR="dist"
RB="${RB_CLI:-rb-cli}"
NAME="BSTOOL"
CD_SIZE="8M"
HDD_SIZE="50M"
HEADS="16"
SECTORS="63"
EXTRAS=""

die() { echo "package-efs: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--bin)      BIN="$2"; shift 2 ;;
		--version)  VERSION="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		--rb-cli)   RB="$2"; shift 2 ;;
		--name)     NAME="$2"; shift 2 ;;
		--cd-size)  CD_SIZE="$2"; shift 2 ;;
		--hdd-size) HDD_SIZE="$2"; shift 2 ;;
		--heads)    HEADS="$2"; shift 2 ;;
		--sectors)  SECTORS="$2"; shift 2 ;;
		--extra)    EXTRAS="$EXTRAS $2"; shift 2 ;;
		-h|--help)  sed -n '2,40p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$BIN" ] || die "missing --bin"
[ -n "$VERSION" ] || die "missing --version"
[ -f "$BIN" ] || die "binary not found: $BIN"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"
# Fail early if rb-cli predates the HDD builder.
"$RB" new-sgi-hdd --help >/dev/null 2>&1 || die "this rb-cli has no 'new-sgi-hdd' verb; update rb-cli"

mkdir -p "$OUTDIR"
CD_IMG="$OUTDIR/bstoolbox-efs-cd-$VERSION.img"
HDD_IMG="$OUTDIR/bstoolbox-sgi-hdd-$VERSION.img"

# put_payload <image-ref> : drop the binary + any extras at the volume root.
# <image-ref> is "img" for the single-partition CD, "img@1" for the HDD's EFS root.
put_payload() {
	ref="$1"
	"$RB" put "$ref" "$BIN" /bstoolbox
	for f in $EXTRAS; do
		"$RB" put "$ref" "$f" "/$(basename "$f")"
	done
}

echo ">>> [1/2] EFS CD image: $CD_IMG"
"$RB" new --fs efs --size "$CD_SIZE" --name "$NAME" "$CD_IMG"
put_payload "$CD_IMG"
"$RB" ls "$CD_IMG" /
"$RB" fsck "$CD_IMG"

echo ">>> [2/2] SGI EFS HDD image: $HDD_IMG"
"$RB" new-sgi-hdd "$HDD_IMG" --size "$HDD_SIZE" --name "$NAME" --heads "$HEADS" --sectors "$SECTORS"
put_payload "$HDD_IMG@1"
"$RB" ls "$HDD_IMG@1" /
"$RB" fsck "$HDD_IMG@1"

# Round-trip both: the bytes we put back must match the source binary.
verify_roundtrip() {
	ref="$1"; label="$2"
	tmpd="$(mktemp -d)"
	"$RB" get "$ref" /bstoolbox "$tmpd/out"
	cmp "$BIN" "$tmpd/out" || { rm -rf "$tmpd"; die "$label round-trip MISMATCH"; }
	rm -rf "$tmpd"
	echo "    $label round-trip OK"
}
verify_roundtrip "$CD_IMG" "CD"
verify_roundtrip "$HDD_IMG@1" "HDD"

echo
echo "Packaged bstoolbox $VERSION:"
ls -la "$CD_IMG" "$HDD_IMG"
echo "Note: EFS stores Unix mode bits - on IRIX run 'chmod +x bstoolbox' after"
echo "copying it off the media (or in place on the HDD) before executing."
