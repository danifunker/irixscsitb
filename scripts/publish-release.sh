#!/bin/sh
# Create the GitHub release from a packaged dist directory — the single
# release-publishing code path for the Actions release job AND
# scripts/release-local.sh, so the notes text and the artifact set can never
# drift between the two.
#
# Attaches whatever of the canonical set exists in --dist:
#   irixscsitb-<flavor>  scsitbgui-<flavor>     flavor = o32 | mips1 | n32
#   irixscsitb-<version>-<key>.tardist          key = 53 | mips1 | 65 (the
#                                               per-flavor SWM packages)
#   irixscsitb-<version>.iso.gz  .hda.gz  .tar.gz  (the three are required;
#   the images ship gzipped — mostly empty space — raw stays local)
#
# The release notes open with a download table built from the same files,
# so a release that lacks a flavor never advertises it.
#
# Usage:
#   scripts/publish-release.sh --version V --dist DIR
#                              [--repo OWNER/NAME] [--draft] [--dry-run]
#
# Needs `gh` authenticated (or $GH_TOKEN, as in Actions). --dry-run prints the
# exact command instead of running it.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"        # the flavor table
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
		-h|--help) sed -n '2,22p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

[ -n "$VERSION" ] || die "missing --version"
[ -n "$DIST" ] || die "missing --dist"
DIST=$(cd "$DIST" 2>/dev/null && pwd) || die "dist dir not found: $DIST"
TAG="v$VERSION"

# Canonical artifact set: optional binaries + tardists first, then the
# required (compressed) images.
set --
for fl in $FLAVORS; do
	for f in "irixscsitb-$fl" "scsitbgui-$fl"; do
		[ -f "$DIST/$f" ] && set -- "$@" "$DIST/$f"
	done
done
for fl in $FLAVORS; do
	f="irixscsitb-$VERSION-$(flavor_dist_key "$fl").tardist"
	[ -f "$DIST/$f" ] && set -- "$@" "$DIST/$f"
done
for f in "irixscsitb-$VERSION.iso.gz" "irixscsitb-$VERSION.hda.gz" "irixscsitb-$VERSION.tar.gz"; do
	[ -f "$DIST/$f" ] || die "missing $DIST/$f — run scripts/package-dist.sh first"
	set -- "$@" "$DIST/$f"
done

# Backstop: the tardists are optional here (BUILD_INST=0 is a real choice, and
# this script has no view of it), but a flavor whose binary shipped while its
# product did not is worth shouting about — that combination went out
# unnoticed once already, and the media are degraded in the same way.
for fl in $FLAVORS; do
	d=$(flavor_dist_key "$fl")
	[ -f "$DIST/irixscsitb-$fl" ] || continue
	[ -f "$DIST/irixscsitb-$VERSION-$d.tardist" ] && continue
	echo "publish-release: WARNING: $fl binaries but no irixscsitb-$VERSION-$d.tardist" >&2
	echo "publish-release:          -> the .iso/.hda dist$d carries RAW BINARIES, not an" >&2
	echo "publish-release:             installable distribution. Intended only if the" >&2
	echo "publish-release:             Software Manager products were switched off." >&2
done

# ---- release notes --------------------------------------------------------------
# Per flavor: what it runs on, and how it was built. Kept next to the table
# rather than in ci-lib.sh because it is Markdown for people, not config.
flavor_runs_on() {
	case "$1" in
		o32)   echo "IRIX 5.3 – 6.5 on an **R4000 or later** — Indy, Indigo R4000, Indigo², Challenge, O2, Octane, …" ;;
		mips1) echo "IRIX 5.3 – 6.5 on **any** MIPS CPU — the one for **R2000/R3000** machines such as the R3000 Indigo (IP12), which cannot run the others" ;;
		n32)   echo "IRIX 6.x only — the fastest build for 6.5 machines" ;;
	esac
}
flavor_build() {
	case "$1" in
		o32)   echo "o32 ABI, MIPS II (\`cc -32 -mips2\`), compiled and packaged on IRIX 5.3" ;;
		mips1) echo "o32 ABI, MIPS I (\`cc -32 -mips1\`), compiled and packaged on IRIX 5.3" ;;
		n32)   echo "n32 ABI, MIPS III (\`cc -n32 -mips3\`), compiled and packaged on IRIX 6.5" ;;
	esac
}

NOTES=$(
	echo "Built natively inside the [IRIS emulator](https://github.com/techomancer/iris) on real IRIX 5.3 and 6.5 installs. Every build is the command-line tool \`irixscsitb\` plus the Motif GUI \`scsitbgui\`, for BlueSCSI and ZuluSCSI."
	echo
	echo "## Which download?"
	echo
	echo "Run \`hinv | grep CPU\` on the SGI. **R2000/R3000 → the \`mips1\` package.** Anything else on IRIX 5.3 – 6.5 → the \`53\` package; on IRIX 6.x the \`65\` package is faster."
	echo
	echo "| Software Manager package | Runs on | Build |"
	echo "|---|---|---|"
	for fl in $FLAVORS; do
		f="irixscsitb-$VERSION-$(flavor_dist_key "$fl").tardist"
		[ -f "$DIST/$f" ] || continue
		echo "| \`$f\` | $(flavor_runs_on "$fl") | $(flavor_build "$fl") |"
	done
	echo
	echo "### Everything else"
	echo
	echo "| Download | What it is |"
	echo "|---|---|"
	for fl in $FLAVORS; do
		[ -f "$DIST/irixscsitb-$fl" ] || continue
		g=""
		[ -f "$DIST/scsitbgui-$fl" ] && g=" / \`scsitbgui-$fl\`"
		echo "| \`irixscsitb-$fl\`$g | the raw \`$fl\` binaries — same build as its package, nothing installed for you (copy to \`/usr/sbin\`, \`chmod +x\`) |"
	done
	_dirs=""; _bins=""
	for fl in $FLAVORS; do
		[ -f "$DIST/irixscsitb-$fl" ] || continue
		_dirs="$_dirs \`/dist$(flavor_dist_key "$fl")\`"
		_bins="$_bins \`bin$(flavor_dist_key "$fl")/\`"
	done
	echo "| \`irixscsitb-$VERSION.iso.gz\` | IRIX EFS CD-ROM image carrying every package above as an inst distribution:$_dirs. \`gunzip\`, burn or attach in IRIS, then \`inst -f /CDROM/dist53\` (or the directory for your machine) |"
	echo "| \`irixscsitb-$VERSION.hda.gz\` | the same distributions on an SGI EFS hard-disk image (attach as a disk in IRIS, mount, \`inst -f\`) |"
	echo "| \`irixscsitb-$VERSION.tar.gz\` | the same tree plus the raw binaries with execute bits ($(echo $_bins | sed 's/ /, /g')), for an NFS share |"
	echo
	echo "## Installing"
	echo
	echo "A \`.tardist\` is a plain tar, so it unpacks even on IRIX 5.3 (no \`z\` flag needed): \`tar xvf irixscsitb-*.tardist\`, then \`inst -f .\` in that directory — or just open it with Software Manager. Each package installs \`/usr/sbin/irixscsitb\`, \`/usr/sbin/scsitbgui\` and a Toolchest entry. All three are the same product, so installing a different flavor later replaces the one you have."
	echo
	echo "See [docs/ci-iris.md](https://github.com/${GHREPO:-danifunker/irixscsitb}/blob/main/docs/ci-iris.md) for how this pipeline works."
)

if [ "$DRYRUN" = 1 ]; then
	echo "publish-release: dry run — would publish with:"
	echo
	printf '  gh release create %s \\\n' "$TAG"
	[ -z "$GHREPO" ] || printf '    --repo %s \\\n' "$GHREPO"
	[ "$DRAFT" = 0 ] || printf '    --draft \\\n'
	printf '    --title "irixscsitb %s" \\\n' "$VERSION"
	printf '    --notes "$NOTES" \\\n'
	for f in "$@"; do printf '    %s \\\n' "$f"; done
	echo
	echo "----- release notes -----"
	printf '%s\n' "$NOTES"
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
