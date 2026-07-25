#!/bin/sh
# Enable DHCP (proclaim) on an IRIX 5.3 disk image by turning on the two
# chkconfig flags that gate network startup and the DHCP client.
#
# Usage: irix-enable-dhcp.sh IMAGE
#   IMAGE is required - any installed IRIX 5.3 boot disk.
#   IMAGE may be a .chd or a raw file. CHD images are extracted to a temp
#   raw file, modified, and repacked in place. Any sibling .diff.chd is
#   deleted (it would be stale after the base CHD changes).
#
# After this runs, boot the image once — IRIX will run /usr/etc/proclaim,
# pick up an IP from the DHCP server (IRIS NAT → 192.168.0.2), then turn
# autoconfig_ipaddress back off. Subsequent boots use that acquired address.
#
# Requirements: rb-cli (set RB_CLI=/path), chdman on PATH for .chd images.

set -e

RB="${RB_CLI:-rb-cli}"
CHDMAN="${CHDMAN:-chdman}"

# Required. There is deliberately no default: a default pointing at one
# person's local disk image is useless to everyone else and leaks a path.
IMAGE="${1:-}"
[ -n "$IMAGE" ] || { echo "usage: $0 IMAGE   (an installed IRIX 5.3 boot disk, .chd or raw)" >&2; exit 1; }
# Strip any @N suffix for the file-level operations; we always target @1.
BASE_IMAGE="${IMAGE%%@*}"

die() { echo "error: $*" >&2; exit 1; }

command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found — set RB_CLI=/path/to/rb-cli"

# Temp file holding "on\n" written to each flag.
ON_FILE="$(mktemp)"
printf 'on\n' > "$ON_FILE"

WORK_RAW=""
cleanup() {
    rm -f "$ON_FILE"
    [ -n "$WORK_RAW" ] && rm -f "$WORK_RAW"
}
trap cleanup EXIT

# Determine whether we need CHD ↔ raw conversion.
case "$BASE_IMAGE" in
    *.chd|*.CHD)
        command -v "$CHDMAN" >/dev/null 2>&1 || die "chdman not found — needed for .chd images (set CHDMAN=/path)"
        WORK_RAW="$(mktemp -t irix-dhcp).raw"
        echo "Extracting CHD → raw (this may take a moment)..."
        "$CHDMAN" extractraw -i "$BASE_IMAGE" -o "$WORK_RAW" >/dev/null 2>&1
        TARGET="${WORK_RAW}@1"
        ;;
    *)
        TARGET="${BASE_IMAGE}@1"
        ;;
esac

echo "Enabling network + DHCP client on: $BASE_IMAGE"

echo "  /etc/config/network               off -> on"
"$RB" put --force "$TARGET" "$ON_FILE" /etc/config/network

echo "  /etc/config/autoconfig_ipaddress  off -> on"
"$RB" put --force "$TARGET" "$ON_FILE" /etc/config/autoconfig_ipaddress

# If we worked on a raw copy, repack it back to CHD and remove any stale diff.
if [ -n "$WORK_RAW" ]; then
    echo "Repacking raw → CHD..."
    "$CHDMAN" createraw -i "$WORK_RAW" -o "$BASE_IMAGE" --force >/dev/null 2>&1
    DIFF="${BASE_IMAGE}.diff.chd"
    if [ -f "$DIFF" ]; then
        echo "Removing stale diff: $DIFF"
        rm -f "$DIFF"
    fi
fi

echo "Done. Boot the image once to let proclaim acquire an IP via DHCP."
