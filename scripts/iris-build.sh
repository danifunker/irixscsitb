#!/bin/sh
# Build irixscsitb natively inside IRIX via the IRIS emulator, moving files in
# and out on a WORK DISK — an SGI EFS hard-disk image assembled by rb-cli on
# the host, attached to the guest as a second SCSI drive. No networking is
# required in the guest at all: no DHCP, no NVRAM eaddr, no NFS. This is the
# headless-CI build path for both native flavors:
#
#   --flavor o32   IRIX 5.3 guest, `make` (o32/mips2 CLI + Motif GUI).
#                  Runs on every IRIX 5.3-6.5. Boots SINGLE-USER via sash, so
#                  none of the image's rc2 services run — no mediad, no xdm,
#                  no third-party daemons that could hang a headless boot
#                  (a tgcware prngd wedged multiuser boots during bring-up).
#   --flavor n32   IRIX 6.5 guest, `make irix-n32` (+ GUI, non-fatal).
#                  Faster binary, 6.x-only. Boots MULTIUSER (proven clean).
#
# WHY native-in-IRIS instead of a cross-compiler: GNU binutils cannot link
# IRIX 5.3's o32 shared libs (sgi1.0 RLD format) and 5.3 has no static libc.a,
# so a GNU cross-toolchain can compile but not LINK for 5.3. IRIX's own
# MIPSpro cc + ld (already on the boot disk) do it correctly — and they also
# build the Motif GUI, which no cross sysroot we ship carries headers for.
#
# WHY a work disk instead of NFS: the disk needs nothing configured inside the
# guest image (NFS needs a valid NVRAM MAC + an IP the NAT can route), it is
# bidirectional (sources in, binaries out), and rb-cli both creates it from a
# host folder (`new hd sgi-efs --from-dir`) and reads the results back out
# (`get IMG@1 ...`). The guest sees it as /dev/dsk/dks0d2s0.
#
# THE BOOT DISK IS NEVER MODIFIED. The ci/*.toml configs set `overlay = true`,
# so every guest write lands in `<image>.chd.diff.chd` next to the image;
# delete that file (or pass --fresh) to reset the guest to a pristine state.
#
# Prereqs:
#   - iris + iris-ci binaries: either build ../iris from source
#     (cargo build --release --features chd,jit,rex-jit,lightning) or fetch
#     the prebuilt release pair with scripts/fetch-iris.sh.
#   - an installed IRIX boot disk (.chd) with the dev tools: cc, make,
#     /usr/include/sys/dsreq.h; Motif headers if you want the GUI.
#     Root must have an EMPTY password (or set IRIX_ROOT_PASSWORD).
#   - rb-cli with `new hd sgi-efs --from-dir` (release 2026-07 or later).
#
# Usage:
#   scripts/iris-build.sh --flavor o32 [--image /path/to/irix53.chd] \
#       [--iris-dir ../iris] [--config ci/iris-irix53.toml] [--rb-cli rb-cli] \
#       [--outdir dist] [--workdir DIR] [--fresh] [--version V] \
#       [--no-package] [--no-gendist] [--bin-out PATH] [--gui-out PATH]
#
# PACKAGING BY THE OS THAT BUILT IT: unless --no-gendist, the same guest
# session also runs ITS OWN native gendist over inst/irixscsitb.{spec,idb},
# emitting the Software Manager product trio to $OUTDIR/inst53 (o32 flavor)
# or $OUTDIR/inst65 (n32) — a 5.3-format product from the 5.3 guest, a
# 6.5-format one from the 6.5 guest. If the guest has no /usr/sbin/gendist
# (the inst_dev.sw "Software Packager" subsystem), the step is skipped with a
# warning — scripts/iris-gendist.sh can provision it from the IDO CD.
#
# WHERE THE BOOT DISK COMES FROM (first match wins):
#   1. --image PATH
#   2. $IRIX53_IMAGE / $IRIX65_IMAGE (per flavor)
#   3. ci/local.conf — per-machine paths, .gitignore'd; copy
#      ci/local.conf.example and edit. It may also set IRIS_DIR.
#      Parsed (KEY=VALUE), never sourced — no shell code runs from it.
#
# The o32 flavor packages .iso/.hda/.tar.gz via scripts/package.sh when
# --version is given (skip with --no-package); the n32 flavor only ever emits
# binaries (the distributable images always carry the portable o32 build).
# --bin-out / --gui-out additionally copy the CLI / GUI binary to fixed paths
# (what CI uploads as artifacts).
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
. "$REPO/scripts/ci-lib.sh"        # conf_get + flavor->image/url key mapping
FLAVOR=""
IMAGE=""
IRIS_DIR="${IRIS_DIR:-}"       # --iris-dir > $IRIS_DIR env > conf > ../iris
CONFIG=""
RB=""                          # --rb-cli > $RB_CLI env/conf > rb-cli on PATH
VERSION=""
OUTDIR="$REPO/dist"
WORKDIR=""
FRESH=0
DO_PACKAGE=1
DO_GENDIST=1
BIN_OUT=""
GUI_OUT=""
ROOT_PW="${IRIX_ROOT_PASSWORD:-}"

die() { echo "iris-build: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--flavor)     FLAVOR="$2"; shift 2 ;;
		--image)      IMAGE="$2"; shift 2 ;;
		--iris-dir)   IRIS_DIR="$2"; shift 2 ;;
		--config)     CONFIG="$2"; shift 2 ;;
		--rb-cli)     RB="$2"; shift 2 ;;
		--version)    VERSION="$2"; shift 2 ;;
		--outdir)     OUTDIR="$2"; shift 2 ;;
		--workdir)    WORKDIR="$2"; shift 2 ;;
		--fresh)      FRESH=1; shift ;;
		--no-package) DO_PACKAGE=0; shift ;;
		--no-gendist) DO_GENDIST=0; shift ;;
		--bin-out)    BIN_OUT="$2"; shift 2 ;;
		--gui-out)    GUI_OUT="$2"; shift 2 ;;
		-h|--help)    sed -n '2,56p' "$0"; exit 0 ;;
		*)            die "unknown option: $1" ;;
	esac
done

load_local_conf   # ci/local.conf fills in whatever flags/env didn't set
[ -n "$RB" ] || RB="${RB_CLI:-rb-cli}"

# ---- validate arguments ----------------------------------------------------
case "$FLAVOR" in
	o32) : "${CONFIG:=$REPO/ci/iris-irix53.toml}"; IMG_KEY="IRIX53_IMAGE" ;;
	n32) : "${CONFIG:=$REPO/ci/iris-irix65.toml}"; IMG_KEY="IRIX65_IMAGE"; DO_PACKAGE=0 ;;
	*)   die "--flavor must be o32 or n32" ;;
esac

# Boot disk: --image > $IRIX53_IMAGE/$IRIX65_IMAGE env > ci/local.conf
# (resolution shared with fetch-image.sh via ci-lib.sh; use fetch-image.sh
# instead when the disk may need DOWNLOADING from a *_DISK_URL).
if [ -z "$IMAGE" ]; then
	IMAGE=$(resolve_local_image "$FLAVOR")
	[ -n "$IMAGE" ] || die "no boot disk for --flavor $FLAVOR: pass --image PATH, set \$$IMG_KEY, or copy ci/local.conf.example to ci/local.conf and set $IMG_KEY"
fi
[ -f "$IMAGE" ] || die "boot disk not found: $IMAGE"
IMAGE=$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")
CONFIG=$(cd "$(dirname "$CONFIG")" && pwd)/$(basename "$CONFIG")
[ -f "$CONFIG" ] || die "config not found: $CONFIG"
[ "$DO_PACKAGE" = 0 ] || [ -n "$VERSION" ] || die "packaging needs --version (or pass --no-package)"

# Emulator dir: --iris-dir > $IRIS_DIR env > ci/local.conf > ../iris (the
# conf was already folded into $IRIS_DIR by load_local_conf). A relative
# value is tried from the caller's cwd first, then from the repo root.
[ -n "$IRIS_DIR" ] || IRIS_DIR="$REPO/../iris"
if ! _resolved=$(cd "$IRIS_DIR" 2>/dev/null && pwd); then
	_resolved=$(cd "$REPO/$IRIS_DIR" 2>/dev/null && pwd) || die "iris dir not found: $IRIS_DIR"
fi
IRIS_DIR="$_resolved"
IRIS="$IRIS_DIR/target/release/iris"
CI_BIN="$IRIS_DIR/target/release/iris-ci"
[ -x "$IRIS" ] && [ -x "$CI_BIN" ] || die "need $IRIS + iris-ci: build ../iris or run scripts/fetch-iris.sh"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"

# ---- work area ---------------------------------------------------------------
# The CI control socket is a Unix socket: its path must stay under SUN_LEN
# (~104 bytes), so it lives in /tmp regardless of where the workdir is.
[ -n "$WORKDIR" ] || WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/irisbuild.XXXXXX")
mkdir -p "$WORKDIR"
WORKDIR=$(cd "$WORKDIR" && pwd)
SOCK="/tmp/iris-ci.$$.sock"
STAGE="$WORKDIR/stage"
HDA="$WORKDIR/work.hda"
CONSOLE="$WORKDIR/console.log"

CI() { IRIS_SOCKET="$SOCK" "$CI_BIN" "$@"; }

cleanup() {
	CI quit >/dev/null 2>&1 || true
	sleep 1
	kill "$(cat "$WORKDIR/iris.pid" 2>/dev/null)" 2>/dev/null || true
	rm -f "$SOCK"
}
trap cleanup EXIT INT TERM

# Serial helpers. iris-ci's own run/marker machinery is deliberately NOT used:
# it greps for "\nIRIS-CI-RC=", which any shell that colors its output (bash on
# the 6.5 disk) breaks by emitting ANSI resets before the marker. Instead every
# guest step ends in `echo TOKEN-'OK'` — quoted when TYPED so the command echo
# can't match, contiguous when PRINTED — and we wait for the printed form.
# iris-ci's socket client caps a single wait at 300 s, so long waits loop in
# 170 s rounds.
ser_send() { CI serial-send "$1"; }
ser_wait() { CI -q serial-wait "$1" --timeout "$2" >/dev/null 2>&1; }
ser_wait_long() { # pattern rounds-of-170s label
	_p="$1"; _n="$2"; _l="$3"
	while [ "$_n" -gt 0 ]; do
		ser_wait "$_p" 170 && return 0
		_n=$(($_n - 1))
		echo "    ... still waiting for $_l"
	done
	echo "iris-build: timed out waiting for $_l; last console output:" >&2
	tail -25 "$CONSOLE" >&2 || true
	return 1
}

# ---- 1. stage the sources ----------------------------------------------------
# Same manifest sync-irix-drop.sh ships to a real IRIX machine, minus the
# installer pieces. version.h is stamped HERE, where git exists; it travels to
# the guest as a static input (IRIX has no git — see CLAUDE.md).
echo ">>> staging sources into $STAGE"
sh "$REPO/scripts/mkversion.sh" "$REPO/version.h" >/dev/null
rm -rf "$STAGE"; mkdir -p "$STAGE/out"
for f in irixscsitb.c toolbox.c wifi.c gui_motif.c version.c irix.c \
         irixscsitb.h os.h Makefile version.h; do
	cp "$REPO/$f" "$STAGE/$f"
done
echo "build output lands here" > "$STAGE/out/README"
if [ "$DO_GENDIST" = 1 ]; then
	# The inst product description, version-stamped for THIS flavor. The
	# numeric inst version derives from --version (or a date stamp for
	# version-less CI binary builds).
	DISTVER=$(dist_version_from "${VERSION:-$(date -u +%Y-%m-%d-%H-%M)}")
	stage_inst_inputs "$FLAVOR" "$DISTVER" "$STAGE"
	mkdir -p "$STAGE/chest" "$STAGE/out/inst"
	cp "$REPO/desktop/scsitoolbox.chest" "$STAGE/chest/"
fi

# ---- 2. build the work disk ----------------------------------------------------
# 64 MB EFS, 16 heads x 63 sectors (the geometry the IRIS emulator models).
echo ">>> assembling work disk $HDA"
rm -f "$HDA"
"$RB" -q --progress never new hd sgi-efs "$HDA" --size 64M --heads 16 --sectors 63 \
	--from-dir "$STAGE"

# ---- 3. reset the overlay (optional) and launch iris ---------------------------
if [ "$FRESH" = 1 ]; then
	echo ">>> --fresh: dropping overlay ${IMAGE}.diff.chd"
	rm -f "${IMAGE}.diff.chd" "${IMAGE}.overlay" "${IMAGE}.overlay.dirty"
fi

echo ">>> launching IRIS (headless --ci, boot=$IMAGE, work=$HDA)"
( cd "$WORKDIR" && "$IRIS" --ci --config "$CONFIG" --ci-socket "$SOCK" \
	--scsi1 "$IMAGE" --scsi2 "$HDA" --serial-log "$CONSOLE" \
	> "$WORKDIR/iris.log" 2>&1 & echo $! > "$WORKDIR/iris.pid" )

i=0
until CI ping >/dev/null 2>&1; do
	i=$((i+1)); [ "$i" -lt 30 ] || die "iris control socket never came up (see $WORKDIR/iris.log)"
	sleep 1
done

# ---- 4. boot ---------------------------------------------------------------------
# The PROM menu is driven over serial, so this works for any hostname and any
# root shell. Overlay writes make it deterministic: every run starts from the
# same disk state (fully so with --fresh).
CI start
ser_wait "Option?" 90 || die "PROM menu never appeared (see $CONSOLE)"

if [ "$FLAVOR" = o32 ]; then
	# Single-user via the command monitor + sash (the PROM cannot read EFS
	# itself). initstate=s keeps every rc2 service out of the picture.
	echo ">>> booting single-user (command monitor -> sash -> unix initstate=s)"
	ser_send "5"
	ser_wait ">>" 30 || die "command monitor prompt not seen"
	ser_send "boot -f dksc(0,1,8)sash"
	ser_wait "sash" 60 || die "sash never loaded"
	sleep 1
	ser_send "boot -f dksc(0,1,0)unix initstate=s"
	ser_wait_long "Single User Mode" 2 "single-user prompt" || exit 1
	# "give root password for Single User Mode" — empty unless configured.
	ser_send "$ROOT_PW"
	sleep 2
else
	echo ">>> booting multiuser"
	ser_send "1"
	ser_wait_long "console login" 3 "console login prompt" || exit 1
	if [ -n "$ROOT_PW" ]; then
		CI -q login root --password "$ROOT_PW"
	else
		CI -q login root
	fi
fi

# ---- 5. mount the work disk, build, copy out --------------------------------------
# /mnt may not exist on a 5.3 disk; -p makes both cases a no-op. Building in
# /tmp keeps object files off the work disk and inside the overlay.
echo ">>> mounting work disk + building ($FLAVOR)"
# test -d, not mkdir -p: IRIX 5.3's mkdir -p errors when the dir already
# exists (bites on a reused overlay).
ser_send "test -d /mnt || mkdir /mnt ; mount /dev/dsk/dks0d2s0 /mnt && echo IRIXTB-'MNT'-OK"
ser_wait "IRIXTB-MNT-OK" 60 || { echo "work disk mount failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }

# Every guest line below is deliberately csh-AND-sh clean (`;`, `&&`, `||`,
# `( )` — no `$?`, no `{ }`, no redirects), because root's login shell varies
# by image (stock IRIX root is csh; both dev disks here use bash).
if [ "$FLAVOR" = o32 ]; then
	BUILD_CMD="make"           # detect: uname IRIX -> o32 CLI + GUI (GUI non-fatal)
	CLI_NAME="irixscsitb-o32"; GUI_NAME="scsitbgui-o32"
else
	# On an IP22 6.5 guest uname says IRIX (not IRIX64), so ask for n32
	# explicitly; keep the GUI attempt non-fatal like the detect target does.
	BUILD_CMD="make irix-n32 && (make irix-gui-n32 || echo GUI skipped)"
	CLI_NAME="irixscsitb-n32"; GUI_NAME="scsitbgui-n32"
fi

ser_send "rm -rf /tmp/bsbuild; mkdir /tmp/bsbuild && cp /mnt/*.c /mnt/*.h /mnt/Makefile /tmp/bsbuild && cd /tmp/bsbuild && $BUILD_CMD && echo IRIXTB-'BUILD'-OK || echo IRIXTB-'BUILD'-FAIL"
ser_wait_long "IRIXTB-BUILD-OK" 4 "native compile" || exit 1

ser_send "cp irixscsitb /mnt/out/$CLI_NAME && echo IRIXTB-'CLIOUT'-OK"
ser_wait "IRIXTB-CLIOUT-OK" 60 || { echo "CLI copy-out failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }
ser_send "(test -f scsitbgui && cp scsitbgui /mnt/out/$GUI_NAME); echo IRIXTB-'GUIOUT'-DONE"
ser_wait "IRIXTB-GUIOUT-DONE" 60 || true

# ---- 5b. package with the guest's OWN gendist ---------------------------------------
# The OS that built the binaries also packages them: its native gendist emits
# a Software Manager product in its own inst format (5.3-format from the 5.3
# guest — readable by every inst 5.3-6.5 — and 6.5-format from 6.5).
if [ "$DO_GENDIST" = 1 ]; then
	echo ">>> packaging with the guest's own gendist"
	ser_send "test -x /usr/sbin/gendist && echo PK-'TOOL'-YES || echo PK-'TOOL'-NO"
	if ser_wait "PK-TOOL-YES" 20; then
		ser_send "rm -rf /tmp/pk && mkdir /tmp/pk /tmp/pk/bin /tmp/pk/dist && cp /mnt/irixscsitb.spec /mnt/irixscsitb.idb /tmp/pk && cp -r /mnt/chest /tmp/pk && cp irixscsitb /tmp/pk/bin/ && (test -f scsitbgui && cp scsitbgui /tmp/pk/bin/) ; echo PK-'STAGE'-OK"
		ser_wait "PK-STAGE-OK" 60 || { echo "gendist staging failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }
		# idb filter: drop the GUI line — and its Toolchest launcher — when
		# no GUI was built (an image without Motif legitimately has none).
		# Plain grep: 5.3's old awk can't be trusted with system().
		ser_send "cd /tmp/pk && (test -f bin/scsitbgui && cp irixscsitb.idb idb.f || grep -v scsitbgui irixscsitb.idb | grep -v scsitoolbox > idb.f) && gendist -verbose -sbase /tmp/pk -idb /tmp/pk/idb.f -spec /tmp/pk/irixscsitb.spec -dist /tmp/pk/dist -all && cp dist/* /mnt/out/inst/ && cd /tmp/bsbuild && echo PK-'GEN'-OK || echo PK-'GEN'-FAIL"
		ser_wait_long "PK-GEN-OK" 2 "gendist" || { tail -20 "$CONSOLE" >&2; exit 1; }
	else
		echo ">>> WARNING: guest has no /usr/sbin/gendist (inst_dev.sw not installed);"
		echo ">>>          skipping the Software Manager product for $FLAVOR."
		echo ">>>          scripts/iris-gendist.sh can provision it from the IDO CD."
		DO_GENDIST=0
	fi
fi

ser_send "cd / && umount /mnt && sync && echo IRIXTB-'XFER'-OK"
ser_wait "IRIXTB-XFER-OK" 90 || { echo "umount/sync failed:" >&2; tail -10 "$CONSOLE" >&2; exit 1; }

CI quit >/dev/null 2>&1 || true
sleep 2

# ---- 6. pull the binaries off the work disk ----------------------------------------
echo ">>> extracting binaries from the work disk"
mkdir -p "$OUTDIR"
"$RB" -q get --force "$HDA@1" "/out/$CLI_NAME" "$OUTDIR/$CLI_NAME"
[ -s "$OUTDIR/$CLI_NAME" ] || die "no $CLI_NAME on the work disk (build failed? see $CONSOLE)"
GUI_BUILT=0
if "$RB" ls "$HDA@1" /out 2>/dev/null | grep -q "$GUI_NAME"; then
	"$RB" -q get --force "$HDA@1" "/out/$GUI_NAME" "$OUTDIR/$GUI_NAME"
	GUI_BUILT=1
fi
if [ "$DO_GENDIST" = 1 ]; then
	case "$FLAVOR" in o32) INST_OUT="$OUTDIR/inst53" ;; *) INST_OUT="$OUTDIR/inst65" ;; esac
	mkdir -p "$INST_OUT"
	for f in irixscsitb irixscsitb.idb irixscsitb.sw; do
		"$RB" -q get --force "$HDA@1" "/out/inst/$f" "$INST_OUT/$f"
		[ -s "$INST_OUT/$f" ] || die "gendist product file $f missing from the work disk (see $CONSOLE)"
	done
	echo ">>> Software Manager product ($FLAVOR): $INST_OUT"
fi

echo ">>> built:"
file "$OUTDIR/$CLI_NAME" || true
[ "$GUI_BUILT" = 1 ] && file "$OUTDIR/$GUI_NAME" || echo "    (GUI not built on this image - CLI is fine)"

if [ -n "$BIN_OUT" ]; then cp "$OUTDIR/$CLI_NAME" "$BIN_OUT"; echo ">>> CLI binary: $BIN_OUT"; fi
if [ -n "$GUI_OUT" ] && [ "$GUI_BUILT" = 1 ]; then cp "$OUTDIR/$GUI_NAME" "$GUI_OUT"; echo ">>> GUI binary: $GUI_OUT"; fi

# ---- 7. package (o32 only, unless suppressed) --------------------------------------
# Single-flavor convenience packaging: media with a dist53/ entry only.
# Full dual-flavor media come from package-dist.sh after both builds.
if [ "$DO_PACKAGE" = 1 ]; then
	echo ">>> packaging distributable artifacts"
	set -- --version "$VERSION" --outdir "$OUTDIR" --rb-cli "$RB" \
	       --extra "$REPO/README.md" --bin53 "$OUTDIR/$CLI_NAME"
	[ "$GUI_BUILT" = 1 ] && set -- "$@" --gui53 "$OUTDIR/$GUI_NAME"
	[ "$DO_GENDIST" = 1 ] && set -- "$@" --inst53-dir "$OUTDIR/inst53"
	"$REPO/scripts/package.sh" "$@"
fi

echo
echo "Done. Native $FLAVOR build; boot disk untouched (writes in ${IMAGE}.diff.chd)."
echo "Work dir kept for inspection: $WORKDIR"
