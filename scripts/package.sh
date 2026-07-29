#!/bin/sh
# Package the IRIX irixscsitb build products into distributable artifacts with
# rb-cli (https://github.com/danifunker/rusty-backup). Produces, per release:
#
#   irixscsitb-VER.iso.gz    IRIX EFS CD-ROM image, gzipped for distribution
#   irixscsitb-VER.hda.gz    SGI EFS hard-disk image, gzipped (mostly empty
#                            space compresses to almost nothing)
#   irixscsitb-VER.tar.gz    the same tree + raw binaries, executable bits set
#   irixscsitb-VER-53.tardist  Software Manager package (o32, 5.3 format)
#   irixscsitb-VER-65.tardist  Software Manager package (n32, 6.5 format)
#
# The raw .iso/.hda stay in --outdir next to the .gz for local use (attach
# directly in IRIS; `gunzip` before writing to real media).
#
# MEDIA LAYOUT — one directory per flavor, each packaged BY ITS OWN OS
# (iris-build.sh runs the guest's native gendist in the same session):
#   /dist53/   inst distribution from the IRIX 5.3 guest (o32; the 5.3-format
#              product every inst 5.3-6.5 reads):  inst -f /CDROM/dist53
#   /dist65/   inst distribution from the IRIX 6.5 guest (n32, 6.5 format):
#              inst -f /CDROM/dist65
#   /README-dist.txt  generated: which directory is which
# When a flavor has no inst product (guest without the Software Packager),
# its raw binaries take the directory's place instead — copy off + chmod +x.
#
# Usage:
#   scripts/package.sh --version VER [options]
#
# Options (defaults in brackets):
#   --inst53-dir DIR  o32 product trio (irixscsitb, .idb, .sw) -> /dist53
#   --inst65-dir DIR  n32 product trio -> /dist65
#   --bin53 PATH      o32 CLI: tarball bin53/ (+ /dist53 fallback w/o inst)
#   --gui53 PATH      o32 GUI: tarball bin53/ (+ fallback)
#   --bin65 PATH      n32 CLI: tarball bin65/ (+ /dist65 fallback w/o inst)
#   --gui65 PATH      n32 GUI: tarball bin65/ (+ fallback)
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
#   --no-gzip         skip gzipping the .iso/.hda
set -eu

BIN53=""
GUI53=""
BIN65=""
GUI65=""
INST53=""
INST65=""
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
DO_GZIP=1

die() { echo "package: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--inst53-dir) INST53="$2"; shift 2 ;;
		--inst65-dir) INST65="$2"; shift 2 ;;
		--bin53)    BIN53="$2"; shift 2 ;;
		--gui53)    GUI53="$2"; shift 2 ;;
		--bin65)    BIN65="$2"; shift 2 ;;
		--gui65)    GUI65="$2"; shift 2 ;;
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
		--no-gzip)  DO_GZIP=0; shift ;;
		-h|--help)  sed -n '2,52p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
for f in "$BIN53" "$GUI53" "$BIN65" "$GUI65"; do
	[ -z "$f" ] || [ -f "$f" ] || die "not found: $f"
done
for d in "$INST53" "$INST65"; do
	[ -z "$d" ] && continue
	for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
		[ -f "$d/$f" ] || die "inst dir $d is missing $f (iris-build.sh emits it unless --no-gendist)"
	done
done
[ -n "$INST53$BIN53$INST65$BIN65" ] || die "nothing to package: pass --inst53-dir/--bin53 and/or --inst65-dir/--bin65"

# Fail early (and clearly) if this rb-cli predates the current builder grammar.
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"
if [ "$DO_ISO" = 1 ]; then
	"$RB" optical new sgi-efs --help >/dev/null 2>&1 || \
		die "this rb-cli lacks 'optical new sgi-efs' (update rb-cli)"
fi
if [ "$DO_HDA" = 1 ]; then
	"$RB" new hd sgi-efs --help >/dev/null 2>&1 || \
		die "this rb-cli lacks 'new hd sgi-efs' (update rb-cli)"
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
	if [ -n "$INST53" ]; then
		echo "dist53/   Software Manager distribution, built and packaged ON"
		echo "          IRIX 5.3 (o32 - runs on 5.3 through 6.5):"
		echo "              inst -f /CDROM/dist53     (or swmgr)"
	elif [ -n "$BIN53" ]; then
		echo "dist53/   o32 binaries (run on IRIX 5.3-6.5): copy off + chmod +x"
	fi
	if [ -n "$INST65" ]; then
		echo "dist65/   Software Manager distribution, built and packaged ON"
		echo "          IRIX 6.5 (n32 - IRIX 6.x only, faster):"
		echo "              inst -f /CDROM/dist65     (or swmgr)"
	elif [ -n "$BIN65" ]; then
		echo "dist65/   n32 binaries (IRIX 6.x only): copy off + chmod +x"
	fi
	echo ""
	echo "Each product installs /usr/sbin/irixscsitb (CLI) and, where the"
	echo "build had Motif, /usr/sbin/scsitbgui (GUI). Installing the other"
	echo "flavor's product later simply replaces it."
} > "$README_DIST"

PAYLOAD=""
add_payload() { PAYLOAD="$PAYLOAD$1|$2
"; }
# dist53: the inst product, or raw binaries when no product was generated.
if [ -n "$INST53" ]; then
	for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
		add_payload "$INST53/$f" "/dist53/$f"
	done
elif [ -n "$BIN53" ]; then
	add_payload "$BIN53" "/dist53/irixscsitb"
	[ -z "$GUI53" ] || add_payload "$GUI53" "/dist53/scsitbgui"
fi
if [ -n "$INST65" ]; then
	for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
		add_payload "$INST65/$f" "/dist65/$f"
	done
elif [ -n "$BIN65" ]; then
	add_payload "$BIN65" "/dist65/irixscsitb"
	[ -z "$GUI65" ] || add_payload "$GUI65" "/dist65/scsitbgui"
fi
add_payload "$README_DIST" "/README-dist.txt"
for f in $EXTRAS; do
	add_payload "$f" "/$(basename "$f")"
done

# put_payload <image-ref> : create the flavor dirs and drop every manifest
# file. <image-ref> addresses the EFS partition as "@1" for both the CD
# (slot 7) and the HDD (slot 0) — rb-cli maps @1 to the sole EFS partition.
put_payload() {
	ref="$1"
	[ -z "$INST53$BIN53" ] || "$RB" mkdir "$ref" /dist53
	[ -z "$INST65$BIN65" ] || "$RB" mkdir "$ref" /dist65
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
	# The media tree verbatim...
	printf '%s' "$PAYLOAD" | while IFS='|' read -r host guest; do
		[ -n "$host" ] || continue
		case "$guest" in
			/README-dist.txt) dest="$top/README-dist.txt" ;;
			*)                dest="$top$guest" ;;
		esac
		mkdir -p "$stage/$(dirname "$dest")"
		cp "$host" "$stage/$dest"
	done
	# ...plus the raw binaries with executable bits, for NFS/direct-copy use.
	for pair in "bin53:$BIN53" "bin53:$GUI53" "bin65:$BIN65" "bin65:$GUI65"; do
		d=${pair%%:*}; src=${pair#*:}
		[ -n "$src" ] || continue
		mkdir -p "$stage/$top/$d"
		case "$src" in *scsitbgui*) n=scsitbgui ;; *) n=irixscsitb ;; esac
		cp "$src" "$stage/$top/$d/$n"
		chmod +x "$stage/$top/$d/$n"
	done
	tar czf "$TARBALL" -C "$stage" "$top"
	rm -rf "$stage"
	echo "    contents:"; tar tzf "$TARBALL" | sed 's/^/      /'
fi

# Per-flavor tardists: a plain tar of the product trio — the classic
# "download and open with Software Manager" vector.
if [ -n "$INST53" ]; then
	echo ">>> tardist (o32/5.3): $OUTDIR/irixscsitb-$VERSION-53.tardist"
	( cd "$INST53" && tar cf "$OUTDIR/irixscsitb-$VERSION-53.tardist" irixscsitb irixscsitb.idb irixscsitb.sw )
fi
if [ -n "$INST65" ]; then
	echo ">>> tardist (n32/6.5): $OUTDIR/irixscsitb-$VERSION-65.tardist"
	( cd "$INST65" && tar cf "$OUTDIR/irixscsitb-$VERSION-65.tardist" irixscsitb irixscsitb.idb irixscsitb.sw )
fi

# Distribution compression: the images are mostly empty space. The raw files
# stay for direct local use (IRIS attaches them as-is).
if [ "$DO_GZIP" = 1 ]; then
	[ "$DO_ISO" = 1 ] && { echo ">>> gzip: $ISO_IMG.gz"; gzip -9 -c "$ISO_IMG" > "$ISO_IMG.gz"; }
	[ "$DO_HDA" = 1 ] && { echo ">>> gzip: $HDD_IMG.gz"; gzip -9 -c "$HDD_IMG" > "$HDD_IMG.gz"; }
fi

rm -f "$README_DIST"
echo
echo "Packaged irixscsitb $VERSION:"
ls -la "$OUTDIR"/irixscsitb-"$VERSION"* 2>/dev/null || true
echo "Note: the .gz images are the distribution artifacts (gunzip before"
echo "writing to real media; IRIS can attach the raw .iso/.hda directly)."
