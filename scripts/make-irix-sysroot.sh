#!/bin/sh
# Build an IRIX cross-compile sysroot tarball from an IRIS-emulator IRIX disk
# image with rb-cli. In the IRIS guest, install IRIX + the IDO (IRIS Development
# Option) so /usr/include (incl. sys/dsreq.h) and the crt objects under /usr/lib
# exist. Two modes:
#
#  ROBUST (recommended) -- tar inside IRIX, then pull that single file off:
#    1) in the IRIX guest, make one relative tar on the disk:
#         cd / && tar cf /var/tmp/irix-sysroot.tar ./usr/include ./usr/lib
#       (IRIX tar preserves symlinks like libc.so -> libc.so.1; a host tree-walk
#        does not, and macOS case-folding corrupts it.)
#    2) on the host:
#         scripts/make-irix-sysroot.sh --image IRIX.img[@N] \
#             --from-tar /var/tmp/irix-sysroot.tar --out irix-5.3-sysroot.tgz
#
#  DIRECT (fallback) -- host tree-walk via rb-cli. Use only on a CASE-SENSITIVE
#  host (e.g. a Linux box / CI runner; macOS will drop case-colliding files).
#  Recreates symlinks rb-cli would otherwise write as text:
#         scripts/make-irix-sysroot.sh --image IRIX.img@1 --direct \
#             --out irix-5.3-sysroot.tgz --abi o32
#
# Common: --rb-cli PATH ($RB_CLI or `rb-cli`), --abi o32|n32|both (direct mode).
set -eu

IMAGE=""; OUT=""; RB="${RB_CLI:-rb-cli}"; ABI="o32"; FROM_TAR=""; DIRECT=0; WORK=""

die() { echo "make-irix-sysroot: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--image)    IMAGE="$2"; shift 2 ;;
		--out)      OUT="$2"; shift 2 ;;
		--rb-cli)   RB="$2"; shift 2 ;;
		--abi)      ABI="$2"; shift 2 ;;
		--from-tar) FROM_TAR="$2"; shift 2 ;;
		--direct)   DIRECT=1; shift ;;
		--work)     WORK="$2"; shift 2 ;;
		-h|--help)  sed -n '2,38p' "$0"; exit 0 ;;
		*)          die "unknown option: $1" ;;
	esac
done

[ -n "$IMAGE" ] || die "missing --image (e.g. irix.chd@1)"
[ -n "$OUT" ]   || die "missing --out (e.g. irix-5.3-sysroot.tgz)"
command -v "$RB" >/dev/null 2>&1 || [ -x "$RB" ] || die "rb-cli not found: $RB"
[ -n "$FROM_TAR" ] || [ "$DIRECT" -eq 1 ] || die "pick a mode: --from-tar PATH (robust) or --direct"

WORK="${WORK:-$(mktemp -d)}"

if [ -n "$FROM_TAR" ]; then
	# Robust: pull the single, guest-made tar off the disk and (re)compress it.
	echo ">>> pulling $FROM_TAR off $IMAGE"
	"$RB" get --force "$IMAGE" "$FROM_TAR" "$WORK/sysroot.tar"
	case "$FROM_TAR" in
		*.gz|*.tgz) cp "$WORK/sysroot.tar" "$OUT" ;;     # already compressed
		*)          gzip -c "$WORK/sysroot.tar" > "$OUT" ;;
	esac
else
	# Direct host tree-walk. rb-cli writes symlinks as text and aborts on a
	# pre-existing destination, so use --skip-existing and recreate symlinks
	# from the "symlink as text: X -> Y" notices it prints.
	SYS="$WORK/sysroot"; LOG="$WORK/get.log"; mkdir -p "$SYS"; : > "$LOG"

	get_tree() {  # SRC DESTPARENT  (rb-cli lays SRC's basename under DESTPARENT)
		echo ">>> extracting $1"
		mkdir -p "$2"
		"$RB" get -r --skip-existing "$IMAGE" "$1" "$2" 2>>"$LOG" \
			|| echo "    (warning: $1 incomplete - see log)" >&2
	}
	get_tree /usr/include "$SYS/usr"
	case "$ABI" in o32|both) get_tree /usr/lib "$SYS/usr"; get_tree /lib "$SYS" ;; esac
	case "$ABI" in n32|both) get_tree /usr/lib32 "$SYS/usr"; get_tree /lib32 "$SYS" ;; esac

	# Recreate symlinks rb-cli emitted as text files.
	echo ">>> recreating symlinks"
	grep '^  *symlink as text: ' "$LOG" 2>/dev/null \
		| sed -E 's/^ *symlink as text: (.*) -> (.*) \(use.*/\1	\2/' \
		| while IFS="	" read -r lpath ltarget; do
			[ -n "$lpath" ] && [ -n "$ltarget" ] || continue
			rm -f "$lpath"; ln -s "$ltarget" "$lpath"
		done

	[ -f "$SYS/usr/include/sys/dsreq.h" ] || \
		echo "WARNING: sys/dsreq.h missing - IDO not installed, or extraction was truncated (macOS case-folding? use --from-tar)" >&2
	tar -C "$SYS" -czf "$OUT" .
fi

echo "done:"; ls -la "$OUT"
echo
echo "Next: upload $OUT to your private store, then set the GitHub Actions secret"
echo "IRIX_SYSROOT_URL to a (short-lived, signed) download URL for it."
