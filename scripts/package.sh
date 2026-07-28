#!/bin/sh
# Package the IRIX irixscsitb binary into distributable artifacts with rb-cli
# (https://github.com/danifunker/rusty-backup). Produces three things a user can
# drop straight into the IRIS emulator (or onto real SGI/BlueSCSI hardware):
#
#   1. .iso  - IRIX EFS CD-ROM image: an SGI volume header + partition table with
#              the EFS filesystem in slot 7 (typed SYSV, the IRIX EFS-CD
#              convention) and CD geometry. In IRIS: attach as a `cdrom = true`
#              SCSI device (or drop into a `discs = [...]` changer). On IRIX:
#              `mount -t efs -o ro /dev/dsk/dks0d<N>s7 /CDROM`.
#   2. .hda  - SGI EFS hard-disk image: dvh volume header + partition table
#              wrapping an EFS root partition (slot 0). In IRIS: attach as a
#              `cdrom = false` SCSI device. Recognised by IRIX `fx` / `prtvtoc`.
#   3. .tar.gz - a plain gzip tarball of the binaries + README. The friendliest
#              vector now that IRIS has a built-in NFS server: point `[nfs]
#              shared_dir` at a folder, drop this tarball's contents in, mount
#              `192.168.0.1:/` inside IRIX and untar. No image to build at all.
#
# The .iso and .hda are created + populated + fsck'd + round-trip-verified.
#
# rb-cli grammar note: this targets CURRENT rb-cli, where the CD builder is
# `optical new sgi-efs` and the HDD builder is `new hd sgi-efs`. The old
# top-level `new-sgi-cdrom` / `new-sgi-hdd` / `new --fs efs` verbs were renamed.
#
# Usage:
#   scripts/package.sh --bin PATH/irixscsitb-o32 --version VER [options]
#
# Options (defaults in brackets):
#   --bin PATH        primary IRIX binary -> images (as /irixscsitb) + tarball (required)
#   --gui-bin PATH    Motif GUI binary -> images (as /scsitbgui) + tarball (optional)
#   --version VER     version string used in output filenames (required)
#   --outdir DIR      where to write the artifacts       [dist]
#   --rb-cli PATH     rb-cli binary    [$RB_CLI, then `rb-cli` on PATH]
#   --name LABEL      EFS volume label, max 6 bytes       [BSTOOL]
#   --extra PATH      extra file at image root + in tarball (repeatable)
#   --tar-bin PATH    extra binary for the tarball only, e.g. the n32 build (repeatable)
#   --cd-size SIZE    EFS CD image size                   [8M]
#   --hdd-size SIZE   SGI HDD image size                  [50M]
#   --heads N         HDD geometry heads (IRIS = 16)      [16]
#   --sectors N       HDD geometry sectors/track (IRIS = 63) [63]
#   --no-iso          skip the .iso
#   --no-hda          skip the .hda
#   --no-tar          skip the .tar.gz
set -eu

BIN=""
GUI_BIN=""
VERSION=""
OUTDIR="dist"
RB="${RB_CLI:-rb-cli}"
NAME="BSTOOL"
CD_SIZE="8M"
HDD_SIZE="50M"
HEADS="16"
SECTORS="63"
EXTRAS=""
TAR_BINS=""
DO_ISO=1
DO_HDA=1
DO_TAR=1

die() { echo "package: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--bin)      BIN="$2"; shift 2 ;;
		--gui-bin)  GUI_BIN="$2"; shift 2 ;;
		--version)  VERSION="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		--rb-cli)   RB="$2"; shift 2 ;;
		--name)     NAME="$2"; shift 2 ;;
		--extra)    EXTRAS="$EXTRAS $2"; shift 2 ;;
		--tar-bin)  TAR_BINS="$TAR_BINS $2"; shift 2 ;;
		--cd-size)  CD_SIZE="$2"; shift 2 ;;
		--hdd-size) HDD_SIZE="$2"; shift 2 ;;
		--heads)    HEADS="$2"; shift 2 ;;
		--sectors)  SECTORS="$2"; shift 2 ;;
		--no-iso)   DO_ISO=0; shift ;;
		--no-hda)   DO_HDA=0; shift ;;
		--no-tar)   DO_TAR=0; shift ;;
		-h|--help)  sed -n '2,42p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$BIN" ] || die "missing --bin"
[ -n "$VERSION" ] || die "missing --version"
[ -f "$BIN" ] || die "binary not found: $BIN"
[ -z "$GUI_BIN" ] || [ -f "$GUI_BIN" ] || die "gui binary not found: $GUI_BIN"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

# Fail early (and clearly) if this rb-cli predates the current builder grammar.
if [ "$DO_ISO" = 1 ]; then
	"$RB" optical new sgi-efs --help >/dev/null 2>&1 || \
		die "this rb-cli lacks 'optical new sgi-efs' (update rb-cli; the old 'new-sgi-cdrom' verb was renamed)"
fi
if [ "$DO_HDA" = 1 ]; then
	"$RB" new hd sgi-efs --help >/dev/null 2>&1 || \
		die "this rb-cli lacks 'new hd sgi-efs' (update rb-cli; the old 'new-sgi-hdd' verb was renamed)"
fi

mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)          # absolutise so tar's -f resolves cleanly
ISO_IMG="$OUTDIR/irixscsitb-$VERSION.iso"
HDD_IMG="$OUTDIR/irixscsitb-$VERSION.hda"
TARBALL="$OUTDIR/irixscsitb-$VERSION.tar.gz"

# put_payload <image-ref> : drop the binaries (as /irixscsitb [+ /scsitbgui])
# + any extras at the volume root. <image-ref> addresses the EFS partition as
# "@1" for both the CD (slot 7) and the HDD (slot 0) — rb-cli maps @1 to the
# sole EFS partition.
put_payload() {
	ref="$1"
	"$RB" put "$ref" "$BIN" /irixscsitb
	[ -z "$GUI_BIN" ] || "$RB" put "$ref" "$GUI_BIN" /scsitbgui
	for f in $EXTRAS; do
		"$RB" put "$ref" "$f" "/$(basename "$f")"
	done
}

# Round-trip: each binary we read back must match its source.
verify_roundtrip() {
	ref="$1"; label="$2"
	tmpd="$(mktemp -d)"
	"$RB" get "$ref" /irixscsitb "$tmpd/out"
	cmp "$BIN" "$tmpd/out" || { rm -rf "$tmpd"; die "$label round-trip MISMATCH"; }
	if [ -n "$GUI_BIN" ]; then
		"$RB" get "$ref" /scsitbgui "$tmpd/gui"
		cmp "$GUI_BIN" "$tmpd/gui" || { rm -rf "$tmpd"; die "$label GUI round-trip MISMATCH"; }
	fi
	rm -rf "$tmpd"
	echo "    $label round-trip OK"
}

if [ "$DO_ISO" = 1 ]; then
	echo ">>> EFS CD-ROM image: $ISO_IMG"
	"$RB" optical new sgi-efs "$ISO_IMG" --size "$CD_SIZE" --name "$NAME"
	put_payload "$ISO_IMG@1"
	"$RB" ls "$ISO_IMG@1" /
	"$RB" fsck "$ISO_IMG@1"
	verify_roundtrip "$ISO_IMG@1" "iso"
fi

if [ "$DO_HDA" = 1 ]; then
	echo ">>> SGI EFS HDD image: $HDD_IMG"
	"$RB" new hd sgi-efs "$HDD_IMG" --size "$HDD_SIZE" --name "$NAME" --heads "$HEADS" --sectors "$SECTORS"
	put_payload "$HDD_IMG@1"
	"$RB" ls "$HDD_IMG@1" /
	"$RB" fsck "$HDD_IMG@1"
	verify_roundtrip "$HDD_IMG@1" "hda"
fi

if [ "$DO_TAR" = 1 ]; then
	echo ">>> tarball: $TARBALL"
	stage="$(mktemp -d)"
	top="irixscsitb-$VERSION"
	mkdir -p "$stage/$top"
	# Binaries are stored by basename and made executable, so `tar xf` yields a
	# ready-to-run binary (unlike the EFS images, which land 0644 — chmod +x
	# after copying off the media).
	cp "$BIN" "$stage/$top/"
	chmod +x "$stage/$top/$(basename "$BIN")"
	if [ -n "$GUI_BIN" ]; then
		cp "$GUI_BIN" "$stage/$top/"
		chmod +x "$stage/$top/$(basename "$GUI_BIN")"
	fi
	for f in $TAR_BINS; do
		[ -f "$f" ] || die "tar-bin not found: $f"
		cp "$f" "$stage/$top/"
		chmod +x "$stage/$top/$(basename "$f")"
	done
	for f in $EXTRAS; do
		cp "$f" "$stage/$top/"
	done
	tar czf "$TARBALL" -C "$stage" "$top"
	rm -rf "$stage"
	echo "    contents:"; tar tzf "$TARBALL" | sed 's/^/      /'
fi

echo
echo "Packaged irixscsitb $VERSION:"
ls -la "$OUTDIR"/irixscsitb-"$VERSION".* 2>/dev/null || true
echo "Note: EFS stores Unix mode bits - the .iso/.hda land irixscsitb 0644, so on"
echo "IRIX run 'chmod +x irixscsitb' after copying it off the media (the .tar.gz"
echo "already carries the executable bit)."
