#!/bin/sh
# irix-native-build.sh - build irixscsitb natively on IRIX, NFS-safe, logged.
#
# Shipped into the IRIS/IRIX drop folder as "build.sh" by
# scripts/sync-irix-drop.sh, which is the only supported way to assemble that
# folder. Edit it HERE, in the repo - the copy in the drop is disposable.
#
# Run this INSIDE IRIX (5.3 through 6.5), as root or a user with cc.
#
# HOW IT WORKS: getcwd(2) does not work inside the IRIS built-in NFS mount on
# IRIX 5.3 (`pwd` and `make` both die with "getcwd (bu5).").  So this script does
# ALL the work - copy sources, compile, log - in a stable LOCAL scratch dir in
# your home ($HOME/irixscsitb-build), where getcwd works.  Only at the very end does it
# copy the finished binary and the build log back onto the NFS share (this
# folder) so you can grab them from the host.  The share is only ever touched
# with plain relative/absolute paths (cp), which don't need getcwd.
#
# Written for the IRIX 5.3 /bin/sh (Bourne shell): backticks, no $( ), no
# arithmetic, no 'local', no pipefail, NO 'pwd'/getcwd, and NO '~' (the old
# Bourne shell doesn't expand tilde - we use $HOME).
#
#   ./build.sh            # portable o32/mips2 build (runs on 5.3 - 6.5)
#   ./build.sh irix-n32   # faster 6.x-only n32 build
#   NOGUI=1 ./build.sh    # skip the Motif GUI, build only the CLI
#   BUILDDIR=$HOME/foo ./build.sh    # override the local scratch dir
#
# Builds TWO binaries: irixscsitb (the CLI, no X dependency) and scsitbgui
# (the IRIS IM / Motif GUI).  The GUI is treated as OPTIONAL - if it fails to
# compile or link (no Motif dev installed, say) the script says so and still
# delivers the CLI, because the CLI is the thing you cannot do without.
#
# Everything finished lands in an output/ subfolder of this (NFS) share, so the
# host can pick the whole thing up in one go (.gitignore'd, so it is safe even
# if the repo itself is the share):
#
#   output/irixscsitb                the CLI binary
#   output/scsitbgui                 the Motif GUI binary (if it built)
#   output/install.sh                installer - run as root on the target
#   output/uninstall.sh              removes everything the installer added
#   output/desktop/                  desktop icon rules the installer needs
#   output/README.md
#   output/irixscsitb-<rev>.tar      all of the above (plain tar: IRIX 5.3
#                                    tar cannot decompress, so no gzip)
#   output/build-<stamp>.log         full log (+ build-latest.log)
#
# The same set is staged on LOCAL disk at
# $HOME/irixscsitb-build/irixscsitb-<rev>/, and that is the copy to install
# from: an NFS share generally will not keep an execute bit on a file the guest
# created, so output/install.sh often ends up non-executable even though the
# script chmods it. output/ is for the host to pick up; the local copy is for
# running.

# Where the sources + this script live (the NFS share).  Taken from $0 - do NOT
# pwd it (getcwd fails over NFS).  May be relative (e.g. "."), which is fine:
# this shell never changes directory, so relative paths stay valid throughout.
SRCDIR=`dirname "$0"`
TARGET=${1:-irix-o32}

# Matching GUI target for whichever ABI was asked for.  'case' is fine in the
# 5.3 Bourne shell; ${x/y} substitution is not.
case "$TARGET" in
	irix-n32) GUITARGET=irix-gui-n32 ;;
	*)        GUITARGET=irix-gui-o32 ;;
esac

# Stable local scratch dir in home (same one every run).  ${x:-y} also covers
# the empty case; if $HOME is unset this falls back to /irixscsitb-build (still local).
BUILDDIR=${BUILDDIR:-$HOME/irixscsitb-build}

STAMP=`date +%Y%m%d-%H%M%S`

# --- 1. fresh local scratch dir; the log lives here during the build --------
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
LOG="$BUILDDIR/build-$STAMP.log"

echo "=== irixscsitb IRIX build ==="   | tee "$LOG"
echo "date    : `date`"               | tee -a "$LOG"
echo "uname   : `uname -a`"           | tee -a "$LOG"
echo "srcdir  : $SRCDIR (NFS share)"  | tee -a "$LOG"
echo "builddir: $BUILDDIR (local home - NFS-safe)" | tee -a "$LOG"
echo "target  : $TARGET"              | tee -a "$LOG"
echo "log     : $LOG"                 | tee -a "$LOG"
echo "----------------------------------------" | tee -a "$LOG"

echo ">>> copying sources to $BUILDDIR" | tee -a "$LOG"
cp "$SRCDIR/irixscsitb.c" "$SRCDIR/toolbox.c" "$SRCDIR/wifi.c" \
   "$SRCDIR/gui_motif.c" "$SRCDIR/version.c" "$SRCDIR/irix.c" \
   "$SRCDIR/irixscsitb.h" "$SRCDIR/os.h" "$SRCDIR/Makefile" \
   "$BUILDDIR"/ 2>&1 | tee -a "$LOG"
# README is optional; if present it gets bundled into the .tar.gz by 'make tar'.
[ -f "$SRCDIR/README.md" ] && cp "$SRCDIR/README.md" "$BUILDDIR"/ 2>/dev/null

# version.h is a shipped INPUT, not something built here: it is stamped on a
# host that has git (see scripts/sync-irix-drop.sh) and compiled as-is. Nothing
# on this machine regenerates it - IRIX has no git, so a "regeneration" here
# could only ever downgrade a good revision to "unknown". Because the file
# already exists, make's version.h rule never fires.
if [ -f "$SRCDIR/version.h" ]; then
	cp "$SRCDIR/version.h" "$BUILDDIR"/ 2>/dev/null
	grep BUILD_REV "$SRCDIR/version.h" | tee -a "$LOG"
else
	echo "WARN: no version.h shipped - re-run scripts/sync-irix-drop.sh on the host." | tee -a "$LOG"
fi

# The revision names the staging directory and the tarball, so unpacking gives
# you a folder you can identify months later rather than a generic "pkg".
REV=`sed -n 's/^#define BUILD_REV  *"\(.*\)"/\1/p' "$SRCDIR/version.h" 2>/dev/null`
if [ -z "$REV" ]; then
	REV=unknown
fi

# Carried only as a safety net: if version.h is missing entirely, this lets the
# Makefile write an "unknown" stub instead of failing outright.
mkdir -p "$BUILDDIR/scripts" 2>/dev/null
cp "$SRCDIR/scripts/mkversion.sh" "$BUILDDIR/scripts"/ 2>/dev/null

# Shipped alongside the binaries rather than built: the installer and the
# desktop icon rules it puts in place.
mkdir -p "$BUILDDIR/desktop/iconlib" 2>/dev/null
cp "$SRCDIR/install.sh" "$BUILDDIR"/ 2>/dev/null
cp "$SRCDIR/uninstall.sh" "$BUILDDIR"/ 2>/dev/null
cp "$SRCDIR/desktop/scsitbgui.ftr" "$BUILDDIR/desktop"/ 2>/dev/null
cp "$SRCDIR/desktop/iconlib/scsitbgui.fti" "$BUILDDIR/desktop/iconlib"/ 2>/dev/null

# --- 2. build locally (getcwd + make work on local disk) --------------------
echo ">>> building CLI ($TARGET) in $BUILDDIR" | tee -a "$LOG"
( cd "$BUILDDIR" && make "$TARGET" ) 2>&1 | tee -a "$LOG"

# The Motif GUI is a separate link of the same objects.  Deliberately after the
# CLI and deliberately non-fatal: a missing libXm must not cost you the CLI.
if [ -n "$NOGUI" ]; then
	echo ">>> skipping GUI (NOGUI set)" | tee -a "$LOG"
else
	echo "----------------------------------------" | tee -a "$LOG"
	echo ">>> building Motif GUI ($GUITARGET)" | tee -a "$LOG"
	( cd "$BUILDDIR" && make "$GUITARGET" ) 2>&1 | tee -a "$LOG"
fi

echo "----------------------------------------" | tee -a "$LOG"

# --- 3. stage everything, tar it, copy into <share>/output/ -----------------
# Success is decided by the binary existing, not by the pipe's exit status
# (Bourne sh reports tee's status after a pipeline, not make's).
OUT="$SRCDIR/output"        # finished artifacts on the share (.gitignore'd)
PKG="$BUILDDIR/irixscsitb-$REV"   # staging dir = the tarball's single root
mkdir -p "$OUT" 2>/dev/null
rm -rf "$PKG"; mkdir -p "$PKG/desktop/iconlib"

if [ -x "$BUILDDIR/irixscsitb" ]; then
	echo "OK: built irixscsitb (CLI)" | tee -a "$LOG"
	file "$BUILDDIR/irixscsitb" 2>/dev/null | tee -a "$LOG"

	# Ask the binary what it thinks it is. This lands in the log, so a build
	# can always be matched back to a commit and a machine afterwards.
	echo "" | tee -a "$LOG"
	"$BUILDDIR/irixscsitb" -version 2>&1 | tee -a "$LOG"
	echo "" | tee -a "$LOG"

	# Report the GUI separately so a Motif problem is obvious rather than
	# buried in the compile output above.
	if [ -n "$NOGUI" ]; then
		:
	elif [ -x "$BUILDDIR/scsitbgui" ]; then
		echo "OK: built scsitbgui (Motif GUI)" | tee -a "$LOG"
		file "$BUILDDIR/scsitbgui" 2>/dev/null | tee -a "$LOG"
	else
		echo "WARN: the Motif GUI did not build - CLI is fine, GUI skipped." | tee -a "$LOG"
		echo "      Check for libXm/libSgm above; 'versions | grep -i motif'" | tee -a "$LOG"
		echo "      should list motif_dev. Re-run with NOGUI=1 to silence this." | tee -a "$LOG"
	fi

	# --- assemble the distributable ------------------------------------
	# Everything a target machine needs: both binaries, the installer, the
	# desktop icon rules it installs, and the README. One directory, so the
	# tarball unpacks into one place rather than scattering.
	cp "$BUILDDIR/irixscsitb" "$PKG"/ 2>/dev/null
	[ -x "$BUILDDIR/scsitbgui" ] && cp "$BUILDDIR/scsitbgui" "$PKG"/ 2>/dev/null
	[ -f "$BUILDDIR/README.md" ] && cp "$BUILDDIR/README.md" "$PKG"/ 2>/dev/null
	[ -f "$BUILDDIR/install.sh" ] && cp "$BUILDDIR/install.sh" "$PKG"/ 2>/dev/null
	[ -f "$BUILDDIR/uninstall.sh" ] && cp "$BUILDDIR/uninstall.sh" "$PKG"/ 2>/dev/null
	[ -f "$BUILDDIR/desktop/scsitbgui.ftr" ] && \
		cp "$BUILDDIR/desktop/scsitbgui.ftr" "$PKG/desktop"/ 2>/dev/null
	[ -f "$BUILDDIR/desktop/iconlib/scsitbgui.fti" ] && \
		cp "$BUILDDIR/desktop/iconlib/scsitbgui.fti" "$PKG/desktop/iconlib"/ 2>/dev/null
	chmod 755 "$PKG/install.sh" "$PKG/uninstall.sh" 2>/dev/null

	TARBALL="irixscsitb-$REV.tar"

	# Plain tar, deliberately NOT tar.gz. IRIX 5.3's tar cannot decompress,
	# so a .tar.gz needs "gunzip -c f | tar xvf -" at the other end, and
	# packing one needs gzip on PATH here. The payload is ~140K; compressing
	# it would save well under 100K and cost a dependency at both ends.
	echo ">>> packaging $TARBALL" | tee -a "$LOG"
	( cd "$BUILDDIR" && tar cf "$TARBALL" "irixscsitb-$REV" ) 2>&1 | tee -a "$LOG"

	echo "" | tee -a "$LOG"
	echo "The tarball is for moving to ANOTHER machine. It is plain tar, not" | tee -a "$LOG"
	echo "gzipped, so IRIX unpacks it directly - no pipe, no gzip needed:" | tee -a "$LOG"
	echo "  tar xvf $TARBALL" | tee -a "$LOG"
	echo "" | tee -a "$LOG"
	echo "To install on this machine (as root):" | tee -a "$LOG"
	echo "  cd $PKG && ./install.sh" | tee -a "$LOG"
	echo "" | tee -a "$LOG"
	echo "Or run straight from local disk without installing:" | tee -a "$LOG"
	echo "  cd $BUILDDIR"                            | tee -a "$LOG"
	echo "  ./irixscsitb -b                     # scan the bus (no device path needed)" | tee -a "$LOG"
	echo "  ./irixscsitb -i /dev/scsi/sc0d1l0   # interrogate / detect"       | tee -a "$LOG"
	echo "  ./irixscsitb -t /dev/scsi/sc0d1l0   # list emulated targets"      | tee -a "$LOG"
	echo "  ./irixscsitb -s /dev/scsi/sc0d1l0   # list /shared"               | tee -a "$LOG"
	echo "  ./irixscsitb -l /dev/scsi/sc0d1l0   # list CD images"             | tee -a "$LOG"
	echo "NOTE: -b is the only command that needs no device path; for the rest," | tee -a "$LOG"
	echo "      put the options BEFORE the path (IRIX getopt does not reorder)." | tee -a "$LOG"
	if [ -x "$BUILDDIR/scsitbgui" ]; then
		echo "" | tee -a "$LOG"
		echo "Motif GUI (needs an X display; from an IRIX xwsh/winterm):" | tee -a "$LOG"
		echo "  DISPLAY=:0 ./scsitbgui      # opens with the bus already scanned" | tee -a "$LOG"
		echo "  DISPLAY=192.168.0.1:0 ./scsitbgui   # to an X server on the Mac" | tee -a "$LOG"
	fi
	RESULT=0
else
	echo "FAIL: irixscsitb was not produced - see the compile output above." | tee -a "$LOG"
	RESULT=1
fi

# Copy the finished set onto the share so the host can grab it. Plain cp's to
# a (possibly NFS) directory - no getcwd needed anywhere here.
echo ">>> copying results into $OUT" | tee -a "$LOG"
mkdir -p "$OUT/desktop/iconlib" 2>/dev/null
for f in irixscsitb scsitbgui README.md install.sh uninstall.sh; do
	[ -f "$PKG/$f" ] && cp "$PKG/$f" "$OUT"/ 2>/dev/null
done
[ -f "$PKG/desktop/scsitbgui.ftr" ] && cp "$PKG/desktop/scsitbgui.ftr" "$OUT/desktop"/ 2>/dev/null
[ -f "$PKG/desktop/iconlib/scsitbgui.fti" ] && cp "$PKG/desktop/iconlib/scsitbgui.fti" "$OUT/desktop/iconlib"/ 2>/dev/null
[ -f "$BUILDDIR/$TARBALL" ] && cp "$BUILDDIR/$TARBALL" "$OUT"/ 2>/dev/null
chmod 755 "$OUT/install.sh" "$OUT/uninstall.sh" 2>/dev/null

# The share is very often an NFS mount that will not keep an execute bit on a
# file the guest creates - the chmod above silently does nothing. Then
# ./install.sh gives "permission denied" while /bin/sh ./install.sh works,
# which is a genuinely confusing pair of symptoms. Check, and say so.
if [ -f "$OUT/install.sh" ] && [ ! -x "$OUT/install.sh" ]; then
	echo "" | tee -a "$LOG"
	echo "NOTE: $OUT does not keep the execute bit (an NFS share usually" | tee -a "$LOG"
	echo "      will not), so $OUT/install.sh is not directly runnable." | tee -a "$LOG"
	echo "      Use the local copy, which is executable:" | tee -a "$LOG"
	echo "          cd $PKG && ./install.sh" | tee -a "$LOG"
	echo "      or run the share copy through the shell:" | tee -a "$LOG"
	echo "          sh $OUT/install.sh" | tee -a "$LOG"
fi

echo "Artifacts in $OUT:" | tee -a "$LOG"
ls "$OUT" 2>/dev/null | tee -a "$LOG"
# Copy the (now complete) log last, to both a timestamped and a stable name.
cp "$LOG" "$OUT"/build-"$STAMP".log 2>/dev/null
cp "$LOG" "$OUT"/build-latest.log 2>/dev/null

exit $RESULT
