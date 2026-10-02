#!/bin/sh
# Assemble the package.sh invocation from whatever binaries actually exist in
# a dist directory, and run it. The single packaging-arg code path for the
# GitHub Actions package job AND scripts/release-local.sh (the "[ -f gui ] &&
# add the flag" dance used to live in both).
#
# Expects in --dir (produced by iris-build.sh --outdir / downloaded artifacts):
#   irixscsitb-<flavor> / scsitbgui-<flavor>   raw binaries, flavor = o32 |
#                                    mips1 | n32 (tarball bin<key>/)
#   inst<key>/                       per-OS gendist product trios -> media
#                                    /dist<key> and the .tardists
# where <key> is the flavor's dist key from scripts/ci-lib.sh: 53 (o32),
# mips1 (mips1), 65 (n32). Writes irixscsitb-<version>.{iso,hda,tar.gz,
# iso.gz,hda.gz} and -<key>.tardist into the same directory. A wholly absent
# flavor is simply omitted; at least one flavor is required.
#
# A flavor with binaries but NO inst product can still be packaged — its raw
# binaries take the /distXX directory's place — but that is a real downgrade
# (no .tardist, nothing for Software Manager to install), so it is only
# allowed when the products were switched off deliberately with BUILD_INST=0
# / release-local.sh --skip-inst. Otherwise it is an error: the products going
# missing has historically been INVISIBLE here (CI built them, then dropped
# them on the build runner's floor because the artifact upload didn't carry
# dist/inst53, dist/inst65), and the release published looking perfectly fine.
#
# Usage:
#   scripts/package-dist.sh --version V --dir DIR [--rb-cli PATH]
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"        # flavor table, inst_enabled, conf loading
VERSION=""
DIR=""
RB="${RB_CLI:-rb-cli}"

die() { echo "package-dist: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION="$2"; shift 2 ;;
		--dir)     DIR="$2"; shift 2 ;;
		--rb-cli)  RB="$2"; shift 2 ;;
		-h|--help) sed -n '2,25p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

load_local_conf
[ -n "$VERSION" ] || die "missing --version"
[ -n "$DIR" ] || die "missing --dir"
DIR=$(cd "$DIR" 2>/dev/null && pwd) || die "dist dir not found: $DIR"

# Every medium carries a dist<key>/ entry per flavor actually built.
BUILT=""
for _fl in $FLAVORS; do
	[ -f "$DIR/irixscsitb-$_fl" ] && BUILT="$BUILT $_fl"
done
[ -n "$BUILT" ] || die "no irixscsitb-{$(echo $FLAVORS | tr ' ' ,)} in $DIR — nothing to package"
[ -f "$DIR/irixscsitb-o32" ] \
	|| echo "package-dist: NOTE: no o32 build — the media carry no mips2 build for IRIX 5.3-6.5 (built:$BUILT)" >&2
[ -f "$DIR/irixscsitb-mips1" ] \
	|| echo "package-dist: NOTE: no mips1 build — nothing on the media runs on an R3000 (built:$BUILT)" >&2

# Every flavor that was BUILT must also have been PACKAGED by its own guest,
# unless the products were switched off on purpose. iris-build.sh writes the
# trio to <dir>/inst<key>; if it is not here, either the guest lacked gendist
# or something between the build and this directory lost it (in Actions: the
# build job's artifact upload).
if inst_enabled; then
	for _fl in $BUILT; do
		_d=$(flavor_dist_key "$_fl")
		[ -f "$DIR/inst$_d/irixscsitb.sw" ] && continue
		die "the $_fl build has no Software Manager product ($DIR/inst$_d/irixscsitb.sw).
  Without it the media carry raw binaries instead of an installable
  distribution and no irixscsitb-$VERSION-$_d.tardist is cut.
  In Actions: check that build-native uploads dist/inst$_d in its artifact.
  Locally: check the iris-build.sh log for the gendist step.
  To release without the products anyway: BUILD_INST=0 (or --skip-inst)."
	done
fi

set -- --version "$VERSION" --outdir "$DIR" --rb-cli "$RB" --extra "$REPO/README.md"
for _fl in $BUILT; do
	_k=$(flavor_dist_key "$_fl")
	set -- "$@" "--bin$_k" "$DIR/irixscsitb-$_fl"
	[ -f "$DIR/scsitbgui-$_fl" ] && set -- "$@" "--gui$_k" "$DIR/scsitbgui-$_fl"
	# Per-OS gendist products (emitted by iris-build.sh in the same guest
	# session that compiled them) become the Software Manager dists +
	# .tardists.
	[ -f "$DIR/inst$_k/irixscsitb.sw" ] && set -- "$@" "--inst$_k-dir" "$DIR/inst$_k"
done

exec "$REPO/scripts/package.sh" "$@"
