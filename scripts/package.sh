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
# Both flavors ride on every medium, in per-flavor directories so nobody has
# to guess which binary they are looking at:
#   /dist53/irixscsitb [+ scsitbgui]   o32/mips2 — runs on IRIX 5.3 through 6.5
#   /dist65/irixscsitb [+ scsitbgui]   n32/mips3 — IRIX 6.x only, faster
#   /README-dist.txt                   generated: which directory is which
# The .tar.gz carries the same dist53/ + dist65/ tree.
#
# Usage:
#   scripts/package.sh --dist53-bin PATH --dist65-bin PATH --version VER [options]
#
# Options (defaults in brackets):
#   --dist53-bin PATH  o32 CLI  -> dist53/irixscsitb   (at least one of the
#   --dist65-bin PATH  n32 CLI  -> dist65/irixscsitb    two CLIs is required)
#   --dist53-gui PATH  o32 GUI  -> dist53/scsitbgui    (optional)
#   --dist65-gui PATH  n32 GUI  -> dist65/scsitbgui    (optional)
#   --version VER     version string used in output filenames (required)
#   --outdir DIR      where to write the artifacts       [dist]
#   --rb-cli PATH     rb-cli binary    [$RB_CLI, then `rb-cli` on PATH]
#   --name LABEL      EFS volume label, max 6 bytes       [SCSITB]
#   --extra PATH      extra file at image root + tarball top (repeatable)
#   --cd-size SIZE    EFS CD image size                   [8M]
#   --hdd-size SIZE   SGI HDD image size                  [50M]
#   --heads N         HDD geometry heads (IRIS = 16)      [16]
#   --sectors N       HDD geometry sectors/track (IRIS = 63) [63]
#   --no-iso          skip the .iso
#   --no-hda          skip the .hda
#   --no-tar          skip the .tar.gz
set -eu

BIN53=""
GUI53=""
BIN65=""
GUI65=""
VERSION=""
OUTDIR="dist"
RB="${RB_CLI:-rb-cli}"
NAME="SCSITB"
CD_SIZE="8M"
HDD_SIZE="50M"
HEADS="16"
SECTORS="63"
EXTRAS=""
DO_ISO=1
DO_HDA=1
DO_TAR=1

die() { echo "package: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dist53-bin) BIN53="$2"; shift 2 ;;
		--dist53-gui) GUI53="$2"; shift 2 ;;
		--dist65-bin) BIN65="$2"; shift 2 ;;
		--dist65-gui) GUI65="$2"; shift 2 ;;
		--version)  VERSION="$2"; shift 2 ;;
		--outdir)   OUTDIR="$2"; shift 2 ;;
		--rb-cli)   RB="$2"; shift 2 ;;
		--name)     NAME="$2"; shift 2 ;;
		--extra)    EXTRAS="$EXTRAS $2"; shift 2 ;;
		--cd-size)  CD_SIZE="$2"; shift 2 ;;
		--hdd-size) HDD_SIZE="$2"; shift 2 ;;
		--heads)    HEADS="$2"; shift 2 ;;
		--sectors)  SECTORS="$2"; shift 2 ;;
		--no-iso)   DO_ISO=0; shift ;;
		--no-hda)   DO_HDA=0; shift ;;
		--no-tar)   DO_TAR=0; shift ;;
		-h|--help)  sed -n '2,50p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
[ -n "$BIN53" ] || [ -n "$BIN65" ] || die "need --dist53-bin and/or --dist65-bin"
for f in "$BIN53" "$GUI53" "$BIN65" "$GUI65"; do
	[ -z "$f" ] || [ -f "$f" ] || die "not found: $f"
done
[ -z "$GUI53" ] || [ -n "$BIN53" ] || die "--dist53-gui without --dist53-bin"
[ -z "$GUI65" ] || [ -n "$BIN65" ] || die "--dist65-gui without --dist65-bin"
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

# The payload manifest: "host-path|guest-path" lines, one per file. Built
# once, used by the image populate, the round-trip verify, AND the tarball,
# so the three can never disagree about what ships.
README_DIST="$OUTDIR/.README-dist.$$"
{
	echo "irixscsitb $VERSION - toolbox for BlueSCSI / ZuluSCSI on SGI IRIX"
	echo ""
	[ -z "$BIN53" ] || echo "dist53/   o32 (mips2) binaries: run on IRIX 5.3 through 6.5"
	[ -z "$BIN65" ] || echo "dist65/   n32 (mips3) binaries: IRIX 6.x ONLY, faster"
	echo ""
	echo "Each directory holds irixscsitb (the CLI) and, where the build had"
	echo "Motif, scsitbgui (the GUI). EFS media store files mode 0644: after"
	echo "copying off the CD/disk, chmod +x the binaries. The .tar.gz carries"
	echo "the executable bits already."
} > "$README_DIST"

PAYLOAD=""
add_payload() { PAYLOAD="$PAYLOAD$1|$2
"; }
[ -z "$BIN53" ] || add_payload "$BIN53" "/dist53/irixscsitb"
[ -z "$GUI53" ] || add_payload "$GUI53" "/dist53/scsitbgui"
[ -z "$BIN65" ] || add_payload "$BIN65" "/dist65/irixscsitb"
[ -z "$GUI65" ] || add_payload "$GUI65" "/dist65/scsitbgui"
add_payload "$README_DIST" "/README-dist.txt"
for f in $EXTRAS; do
	add_payload "$f" "/$(basename "$f")"
done

# put_payload <image-ref> : create the flavor dirs and drop every manifest
# file. <image-ref> addresses the EFS partition as "@1" for both the CD
# (slot 7) and the HDD (slot 0) — rb-cli maps @1 to the sole EFS partition.
put_payload() {
	ref="$1"
	[ -z "$BIN53" ] || "$RB" mkdir "$ref" /dist53
	[ -z "$BIN65" ] || "$RB" mkdir "$ref" /dist65
	printf '%s' "$PAYLOAD" | while IFS='|' read -r host guest; do
		[ -n "$host" ] || continue
		"$RB" put "$ref" "$host" "$guest"
	done
}

# Round-trip: every manifest file read back must match its source.
verify_roundtrip() {
	ref="$1"; label="$2"
	tmpd="$(mktemp -d)"
	printf '%s' "$PAYLOAD" | while IFS='|' read -r host guest; do
		[ -n "$host" ] || continue
		"$RB" -q get "$ref" "$guest" "$tmpd/rt"
		cmp -s "$host" "$tmpd/rt" || { echo "MISMATCH $guest" > "$tmpd/fail"; break; }
		rm -f "$tmpd/rt"
	done
	[ ! -f "$tmpd/fail" ] || { read -r m < "$tmpd/fail"; rm -rf "$tmpd"; die "$label round-trip $m"; }
	rm -rf "$tmpd"
	echo "    $label round-trip OK (all files)"
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
	# Same dist53/ + dist65/ tree as the media, with the executable bits set,
	# so `tar xf` yields ready-to-run binaries (the EFS images land 0644 —
	# chmod +x after copying off).
	printf '%s' "$PAYLOAD" | while IFS='|' read -r host guest; do
		[ -n "$host" ] || continue
		case "$guest" in
			/README-dist.txt) dest="$top/README-dist.txt" ;;
			*)                dest="$top$guest" ;;
		esac
		mkdir -p "$stage/$(dirname "$dest")"
		cp "$host" "$stage/$dest"
		case "$guest" in /dist*/*) chmod +x "$stage/$dest" ;; esac
	done
	tar czf "$TARBALL" -C "$stage" "$top"
	rm -rf "$stage"
	echo "    contents:"; tar tzf "$TARBALL" | sed 's/^/      /'
fi

rm -f "$README_DIST"
echo
echo "Packaged irixscsitb $VERSION:"
ls -la "$OUTDIR"/irixscsitb-"$VERSION".* 2>/dev/null || true
echo "Note: EFS stores Unix mode bits - the .iso/.hda land the binaries 0644, so"
echo "on IRIX run 'chmod +x' after copying them off the media (the .tar.gz"
echo "already carries the executable bits). See README-dist.txt on the media"
echo "for which directory (dist53/dist65) fits which IRIX."
