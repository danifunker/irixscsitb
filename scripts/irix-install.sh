#!/bin/sh
# irix-install.sh - install the SCSI Toolbox on this IRIX machine.
#
# Shipped into the output/ folder (and any release tarball) as install.sh. Run
# it as root from the directory holding the built binaries.
#
#   ./install.sh                 binaries into /usr/local/bin, plus desktop icons
#   PREFIX=/usr ./install.sh     binaries into /usr/bin instead
#   NOICONS=1 ./install.sh       binaries only, skip the desktop icon rules
#
# Two independent things get installed, and it is worth knowing which is which
# when one of them does not show up:
#
#   1. The binaries. Immediate, nothing else needed.
#   2. The desktop File Typing Rules, which are what give the executables an
#      icon in the Icon Catalog and file manager. The desktop reads its type
#      database at startup, so those do NOT appear until it restarts - hence
#      the prompt at the end.
#
# Written for the IRIX 5.3 /bin/sh (Bourne shell): backticks, no $( ), no
# 'local', no arithmetic, no 'command -v'.

PREFIX=${PREFIX:-/usr/local}
BINDIR="$PREFIX/bin"
FTDIR=/usr/lib/filetype/local
SRCDIR=`dirname "$0"`

echo "=== SCSI Toolbox install ==="

# --- must be root ---------------------------------------------------------
UID_NOW=`id -u 2>/dev/null`
if [ "$UID_NOW" != "0" ]; then
	echo "ERROR: run this as root - it writes to $BINDIR and $FTDIR."
	echo "       su, then ./install.sh"
	exit 1
fi

# --- 1. binaries ----------------------------------------------------------
FOUND=0
mkdir -p "$BINDIR" 2>/dev/null
for b in irixscsitb scsitbgui; do
	if [ -f "$SRCDIR/$b" ]; then
		cp "$SRCDIR/$b" "$BINDIR/$b" || exit 1
		chmod 755 "$BINDIR/$b"
		echo "installed $BINDIR/$b"
		FOUND=1
	fi
done
if [ $FOUND -eq 0 ]; then
	echo "ERROR: no irixscsitb or scsitbgui next to this script."
	echo "       Run it from the output/ folder that build.sh produced."
	exit 1
fi

case "$BINDIR" in
	/usr/bin|/bin|/usr/sbin) ;;
	*) echo ""
	   echo "NOTE: $BINDIR may not be on your PATH by default on IRIX."
	   echo "      Either add it, or run the tools by full path." ;;
esac

if [ -n "$NOICONS" ]; then
	echo ""
	echo "Skipping desktop icon rules (NOICONS set). Done."
	exit 0
fi

# --- 2. desktop icon rules ------------------------------------------------
if [ ! -d "$FTDIR" ]; then
	echo ""
	echo "NOTE: $FTDIR does not exist, so the Indigo Magic desktop does not"
	echo "      look to be installed. Binaries are in place; skipping icons."
	exit 0
fi
if [ ! -f "$SRCDIR/desktop/scsitbgui.ftr" ]; then
	echo ""
	echo "NOTE: no desktop/ rules next to this script; skipping icons."
	exit 0
fi

echo ""
echo ">>> installing desktop icon rules into $FTDIR"
mkdir -p "$FTDIR/iconlib" 2>/dev/null
cp "$SRCDIR/desktop/scsitbgui.ftr" "$FTDIR/scsitbgui.ftr" || exit 1
cp "$SRCDIR/desktop/iconlib/scsitbgui.fti" "$FTDIR/iconlib/scsitbgui.fti" || exit 1
echo "installed $FTDIR/scsitbgui.ftr"
echo "installed $FTDIR/iconlib/scsitbgui.fti"

# The rules only get compiled if they are listed in FTR_FILES, so add
# ourselves to SGI's Makefile - idempotently, since this script is expected to
# be re-run after every rebuild.
if grep scsitbgui "$FTDIR/Makefile" > /dev/null 2>&1; then
	echo "already listed in $FTDIR/Makefile"
else
	cp "$FTDIR/Makefile" "$FTDIR/Makefile.presrmtoolbox" 2>/dev/null
	# Insert before the ${NULL} terminator of the FTR_FILES list. index()
	# rather than a regex so no shell/awk escaping of ${...} is involved.
	awk 'BEGIN { done = 0 }
	{
		if (done == 0 && index($0, "${NULL}") > 0) {
			print "\tscsitbgui.ftr \\";
			done = 1;
		}
		print;
	}' "$FTDIR/Makefile" > "$FTDIR/Makefile.new"

	if grep scsitbgui "$FTDIR/Makefile.new" > /dev/null 2>&1; then
		mv "$FTDIR/Makefile.new" "$FTDIR/Makefile"
		echo "added scsitbgui.ftr to $FTDIR/Makefile (backup: Makefile.presrmtoolbox)"
	else
		rm -f "$FTDIR/Makefile.new"
		echo "WARNING: could not add scsitbgui.ftr to $FTDIR/Makefile automatically."
		echo "         Add it to the FTR_FILES list by hand, then run 'make' there."
	fi
fi

echo ""
echo ">>> compiling the type database (/usr/sbin/ftr, /usr/sbin/fftr)"
if ( cd "$FTDIR" && make ) ; then
	echo "type database rebuilt"
else
	echo "WARNING: 'make' in $FTDIR failed - the icon will not appear."
	echo "         Run '/usr/sbin/ftr -v scsitbgui.ftr' there to see why."
fi

# --- done -----------------------------------------------------------------
echo ""
echo "============================================================"
echo "Binaries are installed and usable NOW:"
echo "    $BINDIR/irixscsitb -b"
echo "    $BINDIR/scsitbgui"
echo ""
echo "The desktop ICON will not appear until the desktop reloads its"
echo "type database, which it only does at startup."
echo ""
echo "    PLEASE REBOOT (or log out and back in) to see the icon."
echo "============================================================"
