#!/bin/sh
# Write buildhost.h — the identity of the machine doing the compiling, the
# "was this linked against 5.3 or 6.5 libraries?" half of the build
# identification (see CLAUDE.md). Regenerated every build, never committed.
#
# Used by meson.build (the Linux/CI build). The Makefile keeps its own inline
# recipe: that path runs on IRIX 5.3's make/sh and is proven there — this
# script exists so meson doesn't have to escape a shell one-liner.
#
# Usage: mkbuildhost.sh [output-path]   (default: ./buildhost.h)
set -eu
OUT="${1:-buildhost.h}"
{
	echo '/* Generated per build - identity of the machine that compiled this. */'
	echo "#define BUILD_MACHINE_OS  \"$(uname -s)\""
	echo "#define BUILD_MACHINE_REL \"$(uname -r)\""
} > "$OUT"
