#!/bin/sh
# Resolve — or download — the boot disk for a build flavor, printing the
# usable .chd path on STDOUT. The single image-acquisition code path for the
# GitHub Actions workflow AND local runs (it used to be inline workflow YAML).
#
# Source resolution, first match wins (see scripts/ci-lib.sh):
#   1. $IRIX53_IMAGE / $IRIX65_IMAGE          local path; in Actions this is
#                                             how the irix*_image dispatch
#                                             inputs arrive (self-hosted mode)
#   2. ci/local.conf                   per-machine file, .gitignore'd
#   3. $IRIX53_DISK_URL / $IRIX65_DISK_URL    download URL; a repo secret in
#                                             Actions (hosted mode). Accepts a
#                                             bare .chd or a .zip with one.
#
# Modes:
#   (default)              print the path, downloading to --dest if only a
#                          URL is available. Idempotent: an existing non-empty
#                          --dest is reused (that's what makes actions/cache a
#                          transparent win — a cache hit means no download).
#   --check-only           verify a source EXISTS without downloading; probes
#                          a URL reachability best-effort (warn, never fail).
#                          This is the preflight job.
#   --cache-key            print a short stable hash of the download URL (for
#                          actions/cache); prints "local" when a local path
#                          will be used, so callers can skip caching.
#   --enabled              exit 0 if this flavor is enabled (BUILD_O32 /
#                          BUILD_N32 from env or ci/local.conf; default on),
#                          1 if switched off. Lets the workflow's matrix
#                          filter use the same logic as release-local.sh.
#
# Usage:
#   IMG=$(scripts/fetch-image.sh --flavor o32 [--dest guest-disk.chd])
#   scripts/fetch-image.sh --flavor n32 --check-only
#
# Logs to stderr; stdout carries only the path / key.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

FLAVOR=""
DEST=""
MODE="fetch"

die() { echo "fetch-image: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--flavor)     FLAVOR="$2"; shift 2 ;;
		--dest)       DEST="$2"; shift 2 ;;
		--check-only) MODE="check"; shift ;;
		--cache-key)  MODE="key"; shift ;;
		--enabled)    MODE="enabled"; shift ;;
		-h|--help)    sed -n '2,34p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf fills in whatever flags/env didn't set

IMG_KEY=$(flavor_img_key "$FLAVOR") || die "--flavor must be o32 or n32"
URL_KEY=$(flavor_url_key "$FLAVOR")
LOCAL=$(resolve_local_image "$FLAVOR")
URL=$(resolve_disk_url "$FLAVOR")
[ -n "$DEST" ] || DEST="guest-disk-$FLAVOR.chd"

no_source() {
	die "no boot disk source for --flavor $FLAVOR: pass a local path (\$$IMG_KEY,
  the irix*_image dispatch input, or ci/local.conf — copy the
  .example), or set the $URL_KEY secret/env to a private download URL"
}

case "$MODE" in
enabled)
	if flavor_enabled "$FLAVOR"; then
		echo "fetch-image: $FLAVOR enabled" >&2
		exit 0
	fi
	echo "fetch-image: $FLAVOR disabled (BUILD_O32/BUILD_N32)" >&2
	exit 1
	;;
key)
	if [ -n "$LOCAL" ]; then
		echo "local"
	elif [ -n "$URL" ]; then
		printf %s "$URL" | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-16
	else
		no_source
	fi
	exit 0
	;;
check)
	if [ -n "$LOCAL" ]; then
		[ -f "$LOCAL" ] || die "$IMG_KEY is configured but the file is missing: $LOCAL"
		echo "fetch-image: $FLAVOR: local image present ($LOCAL)" >&2
	elif [ -n "$URL" ]; then
		# Warn-only probe: some hosts reject HEAD/range requests, so an
		# unanswered probe must not fail the run — but a typo'd URL usually
		# shows up here instead of minutes into the build job.
		if curl -sfIL --max-time 20 "$URL" >/dev/null 2>&1 \
		   || curl -sfL --max-time 20 -r 0-0 -o /dev/null "$URL" 2>/dev/null; then
			echo "fetch-image: $FLAVOR: download URL answers" >&2
		else
			echo "fetch-image: WARNING: $FLAVOR download URL did not answer a HEAD/range probe — the download may fail" >&2
		fi
	else
		no_source
	fi
	exit 0
	;;
esac

# ---- fetch mode ---------------------------------------------------------------
if [ -n "$LOCAL" ]; then
	[ -f "$LOCAL" ] || die "$IMG_KEY is configured but the file is missing: $LOCAL"
	printf '%s\n' "$LOCAL"
	exit 0
fi
[ -n "$URL" ] || no_source

if [ -s "$DEST" ]; then
	echo "fetch-image: reusing existing $DEST (cache hit)" >&2
	printf '%s\n' "$DEST"
	exit 0
fi

echo "fetch-image: downloading the $FLAVOR boot disk" >&2
tmp="$DEST.download"
curl -fsSL "$URL" -o "$tmp"
# Accept a bare .chd or a .zip that contains one.
if unzip -l "$tmp" >/dev/null 2>&1; then
	command -v unzip >/dev/null 2>&1 || die "downloaded a zip but unzip is not available"
	xdir="$DEST.unzip"
	rm -rf "$xdir"
	unzip -q -o "$tmp" -d "$xdir"
	chd=$(find "$xdir" -iname '*.chd' | head -1)
	[ -n "$chd" ] || die "no .chd inside the downloaded zip"
	mv "$chd" "$DEST"
	rm -rf "$xdir" "$tmp"
else
	mv "$tmp" "$DEST"
fi
[ -s "$DEST" ] || die "download produced an empty file"
echo "fetch-image: $DEST ($(wc -c < "$DEST" | tr -d ' ') bytes)" >&2
printf '%s\n' "$DEST"
