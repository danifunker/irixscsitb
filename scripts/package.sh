#!/bin/sh
# Package the IRIX irixscsitb build products into distributable artifacts with
# rb-cli (https://github.com/danifunker/rusty-backup). Produces, per release:
#
#   irixscsitb-VER.iso.gz    IRIX EFS CD-ROM image, gzipped for distribution
#   irixscsitb-VER.hda.gz    SGI EFS hard-disk image, gzipped (mostly empty
#                            space compresses to almost nothing)
#   irixscsitb-VER.tar.gz    the same tree + raw binaries, executable bits set
#   irixscsitb-VER-53.tardist     Software Manager package (o32/mips2, 5.3 format)
#   irixscsitb-VER-mips1.tardist  Software Manager package (o32/mips1, 5.3 format)
#   irixscsitb-VER-65.tardist     Software Manager package (n32, 6.5 format)
#
# The raw .iso/.hda stay in --outdir next to the .gz for local use (attach
# directly in IRIS; `gunzip` before writing to real media).
#
# MEDIA LAYOUT — one directory per flavor, each packaged BY ITS OWN OS
# (iris-build.sh runs the guest's native gendist in the same session):
#   /dist53/     inst distribution from the IRIX 5.3 guest (o32/mips2; the
#                5.3-format product every inst 5.3-6.5 reads, R4000 and up):
#                inst -f /CDROM/dist53
#   /distmips1/  the same, built o32/mips1 for ANY MIPS CPU incl. the R3000:
#                inst -f /CDROM/distmips1
#   /dist65/     inst distribution from the IRIX 6.5 guest (n32, 6.5 format):
#                inst -f /CDROM/dist65
#   /README-dist.txt  generated: which directory is which
# When a flavor has no inst product (guest without the Software Packager),
# its raw binaries take the directory's place instead — copy off + chmod +x.
#
# Usage:
#   scripts/package.sh --version VER [options]
#
# Options (defaults in brackets):
#   --inst53-dir DIR  o32 product trio (irixscsitb, .idb, .sw) -> /dist53
#   --instmips1-dir DIR  mips1 product trio -> /distmips1
#   --inst65-dir DIR  n32 product trio -> /dist65
#   --bin53 PATH      o32 CLI: tarball bin53/ (+ /dist53 fallback w/o inst)
#   --gui53 PATH      o32 GUI: tarball bin53/ (+ fallback)
#   --binmips1 PATH   mips1 CLI: tarball binmips1/ (+ /distmips1 fallback)
#   --guimips1 PATH   mips1 GUI: tarball binmips1/ (+ fallback)
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

# Per-flavor inputs, keyed by the dist key (scripts/ci-lib.sh): BIN<key>,
# GUI<key>, INST<key>. KEYS is also the order on the media.
KEYS="53 mips1 65"
BIN53=""; GUI53=""; INST53=""
BINmips1=""; GUImips1=""; INSTmips1=""
BIN65=""; GUI65=""; INST65=""
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
		--instmips1-dir) INSTmips1="$2"; shift 2 ;;
		--bin53)    BIN53="$2"; shift 2 ;;
		--gui53)    GUI53="$2"; shift 2 ;;
		--binmips1) BINmips1="$2"; shift 2 ;;
		--guimips1) GUImips1="$2"; shift 2 ;;
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
		-h|--help)  sed -n '2,60p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

# host_tar ARGS — tar as IRIX can read it. Plain ustar: macOS bsdtar otherwise
# writes pax headers carrying com.apple.provenance xattrs, which IRIX's tar
# unpacks as a junk "PaxHeader" entry (seen on an R3000 Indigo, 2026-10-01 -
# Linux CI tars were clean, release-local.sh ones were not). COPYFILE_DISABLE
# keeps macOS from adding ._ AppleDouble files. Both are no-ops for GNU tar.
host_tar() { COPYFILE_DISABLE=1 tar --format=ustar "$@"; }

# key_get VAR KEY — the value of ${VAR}${KEY} (BIN53, INSTmips1, ...).
key_get() { eval "printf %s \"\${$1$2}\""; }

[ -n "$VERSION" ] || die "missing --version"
ANY=""
for k in $KEYS; do
	for f in "$(key_get BIN "$k")" "$(key_get GUI "$k")"; do
		[ -z "$f" ] || [ -f "$f" ] || die "not found: $f"
	done
	d=$(key_get INST "$k")
	ANY="$ANY$d$(key_get BIN "$k")"
	[ -z "$d" ] && continue
	for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
		[ -f "$d/$f" ] || die "inst dir $d is missing $f (iris-build.sh emits it unless --no-gendist)"
	done
done
[ -n "$ANY" ] || die "nothing to package: pass --inst<key>-dir and/or --bin<key> for at least one of: $KEYS"

# key_runs_on / key_build KEY — the README-dist lines for that directory.
key_runs_on() {
	case "$1" in
		53)    echo "IRIX 5.3-6.5 on an R4000 or later" ;;
		mips1) echo "IRIX 5.3-6.5 on ANY MIPS CPU, incl. the R2000/R3000" ;;
		65)    echo "IRIX 6.x only (fastest)" ;;
	esac
}
key_build() {
	case "$1" in
		53)    echo "o32 ABI, MIPS II - compiled and packaged on IRIX 5.3" ;;
		mips1) echo "o32 ABI, MIPS I - compiled and packaged on IRIX 5.3" ;;
		65)    echo "n32 ABI, MIPS III - compiled and packaged on IRIX 6.5" ;;
	esac
}

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
	for k in $KEYS; do
		if [ -n "$(key_get INST "$k")" ]; then
			_how="inst -f /CDROM/dist$k   (or swmgr)"
		elif [ -n "$(key_get BIN "$k")" ]; then
			_how="raw binaries: copy off + chmod +x"
		else
			continue
		fi
		echo "dist$k/"
		echo "    runs on:  $(key_runs_on "$k")"
		echo "    build:    $(key_build "$k")"
		echo "    install:  $_how"
		echo ""
	done
	echo "Which one? An R2000/R3000 machine (IP12 Indigo and other early"
	echo "systems; 'hinv' prints the CPU) needs distmips1 - it cannot run the"
	echo "others. Any other machine on IRIX 5.3-6.5 takes dist53, and IRIX 6.x"
	echo "may take dist65 instead."
	echo ""
	echo "Each product installs /usr/sbin/irixscsitb (CLI) and, where the"
	echo "build had Motif, /usr/sbin/scsitbgui (GUI). Installing another"
	echo "flavor's product later simply replaces it."
} > "$README_DIST"

PAYLOAD=""
add_payload() { PAYLOAD="$PAYLOAD$1|$2
"; }
# dist<key>: the inst product, or raw binaries when no product was generated.
DIRS=""
for k in $KEYS; do
	inst=$(key_get INST "$k"); bin=$(key_get BIN "$k"); gui=$(key_get GUI "$k")
	if [ -n "$inst" ]; then
		for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
			add_payload "$inst/$f" "/dist$k/$f"
		done
	elif [ -n "$bin" ]; then
		add_payload "$bin" "/dist$k/irixscsitb"
		[ -z "$gui" ] || add_payload "$gui" "/dist$k/scsitbgui"
	else
		continue
	fi
	DIRS="$DIRS /dist$k"
done
add_payload "$README_DIST" "/README-dist.txt"
for f in $EXTRAS; do
	add_payload "$f" "/$(basename "$f")"
done

# put_payload <image-ref> : create the flavor dirs and drop every manifest
# file. <image-ref> addresses the EFS partition as "@1" for both the CD
# (slot 7) and the HDD (slot 0) — rb-cli maps @1 to the sole EFS partition.
put_payload() {
	ref="$1"
	for d in $DIRS; do
		"$RB" mkdir "$ref" "$d"
	done
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
	for k in $KEYS; do
		for pair in "irixscsitb:$(key_get BIN "$k")" "scsitbgui:$(key_get GUI "$k")"; do
			n=${pair%%:*}; src=${pair#*:}
			[ -n "$src" ] || continue
			mkdir -p "$stage/$top/bin$k"
			cp "$src" "$stage/$top/bin$k/$n"
			chmod +x "$stage/$top/bin$k/$n"
		done
	done
	host_tar -czf "$TARBALL" -C "$stage" "$top"
	rm -rf "$stage"
	echo "    contents:"; tar tzf "$TARBALL" | sed 's/^/      /'
fi

# Per-flavor tardists: a plain tar of the product trio — the classic
# "download and open with Software Manager" vector.
for k in $KEYS; do
	inst=$(key_get INST "$k")
	[ -n "$inst" ] || continue
	echo ">>> tardist ($k): $OUTDIR/irixscsitb-$VERSION-$k.tardist"
	( cd "$inst" && host_tar -cf "$OUTDIR/irixscsitb-$VERSION-$k.tardist" irixscsitb irixscsitb.idb irixscsitb.sw )
done

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
