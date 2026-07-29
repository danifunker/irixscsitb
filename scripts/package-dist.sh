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

# Every medium carries a dist53/ (o32) and/or dist65/ (n32) directory —
# whichever flavors were actually built.
[ -f "$DIR/irixscsitb-o32" ] || [ -f "$DIR/irixscsitb-n32" ] \
	|| die "no irixscsitb-o32 or irixscsitb-n32 in $DIR — nothing to package"
[ -f "$DIR/irixscsitb-o32" ] \
	|| echo "package-dist: NOTE: no o32 build — the media get dist65/ only, and those binaries run on IRIX 6.x ONLY" >&2

set -- --version "$VERSION" --outdir "$DIR" --rb-cli "$RB" --extra "$REPO/README.md"
[ -f "$DIR/irixscsitb-o32" ] && set -- "$@" --dist53-bin "$DIR/irixscsitb-o32"
[ -f "$DIR/scsitbgui-o32" ]  && set -- "$@" --dist53-gui "$DIR/scsitbgui-o32"
[ -f "$DIR/irixscsitb-n32" ] && set -- "$@" --dist65-bin "$DIR/irixscsitb-n32"
[ -f "$DIR/scsitbgui-n32" ]  && set -- "$@" --dist65-gui "$DIR/scsitbgui-n32"

exec "$REPO/scripts/package.sh" "$@"
