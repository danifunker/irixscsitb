#!/bin/sh
# Fetch the PREBUILT iris + iris-ci CLI binaries from a danifunker/iris release,
# so CI (and local builds) don't have to clone + `cargo build` the emulator from
# source. That source build is the long pole of the o32 release job (Rust
# toolchain + clang/libclang for the chd feature + a full cargo build); a
# prebuilt download turns minutes into seconds.
#
# IRIS's release pipeline ships BOTH binaries, flat, inside the
# `IRIS-cli-<variant>-linux-<arch>-<ver>.tar.gz` archive:
#     iris  iris-ci  LICENSE  LICENSE-libchdman-rs.txt
# We drop iris + iris-ci into <dir>/target/release/ so that
#     scripts/iris-build.sh --iris-dir <dir>
# consumes them exactly as if you'd built there (it looks for
# <dir>/target/release/{iris,iris-ci}).
#
# WHY the `lightning` variant: it's the fastest end-user build and is compiled
# with the `chd` feature (boots .chd disks) + the always-on in-core NFS server
# that iris-build.sh mounts to move files in/out. It disables interactive
# debugging, which headless CI never needs. Every IRIS-cli-* variant carries
# iris-ci and chd; lightning is just the quickest.
#
# Usage:
#   scripts/fetch-iris.sh [--dir iris] [--repo danifunker/iris] [--tag latest]
#                         [--os linux|macos|windows] [--arch x64|arm64|riscv64]
#                         [--variant lightning]
#
# --os/--arch are auto-detected from uname, so the same call works on a
# GitHub-hosted Ubuntu runner, a self-hosted Mac, a riscv64 box, or Git Bash
# on Windows (zip assets; needs `unzip`). Extraction is layout-tolerant: the
# archive is unpacked whole and the two binaries are located wherever that
# target's packaging put them (flat, ./-prefixed, or nested target/.../release).
#
# Since iris release v2026-07-28-20-04 EVERY target's CLI archive bundles
# `iris-ci` (verified: linux x64/riscv64, macos-arm64, windows-x64, all flat).
# Older tags lacked it outside linux x64/arm64 — pinning --tag at one of those
# fails here with the workaround spelled out. Note also that
# scripts/iris-build.sh itself is validated on Linux/macOS hosts (Windows
# would need the TCP control socket; untested).
#
# In the release workflow this REPLACES the "Clone IRIS source" + "Build iris +
# iris-ci" steps of build-o32-native:
#     - name: Fetch prebuilt iris + iris-ci
#       env: { GH_TOKEN: ${{ github.token }} }
#       run: ./scripts/fetch-iris.sh --dir iris
# then keep copying the boot disk to iris/irix53.chd and calling iris-build.sh
# with --iris-dir iris exactly as before.
#
# Needs: `gh` (authenticated; preferred, matches the rb-cli fetch pattern) OR
# `curl`; plus `tar`. Honors $GH_TOKEN for the GitHub API.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"

DIR="iris"                 # output dir; binaries land in <DIR>/target/release/
SRC_REPO=""                # release repo; --repo > $IRIS_RELEASE_REPO/conf > default
TAG=""                     # release tag;  --tag  > $IRIS_TAG/conf > latest
VARIANT="lightning"        # IRIS-cli-<variant>-... ; lightning is chd+nfs, fastest
OS=""                      # auto-detected from uname -s if empty (linux / macos)
ARCH=""                    # auto-detected from uname -m if empty

die() { echo "fetch-iris: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir)     DIR="$2"; shift 2 ;;
		--repo)    SRC_REPO="$2"; shift 2 ;;
		--tag)     TAG="$2"; shift 2 ;;
		--os)      OS="$2"; shift 2 ;;
		--arch)    ARCH="$2"; shift 2 ;;
		--variant) VARIANT="$2"; shift 2 ;;
		-h|--help) sed -n '2,47p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf may set IRIS_RELEASE_REPO / IRIS_TAG
[ -n "$SRC_REPO" ] || SRC_REPO="${IRIS_RELEASE_REPO:-danifunker/iris}"
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

# Asset name shape from IRIS's release pipeline (version is embedded; match on
# the stable prefix + archive suffix so we don't have to know the version).
ASSET_RE="^IRIS-cli-${VARIANT}-${OS}-${ARCH}-.*${AEXT_RE}$"

echo ">>> resolving $VARIANT/$ARCH asset in $SRC_REPO ($TAG)"
URL=""
if command -v gh >/dev/null 2>&1; then
	URL=$(gh api "repos/$SRC_REPO/$RELPATH" \
		--jq ".assets[] | select(.name|test(\"$ASSET_RE\")) | .browser_download_url" \
		2>/dev/null | head -1) || URL=""
fi
# On an HTTP error gh prints the error JSON BODY to stdout (and head masks the
# exit code), so anything that isn't a https URL is a non-answer — fall through
# to the curl path, whose own failure leaves URL empty for the die below.
case "$URL" in https://*) ;; *) URL="" ;; esac
if [ -z "$URL" ]; then
	# curl fallback: pull the release JSON and grep the download URL directly.
	# (Auth header via an explicit branch rather than a ${VAR:+...} one-liner —
	# plainer to read, and immune to any shell's quoting-in-expansion quirks.)
	command -v curl >/dev/null 2>&1 || die "need gh or curl to fetch the release"
	API="https://api.github.com/repos/$SRC_REPO/$RELPATH"
	if [ -n "${GH_TOKEN:-}" ]; then
		_json=$(curl -fsSL -H "Authorization: Bearer $GH_TOKEN" \
			-H "Accept: application/vnd.github+json" "$API") || _json=""
	else
		_json=$(curl -fsSL -H "Accept: application/vnd.github+json" "$API") || _json=""
	fi
	URL=$(printf '%s' "$_json" \
		| grep -oE "https://[^\"]*IRIS-cli-${VARIANT}-${OS}-${ARCH}-[^\"]*\.${AEXT}" \
		| head -1)
fi
[ -n "$URL" ] || die "no IRIS-cli-$VARIANT-$OS-$ARCH asset found in $SRC_REPO $TAG"

echo ">>> downloading $URL"
DEST="$DIR/target/release"
mkdir -p "$DEST"
ARCHIVE="$DEST/.iris-cli.$AEXT"
if command -v curl >/dev/null 2>&1; then
	curl -fSL "$URL" -o "$ARCHIVE"
else
	# No curl: let gh fetch the asset by pattern (empty tag = latest release).
	[ "$TAG" = latest ] && _t="" || _t="$TAG"
	gh release download ${_t:+"$_t"} --repo "$SRC_REPO" \
		--pattern "IRIS-cli-${VARIANT}-${OS}-${ARCH}-*.${AEXT}" \
		--output "$ARCHIVE" --clobber
fi

echo ">>> extracting iris + iris-ci into $DEST"
# Unpack the whole archive into a scratch dir and locate the binaries wherever
# this target's packaging put them — the layouts genuinely differ per OS/arch
# job (flat on linux x64/arm64 + macos, ./-prefixed on linux-riscv64, nested
# target/<triple>/release/ inside the windows zips).
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
	die "the $OS-$ARCH release archive has no iris-ci — releases BEFORE
  v2026-07-28-20-04 only bundled it on linux x64/arm64. Use a newer --tag,
  or build it from source and drop it in place:
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
