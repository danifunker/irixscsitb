# scripts/ci-lib.sh — helpers shared by the build/release scripts so the local
# flow and the GitHub Actions workflow run the SAME code path. Sourced, never
# executed; the caller sets $REPO first and keeps its own die() (so error
# prefixes name the script the user actually ran).
#
# The flavor table lives here and nowhere else:
#
#   flavor  ISA    guest  dist key  runs on                    switch
#   o32     mips2  5.3    53        IRIX 5.3-6.5, R4000 and up  BUILD_O32
#   mips1   mips1  5.3    mips1     IRIX 5.3-6.5, ANY MIPS CPU  BUILD_MIPS1
#                                   (the R3000 machines - IP12 Indigo et al.)
#   n32     mips3  6.5    65        IRIX 6.x                    BUILD_N32
#
# The guest picks the boot disk (5.3 -> IRIX53_IMAGE / IRIX53_DISK_URL, 6.5 ->
# IRIX65_*), so o32 and mips1 build from the same image. The dist key names
# everything the flavor produces downstream: <outdir>/inst<key>/ (gendist
# product), /dist<key>/ on the media, bin<key>/ in the tarball and
# irixscsitb-<version>-<key>.tardist.
#
# mips1 is its own flavor rather than the o32 build lowered to MIPS I, so the
# machines that can run mips2 keep the better code: an R3000 refuses a mips2
# ELF outright ("Program not supported by architecture").
FLAVORS="o32 mips1 n32"

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
	          IRIS_DIR IRIS_RELEASE_REPO IRIS_TAG RB_CLI BUILD_O32 BUILD_N32 \
	          BUILD_MIPS1 BUILD_INST IRIX53_IDO_ISO; do
		_cur=$(eval "printf %s \"\${$_k:-}\"")
		[ -n "$_cur" ] && continue
		_v=$(conf_get "$_k")
		[ -n "$_v" ] || continue
		eval "$_k=\$_v"
		export "$_k"
	done
}

# flavor_guest FLAVOR — which IRIX release builds (and packages) it: 53 | 65.
flavor_guest() {
	case "$1" in
		o32|mips1) echo 53 ;;
		n32)       echo 65 ;;
		*)         return 1 ;;
	esac
}

# flavor_dist_key FLAVOR — the suffix of everything the flavor ships as
# (inst<key>/, /dist<key>, bin<key>/, -<key>.tardist).
flavor_dist_key() {
	case "$1" in
		o32)   echo 53 ;;
		mips1) echo mips1 ;;
		n32)   echo 65 ;;
		*)     return 1 ;;
	esac
}

flavor_img_key() {
	_g=$(flavor_guest "$1") || return 1
	echo "IRIX${_g}_IMAGE"
}

flavor_url_key() {
	_g=$(flavor_guest "$1") || return 1
	echo "IRIX${_g}_DISK_URL"
}

# flavor_abi_desc FLAVOR — one line for humans: the inst subsystem id and the
# media README both use it.
flavor_abi_desc() {
	case "$1" in
		o32)   echo "o32/mips2, IRIX 5.3-6.5, R4000 and up" ;;
		mips1) echo "o32/mips1, IRIX 5.3-6.5, any CPU incl. R3000" ;;
		n32)   echo "n32/mips3, IRIX 6.x only" ;;
		*)     return 1 ;;
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

# switch_on VALUE — a BUILD_* toggle reads as on unless it is an explicit
# "off" (0/no/false/off, any case). Empty (unset) means on.
switch_on() {
	case "$1" in
		0|[Nn][Oo]|[Ff][Aa][Ll][Ss][Ee]|[Oo][Ff][Ff]) return 1 ;;
		*) return 0 ;;
	esac
}

# flavor_enabled FLAVOR — BUILD_O32 / BUILD_MIPS1 / BUILD_N32 switches;
# enabled unless the value reads as an explicit "off" (0/no/false/off, any case).
flavor_enabled() {
	case "$1" in
		o32)   _e="${BUILD_O32:-1}" ;;
		mips1) _e="${BUILD_MIPS1:-1}" ;;
		n32)   _e="${BUILD_N32:-1}" ;;
		*)     return 1 ;;
	esac
	switch_on "$_e"
}

# inst_enabled — BUILD_INST: are the Software Manager products part of this
# release? Read by iris-build.sh (run the guest's gendist, and REQUIRE it),
# package-dist.sh (require the product dirs rather than silently falling back
# to raw binaries) and release-local.sh (--skip-inst). One switch, so a
# release can never half-decide: either every built flavor ships an inst
# product or none do.
inst_enabled() { switch_on "${BUILD_INST:-1}"; }

# stage_inst_inputs FLAVOR DISTVER DESTDIR — write the version-stamped,
# flavor-specific inst product description (irixscsitb.spec + .idb) into
# DESTDIR. One subsystem per product: each OS packages ITS OWN build with its
# OWN gendist, so the o32 and mips1 products ship in dist53/ and distmips1/
# (5.3 format, readable 5.3-6.5) and the n32 product in dist65/ (6.5 format).
# All three are the same product name, so installing one replaces another.
# idb sources point at bin/ under the gendist -sbase.
stage_inst_inputs() {
	_fl="$1"; _dv="$2"; _dst="$3"
	_abi=$(flavor_abi_desc "$_fl") || return 1
	sed -e "s/@VERSION@/$_dv/" -e "s/@SUBSYS@/$_fl/" -e "s|@ABI_DESC@|$_abi|" \
		"${REPO}/inst/irixscsitb.spec" > "$_dst/irixscsitb.spec"
	sed -e "s/@SUBSYS@/$_fl/" \
		"${REPO}/inst/irixscsitb.idb" > "$_dst/irixscsitb.idb"
}

# dist_version_from VERSION — numeric inst version (first 10 digits of the
# release version; date-stamped versions give sane inst upgrade ordering).
dist_version_from() {
	printf %s "$1" | tr -cd '0-9' | cut -c1-10
}
