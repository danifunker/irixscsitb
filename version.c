/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Build identification, shared by the CLI (-version) and the GUI (Help >
 * About). Deliberately the only file that includes the generated version.h, so
 * regenerating it on every build recompiles this one object and nothing else -
 * which is what keeps the __DATE__/__TIME__ stamp honest without dragging the
 * whole tree through a rebuild.
 *
 * Copyright (C) 2026 Dani Sarfati
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "irixscsitb.h"
#include "version.h"    /* shipped: what source this is  */
#include "buildhost.h"  /* generated: where it got compiled */

/*
 * version.h is a SHIPPED header, so it can legitimately be older than this
 * source - someone syncs a drop, we add a field, they build the old header
 * against the new code. Default anything missing rather than failing to
 * compile: a binary that reports "unknown" for one field is far better than a
 * build that dies on a remote machine with no git and no way to regenerate.
 */
#ifndef BUILD_REV
#define BUILD_REV "unknown"
#endif
#ifndef BUILD_STAMP
#define BUILD_STAMP "unknown"
#endif
#ifndef BUILD_MACHINE_OS
#define BUILD_MACHINE_OS "unknown"
#endif
#ifndef BUILD_MACHINE_REL
#define BUILD_MACHINE_REL ""
#endif

/*
 * Which ABI this is comes from the Makefile target that built it, because the
 * Makefile is the thing that chose it. MIPSpro's own ABI macros vary across
 * releases and are not worth chasing when the build system already knows.
 */
#if defined(BUILD_N32)
#define ABI_NAME "n32 (mips3)"
#define ABI_NOTE "IRIX 6.x only"
#elif defined(BUILD_O32)
#define ABI_NAME "o32 (mips2)"
#define ABI_NOTE "runs on IRIX 5.3 through 6.5"
#else
#define ABI_NAME "host"
#define ABI_NOTE "development build, not for distribution"
#endif

/* Short git revision of the sources, "-dirty" if the tree had local changes. */
const char *build_revision(void)
{
	return BUILD_REV;
}

/*
 * When the source snapshot was stamped, on the host that had git. This is the
 * one that ties a binary to a commit; it is identical across every artifact
 * produced from one sync.
 */
const char *build_stamp(void)
{
	return BUILD_STAMP;
}

/*
 * When this object was actually compiled, from the C preprocessor's own
 * macros - free, and it needs no build-system plumbing at all. Differs from
 * build_stamp() by however long the sources sat before someone built them.
 */
const char *build_compiled(void)
{
	return __DATE__ " " __TIME__;
}

/* ABI, and what it implies about where the binary will run. */
const char *build_abi(void)
{
	return ABI_NAME " - " ABI_NOTE;
}

/*
 * The system whose libraries this was linked against. On IRIX that is exactly
 * the "5.3 libraries or 6.5 libraries" question: a binary built on 5.3 links
 * 5.3's libc/libXm and runs everywhere from 5.3 up, while one built on 6.5 may
 * depend on symbols 5.3 does not have.
 */
const char *build_libs(void)
{
	return BUILD_MACHINE_OS " " BUILD_MACHINE_REL;
}
