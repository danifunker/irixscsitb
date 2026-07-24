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
#                         [--arch x64|arm64] [--variant lightning]
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

DIR="iris"                 # output dir; binaries land in <DIR>/target/release/
REPO="danifunker/iris"     # release repo (the fork that publishes iris + iris-ci)
TAG="latest"               # "latest" or a specific release tag (e.g. v1.2.3)
VARIANT="lightning"        # IRIS-cli-<variant>-... ; lightning is chd+nfs, fastest
ARCH=""                    # auto-detected from uname -m if empty

die() { echo "fetch-iris: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--dir)     DIR="$2"; shift 2 ;;
		--repo)    REPO="$2"; shift 2 ;;
		--tag)     TAG="$2"; shift 2 ;;
		--arch)    ARCH="$2"; shift 2 ;;
		--variant) VARIANT="$2"; shift 2 ;;
		-h|--help) sed -n '2,44p' "$0"; exit 0 ;;
		*)         die "unknown option: $1" ;;
	esac
done

# Map the host machine to the release archive's arch token (x64 / arm64).
if [ -z "$ARCH" ]; then
	case "$(uname -m)" in
		x86_64|amd64)   ARCH="x64" ;;
		aarch64|arm64)  ARCH="arm64" ;;
		*)              die "unsupported arch $(uname -m); pass --arch x64|arm64" ;;
	esac
fi

# The release API path: latest release vs a specific tag.
case "$TAG" in
	latest) RELPATH="releases/latest" ;;
	*)      RELPATH="releases/tags/$TAG" ;;
esac

# Asset name shape from IRIS's release pipeline (version is embedded; match on
# the stable prefix + .tar.gz suffix so we don't have to know the version).
ASSET_RE="^IRIS-cli-${VARIANT}-linux-${ARCH}-.*\\.tar\\.gz$"

echo ">>> resolving $VARIANT/$ARCH asset in $REPO ($TAG)"
URL=""
if command -v gh >/dev/null 2>&1; then
	URL=$(gh api "repos/$REPO/$RELPATH" \
		--jq ".assets[] | select(.name|test(\"$ASSET_RE\")) | .browser_download_url" \
		2>/dev/null | head -1)
fi
if [ -z "$URL" ]; then
	# curl fallback: pull the release JSON and grep the download URL directly.
	command -v curl >/dev/null 2>&1 || die "need gh or curl to fetch the release"
	API="https://api.github.com/repos/$REPO/$RELPATH"
	# shellcheck disable=SC2086 # intentional: optional auth header expands to nothing
	URL=$(curl -fsSL ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} \
		-H "Accept: application/vnd.github+json" "$API" \
		| grep -oE "https://[^\"]*IRIS-cli-${VARIANT}-linux-${ARCH}-[^\"]*\.tar\.gz" \
		| head -1)
fi
[ -n "$URL" ] || die "no IRIS-cli-$VARIANT-linux-$ARCH asset found in $REPO $TAG"

echo ">>> downloading $URL"
DEST="$DIR/target/release"
mkdir -p "$DEST"
TARBALL="$DEST/.iris-cli.tar.gz"
if command -v curl >/dev/null 2>&1; then
	curl -fSL "$URL" -o "$TARBALL"
else
	# No curl: let gh fetch the asset by pattern (empty tag = latest release).
	[ "$TAG" = latest ] && _t="" || _t="$TAG"
	gh release download ${_t:+"$_t"} --repo "$REPO" \
		--pattern "IRIS-cli-${VARIANT}-linux-${ARCH}-*.tar.gz" \
		--output "$TARBALL" --clobber
fi

echo ">>> extracting iris + iris-ci into $DEST"
# The archive is flat (iris, iris-ci, LICENSEs at the root) — extract just the
# two binaries into <dir>/target/release/.
tar -C "$DEST" -xzf "$TARBALL" iris iris-ci
rm -f "$TARBALL"
chmod +x "$DEST/iris" "$DEST/iris-ci"

echo ">>> verifying"
"$DEST/iris" --version
"$DEST/iris-ci" --help >/dev/null   # exits 0 if the boot/login/run client is intact
echo ">>> ready: $DEST/iris + $DEST/iris-ci  (drive with scripts/iris-build.sh --iris-dir $DIR)"
