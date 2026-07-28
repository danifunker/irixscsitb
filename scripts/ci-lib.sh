# scripts/ci-lib.sh — helpers shared by the build/release scripts so the local
# flow and the GitHub Actions workflow run the SAME code path. Sourced, never
# executed; the caller sets $REPO first and keeps its own die() (so error
# prefixes name the script the user actually ran).
#
# The flavor <-> configuration mapping lives here and nowhere else:
#   o32 -> IRIX53_IMAGE (local path) / IRIX53_DISK_URL (download) / BUILD_O32
#   n32 -> IRIX65_IMAGE              / IRIX65_DISK_URL             / BUILD_N32

CONF="${REPO:?ci-lib.sh: caller must set REPO}/ci/local.conf"

# conf_get KEY — value from ci/local.conf (per-machine, .gitignore'd).
# Parsed as KEY=VALUE, deliberately never sourced — no shell code runs from a
# config file. Surrounding double quotes on the value are stripped.
conf_get() {
	[ -f "$CONF" ] || return 0
	sed -n "s/^$1=//p" "$CONF" | head -1 | sed 's/^"//; s/"$//'
}

# load_local_conf — pull every recognised key out of ci/local.conf into the
# environment, WITHOUT overriding anything already set: command-line flags and
# real environment variables always win over the config file. Call once, right
# after argument parsing.
load_local_conf() {
	for _k in IRIX53_IMAGE IRIX65_IMAGE IRIX53_DISK_URL IRIX65_DISK_URL \
	          IRIS_DIR IRIS_RELEASE_REPO IRIS_TAG RB_CLI BUILD_O32 BUILD_N32; do
		_cur=$(eval "printf %s \"\${$_k:-}\"")
		[ -n "$_cur" ] && continue
		_v=$(conf_get "$_k")
		[ -n "$_v" ] || continue
		eval "$_k=\$_v"
		export "$_k"
	done
}

flavor_img_key() {
	case "$1" in
		o32) echo IRIX53_IMAGE ;;
		n32) echo IRIX65_IMAGE ;;
		*)   return 1 ;;
	esac
}

flavor_url_key() {
	case "$1" in
		o32) echo IRIX53_DISK_URL ;;
		n32) echo IRIX65_DISK_URL ;;
		*)   return 1 ;;
	esac
}

# resolve_local_image FLAVOR — print the locally-available boot disk path, or
# nothing. Callers run load_local_conf first, so the environment already
# reflects flag/env/conf precedence. (In Actions, the irix*_image dispatch
# inputs arrive as these same variables.)
resolve_local_image() {
	_k=$(flavor_img_key "$1") || return 1
	eval "printf %s \"\${$_k:-}\""
}

# resolve_disk_url FLAVOR — print the download URL for the flavor's boot disk
# (a repo secret in Actions), or nothing.
resolve_disk_url() {
	_k=$(flavor_url_key "$1") || return 1
	eval "printf %s \"\${$_k:-}\""
}

# flavor_enabled FLAVOR — BUILD_O32 / BUILD_N32 switches; enabled unless the
# value reads as an explicit "off" (0/no/false/off, any case).
flavor_enabled() {
	case "$1" in
		o32) _e="${BUILD_O32:-1}" ;;
		n32) _e="${BUILD_N32:-1}" ;;
		*)   return 1 ;;
	esac
	case "$_e" in
		0|[Nn][Oo]|[Ff][Aa][Ll][Ss][Ee]|[Oo][Ff][Ff]) return 1 ;;
		*) return 0 ;;
	esac
}
