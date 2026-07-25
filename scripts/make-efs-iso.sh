#!/bin/sh
# Turn a built package tarball into an IRIX EFS CD-ROM image (and optionally an
# SGI hard-disk image) with rb-cli.
#
# Runs on the HOST, not on IRIX - rb-cli is a host tool. Feed it the .tar that
# build.sh leaves in output/.
#
#   scripts/make-efs-iso.sh --tar output/irixscsitb-<rev>.tar
#   scripts/make-efs-iso.sh --tar T.tar --out mydisc.iso --size 8M --name SCSITB
#   scripts/make-efs-iso.sh --tar T.tar --hdd          # also build a .hda
#
# WHAT LANDS ON THE DISC: just the tarball.
#
#   /irixscsitb-<rev>.tar
#
# One file, extracted on IRIX with a bare `tar xvf`. That is deliberate, not a
# workaround: tar carries modes in its own headers, so everything comes out
# executable with no chmod, and there is exactly one thing to copy.
#
# It is NOT because EFS cannot hold an execute bit - it can. `rb-cli put` of an
# 0755 host file lands 0755 in the image, verified. What does not work today is
# `rb-cli untar`, which drops the mode from the tar header and writes 0644; that
# is why importing the extracted tree here would give a browsable but unrunnable
# copy. See docs in CLAUDE.md for the repro.
#
# Differs from scripts/package.sh, which builds release artifacts around a bare
# binary. This one just takes whatever build.sh packaged and wraps it in EFS.

set -eu

RB="${RB_CLI:-rb-cli}"
TARFILE=""
OUT=""
SIZE="8M"
HDD_SIZE="50M"
NAME="SCSITB"
WANT_HDD=0

die() { echo "make-efs-iso: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--tar)     TARFILE="$2"; shift 2 ;;
		--out)     OUT="$2"; shift 2 ;;
		--size)    SIZE="$2"; shift 2 ;;
		--hdd-size) HDD_SIZE="$2"; shift 2 ;;
		--name)    NAME="$2"; shift 2 ;;
		--rb-cli)  RB="$2"; shift 2 ;;
		--hdd)     WANT_HDD=1; shift ;;
		-h|--help) sed -n '2,26p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

[ -n "$TARFILE" ] || die "missing --tar (the .tar from output/); see --help"
[ -f "$TARFILE" ] || die "no such file: $TARFILE"

# EFS volume labels are 6 bytes. rb-cli would complain, but saying so here is
# friendlier than a failure four steps in.
case "$NAME" in
	??????*) [ ${#NAME} -le 6 ] || die "--name must be 6 characters or fewer (got '$NAME')" ;;
esac

command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || \
	die "rb-cli not found - set RB_CLI=/path or pass --rb-cli"
"$RB" optical new sgi-efs --help >/dev/null 2>&1 || \
	die "this rb-cli lacks 'optical new sgi-efs' - update it (the old 'new-sgi-cdrom' verb was renamed)"
"$RB" untar --help >/dev/null 2>&1 || \
	die "this rb-cli lacks 'untar' - update it"

TARBASE=`basename "$TARFILE"`
PKGDIR=`basename "$TARBASE" .tar`   # the tarball's single root directory
if [ -z "$OUT" ]; then
	OUT=`dirname "$TARFILE"`/`basename "$TARBASE" .tar`.iso
fi

build_one() {
	# $1 = image path, $2 = human label for messages
	img="$1"
	echo ""
	echo ">>> populating $2: $img"
	"$RB" put "$img@1" "$TARFILE" "/$TARBASE"
	"$RB" fsck "$img@1"
	echo "--- contents:"
	"$RB" ls -o "$img@1" /
}

rm -f "$OUT"
echo ">>> creating EFS CD-ROM image: $OUT ($SIZE, label $NAME)"
"$RB" optical new sgi-efs "$OUT" --size "$SIZE" --name "$NAME"
build_one "$OUT" "CD-ROM"

if [ "$WANT_HDD" -eq 1 ]; then
	HDA=`dirname "$OUT"`/`basename "$OUT" .iso`.hda
	rm -f "$HDA"
	echo ""
	echo ">>> creating SGI EFS hard-disk image: $HDA ($HDD_SIZE, label $NAME)"
	# 16 heads / 63 sectors matches what the IRIS emulator expects.
	"$RB" new hd sgi-efs "$HDA" --size "$HDD_SIZE" --name "$NAME" --heads 16 --sectors 63
	build_one "$HDA" "hard disk"
fi

cat <<EOF

============================================================
Done: $OUT
EOF
[ "$WANT_HDD" -eq 1 ] && echo "      $HDA"
cat <<EOF

In IRIS, attach the .iso as a CD:      [scsi.N] path = "...", cdrom = true
              or the .hda as a disk:   [scsi.N] path = "...", cdrom = false

On IRIX, mount and unpack. tar restores the execute bits, so there is no chmod
step:

    mkdir -p /CDROM
    mount -t efs -o ro /dev/dsk/dks0d<N>s7 /CDROM
    cd /usr/tmp
    tar xvf /CDROM/$TARBASE
    cd $PKGDIR
    ./install.sh
============================================================
EOF
