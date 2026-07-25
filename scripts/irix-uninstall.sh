#!/bin/sh
# irix-uninstall.sh - remove everything install.sh put on this machine.
#
# Shipped into the output/ folder (and any release tarball) as uninstall.sh.
# Run as root - logged in as root, not necessarily via su(1), which a stock
# IRIX 5.3 may not have.
#
#   ./uninstall.sh                     remove everything it can find
#   DRYRUN=1 ./uninstall.sh            list what WOULD go, touch nothing
#   BINDIR=/usr/bin/X11 ./uninstall.sh also look there for the binaries
#
# Deliberately safe to run when nothing is installed, and safe to run twice:
# every step reports "not present" rather than failing. That matters most while
# testing, which is when you run it repeatedly and half-installed states are
# normal.
#
# INVOKING IT: run "./uninstall.sh" or "sh ./uninstall.sh". The #!/bin/sh line
# is what selects the shell, so csh being the default LOGIN shell on IRIX 5.3
# makes no difference. Do NOT run it as "csh uninstall.sh" or source it from
# csh - csh reads the #! line as a comment and then chokes on Bourne syntax.
#
# This is Bourne/POSIX only, verified by running it under dash: backticks not
# $( ), no $(( )), no [[ ]], no local/declare/source, no echo -n or -e, no
# arrays, no += , no ${var/x/y}. The one construct newer than V7 sh is the
# shell function, which SVR2 added a decade before IRIX 5.3 shipped.
#

FTDIR=${FTDIR:-/usr/lib/filetype/local}
CHESTDIR=${CHESTDIR:-/usr/lib/X11/app-chests}

# install.sh takes BINDIR/PREFIX, so an install may be anywhere. Search the
# plausible homes rather than only the default, otherwise a non-default install
# quietly survives the uninstall and you are left wondering why the old binary
# is still running.
SEARCH="/usr/sbin /usr/bin/X11 /usr/bin /usr/local/bin /sbin /bin"
if [ -n "$BINDIR" ]; then
	SEARCH="$BINDIR $SEARCH"
fi
if [ -n "$PREFIX" ]; then
	SEARCH="$PREFIX/bin $SEARCH"
fi

echo "=== SCSI Toolbox uninstall ==="
if [ -n "$DRYRUN" ]; then
	echo "(dry run - nothing will actually be removed)"
fi

# --- must be root ---------------------------------------------------------
# Same shape as install.sh: IRIX 5.3's id(1) has no -u, so fall back to parsing
# the SVR4 "uid=0(root)" form, and only refuse on a POSITIVELY non-root answer.
# A dry run needs no privileges at all.
if [ -z "$DRYRUN" ]; then
	UID_NOW=`id -u 2>/dev/null`
	if [ -z "$UID_NOW" ]; then
		UID_NOW=`id 2>/dev/null | sed 's/^uid=\([0-9][0-9]*\).*/\1/'`
	fi
	case "$UID_NOW" in
		0)      ;;
		""|*[!0-9]*)
			echo "NOTE: could not determine your uid; continuing anyway." ;;
		*)      echo ""
			echo "ERROR: run this as root - it removes files from $SEARCH,"
			echo "       $CHESTDIR and $FTDIR."
			echo "       Log in as root and run it again, or use DRYRUN=1"
			echo "       to see what it would do."
			exit 1 ;;
	esac
fi

REMOVED=0

# zap FILE - remove it if present, honouring DRYRUN, and say either way.
zap() {
	if [ -f "$1" ]; then
		if [ -n "$DRYRUN" ]; then
			echo "  would remove $1"
		else
			rm -f "$1" && echo "  removed $1"
		fi
		REMOVED=1
	fi
}

# --- 1. binaries ----------------------------------------------------------
echo ""
echo ">>> binaries"
for d in $SEARCH; do
	zap "$d/irixscsitb"
	zap "$d/scsitbgui"
done
if [ $REMOVED -eq 0 ]; then
	echo "  none found in: $SEARCH"
fi

# --- 2. Toolchest entry ---------------------------------------------------
echo ""
echo ">>> Toolchest entry"
if [ -f "$CHESTDIR/scsitoolbox.chest" ]; then
	zap "$CHESTDIR/scsitoolbox.chest"
else
	echo "  not present"
fi

# --- 3. desktop icon rules ------------------------------------------------
echo ""
echo ">>> desktop icon rules"
zap "$FTDIR/scsitbgui.ftr"
zap "$FTDIR/iconlib/scsitbgui.fti"

# Take our entry back out of SGI's Makefile. Filtering our own line is safer
# than restoring the backup install.sh made: anything else installed since then
# would be thrown away with it.
if [ -f "$FTDIR/Makefile" ]; then
	if grep scsitbgui "$FTDIR/Makefile" > /dev/null 2>&1; then
		if [ -n "$DRYRUN" ]; then
			echo "  would remove the scsitbgui.ftr line from $FTDIR/Makefile"
		else
			grep -v scsitbgui "$FTDIR/Makefile" > "$FTDIR/Makefile.new" &&
				mv "$FTDIR/Makefile.new" "$FTDIR/Makefile" &&
				echo "  removed the scsitbgui.ftr line from $FTDIR/Makefile"
		fi
	else
		echo "  $FTDIR/Makefile does not mention us"
	fi
fi

if [ -f "$FTDIR/Makefile.presrmtoolbox" ]; then
	echo "  (install.sh's backup $FTDIR/Makefile.presrmtoolbox is left in place;"
	echo "   delete it yourself once you are happy)"
fi

# Rebuild the type database so the desktop stops advertising a type whose
# rules are gone.
if [ -z "$DRYRUN" ] && [ -d "$FTDIR" ]; then
	echo ""
	echo ">>> rebuilding the type database"
	if ( cd "$FTDIR" && make ) ; then
		echo "  type database rebuilt"
	else
		echo "  WARNING: 'make' in $FTDIR failed; the old icon may linger."
	fi
fi

# --- done -----------------------------------------------------------------
echo ""
echo "============================================================"
if [ -n "$DRYRUN" ]; then
	echo "Dry run only - nothing was removed."
	echo "Re-run without DRYRUN=1 to do it for real."
else
	echo "Uninstalled."
	echo ""
	echo "The Toolchest entry disappears when the window manager restarts:"
	echo "    /usr/bin/X11/tellwm restart"
	echo "The desktop icon disappears when the desktop restarts, so a reboot"
	echo "(or log out and back in) clears the last of it."
fi
echo "============================================================"
