#!/bin/sh
# Fetch the PREBUILT iris + iris-ci CLI binaries from an iris release, so CI
# (and local builds) don't have to clone + `cargo build` the emulator from
# source. That source build is the long pole of a native release job (Rust
# toolchain + clang/libclang for the chd feature + a full cargo build); a
# prebuilt download turns minutes into seconds.
#
# WHERE FROM: the upstream emulator, github.com/techomancer/iris (default;
# `--repo`, $IRIS_RELEASE_REPO or ci/local.conf override it). The old
# danifunker/iris fork publishes no releases any more - its API answers 404 -
# so nothing here falls back to it.
#
# WHAT: since v2026-08-31-16-28 upstream ships ONE CLI build per platform,
# named without any variant token:
#     IRIS-cli-<os>-<arch>-<ver>.tar.gz      linux / macos
#     IRIS-cli-<os>-<arch>-<ver>.zip         windows
# each holding, flat:  iris  iris-ci  LICENSE  LICENSE-libchdman-rs.txt
# The build features (opcodefusion rex-jit lightning tlbvmap chd camera) are
# all in; the emulated CPU (R4400 / R5000) is a runtime setting, which is why
# the per-CPU `r4400`/`r5000` archives - and the `lightning` ones before them
# - are gone. The asset match still tolerates a `IRIS-cli-<variant>-<os>-...`
# name, so a repo that publishes the old shape keeps working with --repo.
#
# We drop iris + iris-ci into <dir>/target/release/ so that
#     scripts/iris-build.sh --iris-dir <dir>
# consumes them exactly as if you'd built there (it looks for
# <dir>/target/release/{iris,iris-ci}).
#
# Usage:
#   scripts/fetch-iris.sh [--dir iris] [--repo techomancer/iris] [--tag latest]
#                         [--os linux|macos|windows] [--arch x64|arm64|riscv64]
#                         [--resolve-only]
#
# --os/--arch are auto-detected from uname, so the same call works on a
# GitHub-hosted Ubuntu runner, a self-hosted Mac, a riscv64 box, or Git Bash
# on Windows (zip assets; needs `unzip`). --resolve-only prints the asset URL
# it would download and stops - handy for checking a tag or a foreign
# os/arch pair without pulling the archive. Extraction is layout-tolerant: the
# archive is unpacked whole and the two binaries are located wherever that
# target's packaging put them (flat today; ./-prefixed or nested
# target/<triple>/release/ have both been seen).
#
# Every upstream CLI archive bundles `iris-ci` on every target (linux
# x64/arm64/riscv64, macos x64/arm64, windows x64/arm64 - checked against
# v2026-08-31-16-28). Note that scripts/iris-build.sh itself is validated on
# Linux/macOS hosts (Windows would need the TCP control socket; untested).
#
# In the release workflow this is the "Fetch prebuilt iris + iris-ci" step:
#     - name: Fetch prebuilt iris + iris-ci
#       run: ./scripts/fetch-iris.sh --dir iris
# followed by iris-build.sh --iris-dir iris.
#
# Needs: `gh` (authenticated; preferred, matches the rb-cli fetch pattern) OR
# `curl`; plus `tar`. Honors $GH_TOKEN for the GitHub API.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

DIR="iris"                 # output dir; binaries land in <DIR>/target/release/
SRC_REPO=""                # release repo; --repo > $IRIS_RELEASE_REPO/conf > default
TAG=""                     # release tag;  --tag  > $IRIS_TAG/conf > latest
OS=""                      # auto-detected from uname -s if empty (linux / macos)
ARCH=""                    # auto-detected from uname -m if empty
RESOLVE_ONLY=0             # print the asset URL and stop

die() { echo "fetch-iris: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir)          DIR="$2"; shift 2 ;;
		--repo)         SRC_REPO="$2"; shift 2 ;;
		--tag)          TAG="$2"; shift 2 ;;
		--os)           OS="$2"; shift 2 ;;
		--arch)         ARCH="$2"; shift 2 ;;
		--resolve-only) RESOLVE_ONLY=1; shift ;;
		--variant)      die "--variant is gone: upstream ships one CLI build per platform (see the header)" ;;
		-h|--help)      sed -n '2,/^set -eu$/{/^set -eu$/!p;}' "$0"; exit 0 ;;
		*)              die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf may set IRIS_RELEASE_REPO / IRIS_TAG
[ -n "$SRC_REPO" ] || SRC_REPO="${IRIS_RELEASE_REPO:-techomancer/iris}"
[ -n "$TAG" ]      || TAG="${IRIS_TAG:-latest}"

# Map the host to the release archive's OS + arch tokens.
if [ -z "$OS" ]; then
	case "$(uname -s)" in
		Linux)                          OS="linux" ;;
		Darwin)                         OS="macos" ;;
		MINGW*|MSYS*|CYGWIN*|Windows*)  OS="windows" ;;
		*)  die "unsupported OS $(uname -s); pass --os linux|macos|windows" ;;
	esac
fi
if [ -z "$ARCH" ]; then
	case "$(uname -m)" in
		x86_64|amd64)   ARCH="x64" ;;
		aarch64|arm64)  ARCH="arm64" ;;
		riscv64)        ARCH="riscv64" ;;
		*)              die "unsupported arch $(uname -m); pass --arch x64|arm64|riscv64" ;;
	esac
fi

# Windows CLI releases are .zip; everything else is .tar.gz.
case "$OS" in
	windows) AEXT="zip";    AEXT_RE='\\.zip'      ;;
	*)       AEXT="tar.gz"; AEXT_RE='\\.tar\\.gz' ;;
esac

# The release API path: latest release vs a specific tag.
case "$TAG" in
	latest) RELPATH="releases/latest" ;;
	*)      RELPATH="releases/tags/$TAG" ;;
esac

# Asset name shape: IRIS-cli-<os>-<arch>-<ver>.<ext>, with an optional variant
# token between "cli-" and the OS for repos that still publish the old shape.
# The version is embedded, so match on the stable parts only.
ASSET_RE="^IRIS-cli-([a-z0-9]+-)?${OS}-${ARCH}-.*${AEXT_RE}\$"

resolve_url() {	# echoes the asset download URL, or nothing
	_url=""
	if command -v gh >/dev/null 2>&1; then
		_url=$(gh api "repos/$SRC_REPO/$RELPATH" \
			--jq ".assets[] | select(.name|test(\"$ASSET_RE\")) | .browser_download_url" \
			2>/dev/null | head -1) || _url=""
	fi
	# On an HTTP error gh prints the error JSON BODY to stdout (and head masks
	# the exit code), so anything that isn't a https URL is a non-answer - fall
	# through to the curl path, whose own failure leaves _url empty.
	case "$_url" in https://*) ;; *) _url="" ;; esac
	if [ -z "$_url" ]; then
		# curl fallback: pull the release JSON and grep the download URL directly.
		# (Auth header via an explicit branch rather than a ${VAR:+...} one-liner -
		# plainer to read, and immune to any shell's quoting-in-expansion quirks.)
		command -v curl >/dev/null 2>&1 || die "need gh or curl to fetch the release"
		API="https://api.github.com/repos/$SRC_REPO/$RELPATH"
		if [ -n "${GH_TOKEN:-}" ]; then
			_json=$(curl -fsSL -H "Authorization: Bearer $GH_TOKEN" \
				-H "Accept: application/vnd.github+json" "$API") || _json=""
		else
			_json=$(curl -fsSL -H "Accept: application/vnd.github+json" "$API") || _json=""
		fi
		_url=$(printf '%s' "$_json" \
			| grep -oE "https://[^\"]*IRIS-cli-([a-z0-9]+-)?${OS}-${ARCH}-[^\"]*\.${AEXT}" \
			| head -1)
	fi
	printf '%s' "$_url"
}

echo ">>> resolving the $OS-$ARCH CLI asset in $SRC_REPO ($TAG)" >&2
URL=$(resolve_url)
[ -n "$URL" ] || die "no IRIS-cli-$OS-$ARCH-*.$AEXT asset in $SRC_REPO $TAG.
  Upstream (techomancer/iris) publishes that shape from v2026-08-31-16-28 on;
  check the tag exists there, or point --repo / IRIS_RELEASE_REPO elsewhere."

if [ "$RESOLVE_ONLY" -eq 1 ]; then
	echo "$URL"
	exit 0
fi

echo ">>> downloading $URL"
DEST="$DIR/target/release"
mkdir -p "$DEST"
ARCHIVE="$DEST/.iris-cli.$AEXT"
if command -v curl >/dev/null 2>&1; then
	curl -fSL "$URL" -o "$ARCHIVE"
else
	# No curl: let gh fetch the asset by pattern (empty tag = latest release).
	# The glob's leading * also admits an old-style variant token.
	[ "$TAG" = latest ] && _t="" || _t="$TAG"
	gh release download ${_t:+"$_t"} --repo "$SRC_REPO" \
		--pattern "IRIS-cli-*${OS}-${ARCH}-*.${AEXT}" \
		--output "$ARCHIVE" --clobber
fi

echo ">>> extracting iris + iris-ci into $DEST"
# Unpack the whole archive into a scratch dir and locate the binaries wherever
# this target's packaging put them - flat today, but ./-prefixed and nested
# target/<triple>/release/ layouts have both shipped before.
UNPACK="$DEST/.iris-unpack.$$"
rm -rf "$UNPACK"; mkdir -p "$UNPACK"
case "$ARCHIVE" in
	*.zip) command -v unzip >/dev/null 2>&1 || die "need unzip for the windows archives"
	       unzip -q -o "$ARCHIVE" -d "$UNPACK" ;;
	*)     tar -C "$UNPACK" -xzf "$ARCHIVE" ;;
esac
rm -f "$ARCHIVE"

find_bin() { find "$UNPACK" -type f \( -name "$1" -o -name "$1.exe" \) | head -1; }
IRIS_F=$(find_bin iris)
CI_F=$(find_bin iris-ci)
[ -n "$IRIS_F" ] || { rm -rf "$UNPACK"; die "no iris binary inside the $OS-$ARCH archive"; }
if [ -z "$CI_F" ]; then
	rm -rf "$UNPACK"
	die "the $OS-$ARCH archive has no iris-ci. Every upstream CLI archive from
  v2026-08-31-16-28 on bundles it; an older or foreign release may not. Use a
  newer --tag, or build it from source and drop it in place:
      (cd ../iris && cargo build --release --bin iris-ci --features chd)
      cp ../iris/target/release/iris-ci $DEST/
  or skip fetch-iris.sh entirely and point iris-build.sh --iris-dir at a
  source-built iris checkout."
fi

# Normalise to <dir>/target/release/iris[.exe] + iris-ci[.exe].
IRIS_EXT=""; case "$IRIS_F" in *.exe) IRIS_EXT=".exe" ;; esac
CI_EXT="";   case "$CI_F"   in *.exe) CI_EXT=".exe"   ;; esac
mv "$IRIS_F" "$DEST/iris$IRIS_EXT"
mv "$CI_F"   "$DEST/iris-ci$CI_EXT"
rm -rf "$UNPACK"
chmod +x "$DEST/iris$IRIS_EXT" "$DEST/iris-ci$CI_EXT"

echo ">>> verifying"
# NB not --version: iris has no such flag (it would exit 2 and, under set -eu,
# fail the fetch). --help exercises arg parsing + dynamic linking just as well.
"$DEST/iris$IRIS_EXT" --help >/dev/null
"$DEST/iris-ci$CI_EXT" --help >/dev/null   # exits 0 if the boot/login/run client is intact
echo ">>> ready: $DEST/iris$IRIS_EXT + $DEST/iris-ci$CI_EXT  (drive with scripts/iris-build.sh --iris-dir $DIR)"
