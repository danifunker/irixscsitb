#!/bin/sh
# irix-install.sh - install the SCSI Toolbox on this IRIX machine.
#
# Shipped into the output/ folder (and any release tarball) as install.sh. Run
# it as root - logged in as root, not necessarily via su(1), which a stock
# IRIX 5.3 may not have - from the directory holding the built binaries.
#
#   ./install.sh                     binaries into /usr/sbin, plus desktop icons
#   BINDIR=/usr/bin/X11 ./install.sh install somewhere else
#   PREFIX=/usr/local ./install.sh   prefix style: <prefix>/bin
#   NOICONS=1 ./install.sh           binaries only, skip the desktop icon rules
#
# Three independent things get installed, and it is worth knowing which is
# which when one of them does not show up:
#
#   1. The binaries. Immediate, nothing else needed.
#   2. A Toolchest entry, so the GUI is on the start menu. Appears as soon as
#      the window manager restarts.
#   3. The desktop File Typing Rules, which are what give the executables an
#      icon in the Icon Catalog and file manager. The desktop reads its type
#      database only at startup, so this one needs a reboot.
#
# INVOKING IT: run "./install.sh" or "sh ./install.sh". The #!/bin/sh line is
# what selects the shell, so csh being the default LOGIN shell on IRIX 5.3
# makes no difference. Do NOT run it as "csh install.sh" or source it from
# csh - csh reads the #! line as a comment and then chokes on Bourne syntax.
#
# This is Bourne/POSIX only, verified by running it under dash: backticks not
# $( ), no $(( )), no [[ ]], no local/declare/source, no echo -n or -e, no
# arrays, no += , no ${var/x/y}. The one construct newer than V7 sh is the
# shell function, which SVR2 added a decade before IRIX 5.3 shipped.
#

# /usr/sbin, not /usr/local/bin. Both tools need root to open the generic SCSI
# nodes, /usr/sbin is where IRIX keeps administrative binaries, and - the part
# that actually matters - it is on root's default PATH, which /usr/local/bin is
# not (and on a stock install may not even exist). BINDIR overrides outright;
# PREFIX is honoured for anyone who prefers prefix-style layouts.
#
# The other defensible home is /usr/bin/X11, where IRIX keeps X clients: that
# would suit scsitbgui but not the CLI, so both go to one place by default and
# BINDIR is there for anyone who wants to split them.
if [ -n "$PREFIX" ]; then
	BINDIR=${BINDIR:-$PREFIX/bin}
else
	BINDIR=${BINDIR:-/usr/sbin}
fi
# Overridable so the script can be exercised against a fake tree off-target.
FTDIR=${FTDIR:-/usr/lib/filetype/local}
CHESTDIR=${CHESTDIR:-/usr/lib/X11/app-chests}
SRCDIR=`dirname "$0"`

echo "=== SCSI Toolbox install ==="

# --- is there anything to install? ----------------------------------------
# Checked BEFORE the root test on purpose. This needs no privileges, and the
# overwhelmingly common mistake is running the installer before the build (or
# from the source folder rather than output/). Testing root first answers a
# question nobody asked and sends you off to become root for no reason.
HAVE=""
for b in irixscsitb scsitbgui; do
	if [ -f "$SRCDIR/$b" ]; then
		HAVE="$HAVE $b"
	fi
done
if [ -z "$HAVE" ]; then
	echo "ERROR: no irixscsitb or scsitbgui in $SRCDIR - nothing to install yet."
	echo ""
	echo "  If you have not built yet:"
	echo "      ./build.sh"
	echo "      cd output && ./install.sh"
	echo ""
	echo "  If you have built, run the installer from the output/ folder"
	echo "  (or an unpacked release tarball) - it installs the binaries that"
	echo "  sit next to it, and there are none here."
	exit 1
fi
echo "found:$HAVE"

# --- must be root ---------------------------------------------------------
# IRIX 5.3's id(1) has NO -u flag. It fails, UID_NOW comes back empty, and a
# naive [ "$UID_NOW" != "0" ] then reports "not root" precisely when you ARE
# root - which is what this used to do. So: try -u for modern systems, and fall
# back to parsing the SVR4 "uid=0(root) gid=0(sys)" form that IRIX prints.
UID_NOW=`id -u 2>/dev/null`
if [ -z "$UID_NOW" ]; then
	UID_NOW=`id 2>/dev/null | sed 's/^uid=\([0-9][0-9]*\).*/\1/'`
fi

# Block only when we have positively established a non-root uid. If neither
# form parsed we do not know, and refusing on a guess would be the same bug
# again - let the copies below fail with a real permission error instead.
case "$UID_NOW" in
	0)      ;;
	"")     echo "NOTE: could not determine your uid; continuing anyway." ;;
	*[!0-9]*)
		echo "NOTE: could not read a uid out of id(1); continuing anyway." ;;
	*)      echo ""
		echo "ERROR: run this as root - it writes to $BINDIR and $FTDIR."
		echo "       Log in as root and run it again. (Do not assume su(1)"
		echo "       is available; a stock IRIX 5.3 may not have it.)"
		exit 1 ;;
esac

# --- 1. binaries ----------------------------------------------------------
mkdir -p "$BINDIR" 2>/dev/null
for b in $HAVE; do
	cp "$SRCDIR/$b" "$BINDIR/$b" || exit 1
	chmod 755 "$BINDIR/$b"
	echo "installed $BINDIR/$b"
done

case "$BINDIR" in
	/usr/bin|/bin|/usr/sbin|/sbin|/usr/bsd|/usr/bin/X11) ;;
	*) echo ""
	   echo "NOTE: $BINDIR may not be on your PATH by default on IRIX."
	   echo "      Either add it, or run the tools by full path." ;;
esac

if [ -n "$NOICONS" ]; then
	echo ""
	echo "Skipping desktop icon rules (NOICONS set). Done."
	exit 0
fi

# --- 2. Toolchest (start menu) entry --------------------------------------
# /usr/lib/X11/app-chests is a drop-in directory: system.chestrc ends with
# "sinclude /usr/lib/X11/app-chests", so every *.chest file in it is pulled in.
# That means we never have to edit a system config file - a wrong fragment can
# at worst fail to add a menu item, it cannot break the existing menus.
#
# "Menu ToolChest" is additive: system.chestrc itself reopens it AFTER the
# sinclude in order to append Help, which is what lets a fragment contribute a
# top-level entry.
#
# f.checkexec.sh is the same verb SGI's own entries use; the "check" part means
# the item only appears when the program is actually present, so uninstalling
# the binary makes the menu entry disappear by itself.
if [ -d "$CHESTDIR" ]; then
	echo ""
	echo ">>> adding a Toolchest entry in $CHESTDIR"
	# Generated rather than shipped as a static file: the path has to match
	# wherever BINDIR actually put the binary.
	cat > "$CHESTDIR/scsitoolbox.chest" <<CHEST
################################################################################
# scsitoolbox.chest - installed by the SCSI Toolbox installer.
#
# Pulled in by the "sinclude /usr/lib/X11/app-chests" at the end of
# /usr/lib/X11/system.chestrc. Safe to delete - the menu entry goes with it.
################################################################################

Menu ToolChest
{
     no-label		f.separator
    "SCSI Toolbox"	f.checkexec.sh "$BINDIR/scsitbgui"
}
CHEST
	if [ -f "$CHESTDIR/scsitoolbox.chest" ]; then
		echo "installed $CHESTDIR/scsitoolbox.chest"
	else
		echo "WARNING: could not write $CHESTDIR/scsitoolbox.chest"
	fi
else
	echo ""
	echo "NOTE: $CHESTDIR is not present, so this system has no Toolchest to"
	echo "      add to. Skipping the start-menu entry."
fi

# --- 3. desktop icon rules ------------------------------------------------
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
echo "The other two pieces need something restarted, and they differ:"
echo ""
echo "  Toolchest entry  - restart the window manager. Either"
echo "                       /usr/bin/X11/tellwm restart"
echo "                     or Toolchest > System > Restart Window Manager."
echo "  Desktop icon     - needs the DESKTOP restarted, which reads its type"
echo "                     database only at startup."
echo ""
echo "    PLEASE REBOOT (or log out and back in) - that covers both."
echo "============================================================"
