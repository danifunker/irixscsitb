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
# -65.tardist} into the same directory. A wholly absent flavor is simply
# omitted; at least one flavor is required.
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
. "$REPO/scripts/ci-lib.sh"        # inst_enabled (BUILD_INST) + conf loading
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

# Every medium carries a dist53/ (o32) and/or dist65/ (n32) entry —
# whichever flavors were actually built.
[ -f "$DIR/irixscsitb-o32" ] || [ -f "$DIR/irixscsitb-n32" ] \
	|| die "no irixscsitb-o32 or irixscsitb-n32 in $DIR — nothing to package"
[ -f "$DIR/irixscsitb-o32" ] \
	|| echo "package-dist: NOTE: no o32 build — the media get dist65/ only, and those binaries run on IRIX 6.x ONLY" >&2

# Every flavor that was BUILT must also have been PACKAGED by its own guest,
# unless the products were switched off on purpose. iris-build.sh writes the
# trio to <dir>/inst53 (o32) or <dir>/inst65 (n32); if it is not here, either
# the guest lacked gendist or something between the build and this directory
# lost it (in Actions: the build job's artifact upload).
if inst_enabled; then
	for _f in o32:53 n32:65; do
		_fl=${_f%%:*}; _d=${_f#*:}
		[ -f "$DIR/irixscsitb-$_fl" ] || continue
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
[ -f "$DIR/irixscsitb-o32" ] && set -- "$@" --bin53 "$DIR/irixscsitb-o32"
[ -f "$DIR/scsitbgui-o32" ]  && set -- "$@" --gui53 "$DIR/scsitbgui-o32"
[ -f "$DIR/irixscsitb-n32" ] && set -- "$@" --bin65 "$DIR/irixscsitb-n32"
[ -f "$DIR/scsitbgui-n32" ]  && set -- "$@" --gui65 "$DIR/scsitbgui-n32"
# Per-OS gendist products (emitted by iris-build.sh in the same guest session
# that compiled them) become the Software Manager dists + .tardists.
[ -f "$DIR/inst53/irixscsitb.sw" ] && set -- "$@" --inst53-dir "$DIR/inst53"
[ -f "$DIR/inst65/irixscsitb.sw" ] && set -- "$@" --inst65-dir "$DIR/inst65"

exec "$REPO/scripts/package.sh" "$@"
