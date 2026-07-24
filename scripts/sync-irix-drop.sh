#!/bin/sh
# sync-irix-drop.sh - assemble the folder an IRIX/IRIS machine builds from.
#
# This is the ONLY supported way to produce that folder. It used to be a pile of
# cp commands typed by hand, which meant the build inputs for the vintage machine
# were not reproducible and not in version control. Now they are: everything the
# drop contains comes from the repo, and this script is the manifest.
#
# What it does that the IRIX side cannot:
#
#   * stamps version.h from git - the revision, whether the tree was dirty, and
#     when. IRIX has no git, so this HAS to happen here. The header then travels
#     as a static build input; nothing on the far side rewrites it.
#   * flattens the tree, because the IRIX build dir is flat.
#   * renames scripts/irix-native-build.sh to build.sh, which is what the HOWTO
#     tells you to run and what fingers remember.
#
#   scripts/sync-irix-drop.sh [destination]
#       default destination: $HOME/Downloads/irixscsitb53
#
# The destination's build/ subfolder is left alone - that is where the IRIX side
# drops its output, and blowing it away would discard the last build's log.

set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
DEST="${1:-$HOME/Downloads/irixscsitb53}"

cd "$REPO"

echo "sync-irix-drop: repo $REPO"
echo "sync-irix-drop: dest $DEST"

# --- 1. stamp the revision, here, where git exists ------------------------
sh scripts/mkversion.sh version.h

if grep -q -- '-dirty' version.h; then
	echo ""
	echo "  NOTE: the tree is dirty, so this drop is stamped <rev>-dirty."
	echo "        Commit first if you want a build that maps to a real commit."
	echo ""
fi

# --- 2. assemble ----------------------------------------------------------
mkdir -p "$DEST" "$DEST/scripts"

# Sources the IRIX build actually compiles. linux.c is deliberately absent:
# the Makefile never selects it on IRIX and it would not compile there anyway.
for f in irixscsitb.c toolbox.c gui_motif.c version.c irix.c \
         irixscsitb.h os.h Makefile README.md version.h; do
	cp "$REPO/$f" "$DEST/$f"
done

# Only needed as a fallback if version.h ever goes missing on the far side.
cp "$REPO/scripts/mkversion.sh" "$DEST/scripts/mkversion.sh"

# Desktop icon rules - installed by hand on the IRIX side, see the HOWTO.
mkdir -p "$DEST/desktop/iconlib"
cp "$REPO/desktop/scsitbgui.ftr" "$DEST/desktop/scsitbgui.ftr"
cp "$REPO/desktop/iconlib/scsitbgui.fti" "$DEST/desktop/iconlib/scsitbgui.fti"

# The build driver and its instructions, renamed to what the HOWTO documents.
cp "$REPO/scripts/irix-native-build.sh" "$DEST/build.sh"
cp "$REPO/docs/HOWTO-IRIS.txt" "$DEST/HOWTO-IRIS.txt"
chmod +x "$DEST/build.sh" "$DEST/scripts/mkversion.sh"

echo ""
echo "Stamped into the drop:"
sed -n 's/^#define BUILD_\([A-Z_]*\) *"\(.*\)"/  \1 = \2/p' "$DEST/version.h"
echo ""
echo "Next: in IRIX, mount the share and run ./build.sh"
