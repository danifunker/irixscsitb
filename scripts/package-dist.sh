#!/bin/sh
# Assemble the package.sh invocation from whatever binaries actually exist in
# a dist directory, and run it. The single packaging-arg code path for the
# GitHub Actions package job AND scripts/release-local.sh (the "[ -f gui ] &&
# add the flag" dance used to live in both).
#
# Expects in --dir (produced by iris-build.sh --outdir / downloaded artifacts):
#   irixscsitb-o32 / scsitbgui-o32   raw o32 binaries (tarball bin53/)
#   irixscsitb-n32 / scsitbgui-n32   raw n32 binaries (tarball bin65/)
#   inst53/ / inst65/                per-OS gendist product trios -> media
#                                    /dist53 + /dist65 and the .tardists
# Writes irixscsitb-<version>.{iso,hda,tar.gz,iso.gz,hda.gz,-53.tardist,
# -65.tardist} into the same directory. A flavor with binaries but no inst
# product falls back to raw binaries in its /distXX directory; a wholly
# absent flavor is simply omitted. At least one flavor is required.
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

# Every medium carries a dist53/ (o32) and/or dist65/ (n32) entry —
# whichever flavors were actually built.
[ -f "$DIR/irixscsitb-o32" ] || [ -f "$DIR/irixscsitb-n32" ] \
	|| die "no irixscsitb-o32 or irixscsitb-n32 in $DIR — nothing to package"
[ -f "$DIR/irixscsitb-o32" ] \
	|| echo "package-dist: NOTE: no o32 build — the media get dist65/ only, and those binaries run on IRIX 6.x ONLY" >&2

set -- --version "$VERSION" --outdir "$DIR" --rb-cli "$RB" --extra "$REPO/README.md"
[ -f "$DIR/irixscsitb-o32" ] && set -- "$@" --bin53 "$DIR/irixscsitb-o32"
[ -f "$DIR/scsitbgui-o32" ]  && set -- "$@" --gui53 "$DIR/scsitbgui-o32"
[ -f "$DIR/irixscsitb-n32" ] && set -- "$@" --bin65 "$DIR/irixscsitb-n32"
[ -f "$DIR/scsitbgui-n32" ]  && set -- "$@" --gui65 "$DIR/scsitbgui-n32"
# Per-OS gendist products (emitted by iris-build.sh in the same guest session
# that compiled them) become the Software Manager dists + .tardists.
[ -f "$DIR/inst53/irixscsitb.sw" ] && set -- "$@" --inst53-dir "$DIR/inst53"
[ -f "$DIR/inst65/irixscsitb.sw" ] && set -- "$@" --inst65-dir "$DIR/inst65"

exec "$REPO/scripts/package.sh" "$@"
