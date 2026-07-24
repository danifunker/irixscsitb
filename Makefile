CC = cc
CFLAGS =
LDFLAGS =
SRCS =
OBJS =

# Build artifacts (binary, objects) land in the repo root; the packaged tarball
# goes under $(BUILDDIR). Everything here is .gitignore'd so the tree stays clean
# even when the repo itself is shared out over NFS.
BUILDDIR = build
TARNAME  = irixscsitb.tar.gz

.PHONY: default detect irix-o32 irix-n32 irix-gui-o32 irix-gui-n32 tar test gui-syntax clean FORCE

# Two headers, because there are two different questions with two different
# lifecycles. Neither is committed.
#
#   version.h    WHAT source this is: git revision, dirty flag, and when it was
#                stamped. Produced on a modern host that has git, then SHIPPED
#                to the build machine as a static input. mkversion.sh refuses to
#                overwrite it when git is unavailable, so running make on IRIX
#                cannot downgrade a good revision to "unknown".
#
#   buildhost.h  WHERE it got compiled: uname of the build machine. This one is
#                a genuine build product, regenerated every time, and it is what
#                answers "was this linked against 5.3 or 6.5 libraries?" - a
#                question only the machine doing the linking can answer.
#
# Both are included solely by version.c, so regenerating them costs one
# recompile and leaves the rest of the tree alone.
version.h:
	@sh scripts/mkversion.sh version.h

buildhost.h: FORCE
	@echo '/* Generated per build - identity of the machine that compiled this. */' > buildhost.h
	@echo '#define BUILD_MACHINE_OS  "'`uname -s`'"' >> buildhost.h
	@echo '#define BUILD_MACHINE_REL "'`uname -r`'"' >> buildhost.h

FORCE:

# Default target
default: detect

# OS detection and conditional build
detect:
	@OS=`uname -s`; \
	if [ "$$OS" = "Linux" ]; then \
		echo "*** Compiling for Linux"; \
		$(MAKE) irixscsitb \
			BUILD_OS=LINUX \
			SRCS="irixscsitb.c toolbox.c version.c linux.c" \
			OBJS="irixscsitb.o toolbox.o version.o linux.o" \
			CFLAGS="-O2 -DOS_LINUX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX64" ]; then \
		echo "*** Compiling for IRIX64 (n32/mips3)"; \
		$(MAKE) irixscsitb \
			BUILD_OS=IRIX \
			SRCS="irixscsitb.c toolbox.c version.c irix.c" \
			OBJS="irixscsitb.o toolbox.o version.o irix.o" \
			CFLAGS="-mips3 -n32 -O2 -DOS_IRIX -DBUILD_N32" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX" ]; then \
		echo "*** Compiling for IRIX (o32/mips2 - portable 5.3-6.5)"; \
		$(MAKE) irixscsitb \
			BUILD_OS=IRIX \
			SRCS="irixscsitb.c toolbox.c version.c irix.c" \
			OBJS="irixscsitb.o toolbox.o version.o irix.o" \
			CFLAGS="-32 -mips2 -O2 -DOS_IRIX -DBUILD_O32" \
			LDFLAGS=""; \
	else \
		echo "Unsupported OS: $$OS"; exit 1; \
	fi

# Explicit IRIX targets (useful when cross-building for packaging on an EFS ISO).
# o32/mips2 produces ONE binary that runs across IRIX 5.3 through 6.5; n32/mips3
# is faster but 6.x-only. uname reports "IRIX" on 5.3 and "IRIX64" on 6.x.
irix-o32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c toolbox.c version.c irix.c" OBJS="irixscsitb.o toolbox.o version.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX -DBUILD_O32" LDFLAGS=""

irix-n32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c toolbox.c version.c irix.c" OBJS="irixscsitb.o toolbox.o version.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX -DBUILD_N32" LDFLAGS=""

# IRIS IM (Motif) GUI. A SECOND binary, not a replacement: the CLI stays free of
# any X dependency so it still runs on a headless machine or over a serial
# console. Both link the same toolbox.o.
#
# Built against the Motif 1.2 API, which is what IRIX 5.3 ships and 6.5 still
# carries - so the o32 build runs across the whole 5.3-6.5 range. Link order
# matters on IRIX: Sgm before Xm before Xt before X11.
#
# -lSgm pulls in SGI's Motif extension widgets. It is present on 5.3 and 6.x
# alike; drop it from GUILIBS if you ever target a system without them.
GUILIBS = -lSgm -lXm -lXt -lXext -lX11 -lm

irix-gui-o32:
	$(MAKE) scsitbgui \
		OBJS="gui_motif.o toolbox.o version.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX -DBUILD_O32" LDFLAGS="$(GUILIBS)"

irix-gui-n32:
	$(MAKE) scsitbgui \
		OBJS="gui_motif.o toolbox.o version.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX -DBUILD_N32" LDFLAGS="$(GUILIBS)"

# Build (auto-detecting the OS) then package the binary + README into
# $(BUILDDIR)/$(TARNAME) for easy distribution. Uses `tar cf - | gzip` rather
# than `tar czf` because IRIX 5.3 tar has no `z` flag; gzip must be on PATH.
# README.md is included only if present (it isn't in the CI scratch build dir).
tar: default
	@mkdir -p $(BUILDDIR); \
	files="irixscsitb"; \
	if [ -f scsitbgui ]; then files="$$files scsitbgui"; fi; \
	if [ -f README.md ]; then files="$$files README.md"; fi; \
	tar cf - $$files | gzip -c > $(BUILDDIR)/$(TARNAME); \
	echo "*** Packaged $(BUILDDIR)/$(TARNAME)"

# Host-side smoke test. Builds irixscsitb.c against tests/mock_os.c - a fake SCSI
# bus - so the protocol and device-detection logic can be exercised on a dev
# machine with no IRIX or Linux SCSI headers (e.g. macOS). Uses the host cc and
# the host's own C compiler defaults; nothing here ships in a release.
# Expected: BlueSCSI + ZuluSCSI marked [TOOLBOX], IRIS EMUL DISK NOT marked,
# and the page-0x31 liar reported as "claims toolbox, no 0xD9 answer".
test: version.h buildhost.h
	$(CC) -std=c89 -Wall -DOS_IRIX -I. -o tests/irixscsitb-mock irixscsitb.c toolbox.c version.c tests/mock_os.c
	@echo "*** mock bus scan:"
	@./tests/irixscsitb-mock -b

# Syntax-check the Motif GUI against a REAL IRIX header tree without needing an
# IRIX machine. gui_motif.c can't be compiled on the dev box - there is no
# <Xm/Xm.h> - but pointed at a copy of an IRIX /usr/include it parses fine under
# the host compiler, which catches the mistakes that matter here: a widget or
# resource that doesn't exist in Motif 1.2, a nested comment, a C99-ism.
#
#   make gui-syntax IRIX_INCLUDE=/path/to/irix/usr/include
#
# The -D flags stand in for what MIPSpro predefines (sgidefs.h errors out
# without them) and for the couple of types the host compiler supplies as
# builtins. Warnings from inside the IRIX headers themselves are expected -
# they are K&R-era declarations - so only gui_motif.c's own output matters.
IRIX_INCLUDE =
IRIX_FAKE_CC = -std=gnu89 -Wall -fsyntax-only -nostdinc \
	-D_LANGUAGE_C -D_MIPS_SZINT=32 -D_MIPS_SZLONG=32 -D_MIPS_SZPTR=32 \
	-D_LONGLONG -D_SGI_SOURCE -D__EXTENSIONS__ -D_WCHAR_T \
	-Dwchar_t=int "-DXIM=void *" -Dbitlen_t=long

gui-syntax:
	@if [ -z "$(IRIX_INCLUDE)" ]; then \
		echo "Set IRIX_INCLUDE to an IRIX /usr/include tree, e.g."; \
		echo "  make gui-syntax IRIX_INCLUDE=/tmp/irix/include"; exit 1; \
	fi
	$(CC) $(IRIX_FAKE_CC) -DOS_IRIX -I. -I$(IRIX_INCLUDE) gui_motif.c 2>&1 \
		| grep '^gui_motif\.c' | grep -v deprecated-non-prototype || true
	@echo "*** gui_motif.c: no errors against $(IRIX_INCLUDE)"

# Build targets
irixscsitb: $(OBJS)
	$(CC) $(CFLAGS) -o irixscsitb $(OBJS) $(LDFLAGS)

scsitbgui: $(OBJS)
	$(CC) $(CFLAGS) -o scsitbgui $(OBJS) $(LDFLAGS)

# Object file rules
irixscsitb.o: irixscsitb.c irixscsitb.h os.h
	$(CC) $(CFLAGS) -c irixscsitb.c

toolbox.o: toolbox.c irixscsitb.h os.h
	$(CC) $(CFLAGS) -c toolbox.c

version.o: version.c irixscsitb.h version.h buildhost.h
	$(CC) $(CFLAGS) -c version.c

gui_motif.o: gui_motif.c irixscsitb.h os.h
	$(CC) $(CFLAGS) -c gui_motif.c

irix.o: irix.c
	$(CC) $(CFLAGS) -c irix.c

linux.o: linux.c
	$(CC) $(CFLAGS) -c linux.c

# Clean rule
clean:
	@echo "*** Cleaning up..."
	@-rm -f *.o irixscsitb scsitbgui core tests/irixscsitb-mock buildhost.h
	@-rm -rf $(BUILDDIR)

