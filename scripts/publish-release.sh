#!/bin/sh
# Create the GitHub release from a packaged dist directory — the single
# release-publishing code path for the Actions release job AND
# scripts/release-local.sh, so the notes text and the artifact set can never
# drift between the two.
#
# Attaches whatever of the canonical set exists in --dist:
#   irixscsitb-o32  scsitbgui-o32  irixscsitb-n32  scsitbgui-n32
#   irixscsitb-<version>.tardist                   (when the inst dist built)
#   irixscsitb-<version>.iso  .hda  .tar.gz        (the three are required)
#
# Usage:
#   scripts/publish-release.sh --version V --dist DIR
#                              [--repo OWNER/NAME] [--draft] [--dry-run]
#
# Needs `gh` authenticated (or $GH_TOKEN, as in Actions). --dry-run prints the
# exact command instead of running it.
set -eu

VERSION=""
DIST=""
GHREPO=""
DRAFT=0
DRYRUN=0

die() { echo "publish-release: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION="$2"; shift 2 ;;
		--dist)    DIST="$2"; shift 2 ;;
		--repo)    GHREPO="$2"; shift 2 ;;
		--draft)   DRAFT=1; shift ;;
		--dry-run) DRYRUN=1; shift ;;
		-h|--help) sed -n '2,18p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
[ -n "$DIST" ] || die "missing --dist"
DIST=$(cd "$DIST" 2>/dev/null && pwd) || die "dist dir not found: $DIST"
TAG="v$VERSION"

# Canonical artifact set: optional binaries + tardist first, then the
# required images.
set --
for f in irixscsitb-o32 scsitbgui-o32 irixscsitb-n32 scsitbgui-n32 \
         "irixscsitb-$VERSION.tardist"; do
	[ -f "$DIST/$f" ] && set -- "$@" "$DIST/$f"
done
for f in "irixscsitb-$VERSION.iso" "irixscsitb-$VERSION.hda" "irixscsitb-$VERSION.tar.gz"; do
	[ -f "$DIST/$f" ] || die "missing $DIST/$f — run scripts/package-dist.sh first"
	set -- "$@" "$DIST/$f"
done

NOTES="Automated release, built natively inside the IRIS emulator on real IRIX \
5.3 and 6.5 installs: o32 (5.3-6.5) + n32 (6.x) CLI and Motif GUI binaries, \
plus .iso (EFS CD-ROM), .hda (SGI EFS hard disk), and .tar.gz (binaries + \
README, for NFS/direct copy) for the IRIS emulator and real BlueSCSI/ZuluSCSI \
hardware. See docs/ci-iris.md for how this pipeline works."

if [ "$DRYRUN" = 1 ]; then
	echo "publish-release: dry run — would publish with:"
	echo
	printf '  gh release create %s \\\n' "$TAG"
	[ -z "$GHREPO" ] || printf '    --repo %s \\\n' "$GHREPO"
	[ "$DRAFT" = 0 ] || printf '    --draft \\\n'
	printf '    --title "irixscsitb %s" \\\n' "$VERSION"
	printf '    --notes "..." \\\n'
	for f in "$@"; do printf '    %s \\\n' "$f"; done
	exit 0
fi

command -v gh >/dev/null 2>&1 || die "gh not found"
gh release view "$TAG" ${GHREPO:+--repo "$GHREPO"} >/dev/null 2>&1 && die "release $TAG already exists"

# shellcheck disable=SC2086 # optional flags expand to nothing on purpose
gh release create "$TAG" \
	${GHREPO:+--repo "$GHREPO"} \
	$( [ "$DRAFT" = 1 ] && echo --draft ) \
	--title "irixscsitb $VERSION" \
	--notes "$NOTES" \
	"$@"

echo "publish-release: published:"
gh release view "$TAG" ${GHREPO:+--repo "$GHREPO"} --json url --jq .url
