#!/bin/sh
# Assemble the package.sh invocation from whatever binaries actually exist in
# a dist directory, and run it. The single packaging-arg code path for the
# GitHub Actions package job AND scripts/release-local.sh (the "[ -f gui ] &&
# add the flag" dance used to live in both).
#
# Expects in --dir (produced by iris-build.sh --outdir / downloaded artifacts):
#   irixscsitb-o32              images + tarball (the portable 5.3-6.5 binary)
#   scsitbgui-o32               optional — images + tarball
#   irixscsitb-n32              optional — tarball only
#   scsitbgui-n32               optional — tarball only
# Writes irixscsitb-<version>.{iso,hda,tar.gz} into the same directory.
#
# When the o32 pair is absent (an n32-only setup, e.g. BUILD_O32=0), the n32
# pair takes its place in the images + tarball — with a loud note, since that
# binary only runs on IRIX 6.x. At least one CLI binary is required.
#
# Usage:
#   scripts/package-dist.sh --version V --dir DIR [--rb-cli PATH]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION=""
DIR=""
RB="${RB_CLI:-rb-cli}"

die() { echo "package-dist: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION="$2"; shift 2 ;;
		--dir)     DIR="$2"; shift 2 ;;
		--rb-cli)  RB="$2"; shift 2 ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
[ -n "$DIR" ] || die "missing --dir"
DIR=$(cd "$DIR" 2>/dev/null && pwd) || die "dist dir not found: $DIR"

# The images' /irixscsitb is the o32 (portable) binary when built; an
# n32-only run falls back to n32 — clearly noted, it is 6.x-only media then.
if [ -f "$DIR/irixscsitb-o32" ]; then
	BASE="o32"
elif [ -f "$DIR/irixscsitb-n32" ]; then
	BASE="n32"
	echo "package-dist: NOTE: no o32 binary — the .iso/.hda will carry the n32 build, which runs on IRIX 6.x ONLY" >&2
else
	die "no irixscsitb-o32 or irixscsitb-n32 in $DIR — nothing to package"
fi

set -- --bin "$DIR/irixscsitb-$BASE" --version "$VERSION" \
       --outdir "$DIR" --rb-cli "$RB" --extra "$REPO/README.md"
[ -f "$DIR/scsitbgui-$BASE" ] && set -- "$@" --gui-bin "$DIR/scsitbgui-$BASE"
for f in irixscsitb-o32 scsitbgui-o32 irixscsitb-n32 scsitbgui-n32; do
	case "$f" in irixscsitb-$BASE|scsitbgui-$BASE) continue ;; esac
	[ -f "$DIR/$f" ] && set -- "$@" --tar-bin "$DIR/$f"
done

exec "$REPO/scripts/package.sh" "$@"
