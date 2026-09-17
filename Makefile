CC = cc
CFLAGS =
LDFLAGS =
SRCS =
OBJS =

# Objects and binaries land in the repo root; everything finished - binaries,
# tarball, logs - is collected under $(BUILDDIR) (output/). All .gitignore'd so
# the tree stays clean even when the repo itself is shared out over NFS.
BUILDDIR = output
TARNAME  = irixscsitb.tar

# MUST be the first target in this file: make takes the first rule it sees as
# the default goal, so anything above this (the version.h/buildhost.h rules,
# say) would silently become what plain `make` builds.
default: detect

.PHONY: default detect irix-o32 irix-n32 irix-gui-o32 irix-gui-n32 tar test irix-syntax gui-syntax clean FORCE

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

# OS detection and conditional build
# Builds BOTH binaries on IRIX - the CLI and the Motif GUI - unless NOGUI is
# set in the environment:
#
#   make                 CLI + GUI
#   NOGUI=1 make         CLI only
#
# The GUI step is deliberately non-fatal. A machine without Motif development
# installed should still end up with a working irixscsitb rather than a failed
# build; the warning says what happened. Linux gets the CLI only - gui_motif.c
# is IRIX-only by design.
detect:
	@OS=`uname -s`; \
	if [ "$$OS" = "Linux" ]; then \
		echo "*** Compiling for Linux"; \
		$(MAKE) irixscsitb \
			BUILD_OS=LINUX \
			SRCS="irixscsitb.c toolbox.c wifi.c version.c linux.c" \
			OBJS="irixscsitb.o toolbox.o wifi.o version.o linux.o" \
			CFLAGS="-O2 -DOS_LINUX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX64" ]; then \
		echo "*** Compiling for IRIX64 (n32/mips3)"; \
		$(MAKE) irix-n32 || exit 1; \
		if [ -z "$$NOGUI" ]; then \
			echo "*** Compiling the Motif GUI (n32/mips3)"; \
			$(MAKE) irix-gui-n32 || \
				echo "*** WARNING: GUI did not build; CLI is fine. Check libXm/libSgm."; \
		fi; \
	elif [ "$$OS" = "IRIX" ]; then \
		echo "*** Compiling for IRIX (o32/mips2 - portable 5.3-6.5)"; \
		$(MAKE) irix-o32 || exit 1; \
		if [ -z "$$NOGUI" ]; then \
			echo "*** Compiling the Motif GUI (o32/mips2)"; \
			$(MAKE) irix-gui-o32 || \
				echo "*** WARNING: GUI did not build; CLI is fine. Check libXm/libSgm."; \
		fi; \
	else \
		echo "Unsupported OS: $$OS"; exit 1; \
	fi

# Explicit IRIX targets (useful when cross-building for packaging on an EFS ISO).
# o32/mips2 produces ONE binary that runs across IRIX 5.3 through 6.5; n32/mips3
# is faster but 6.x-only. uname reports "IRIX" on 5.3 and "IRIX64" on 6.x.
irix-o32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c toolbox.c wifi.c version.c irix.c" OBJS="irixscsitb.o toolbox.o wifi.o version.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX -DBUILD_O32" LDFLAGS=""

irix-n32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c toolbox.c wifi.c version.c irix.c" OBJS="irixscsitb.o toolbox.o wifi.o version.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX -DBUILD_N32" LDFLAGS=""

# IRIS IM (Motif) GUI. A SECOND binary, not a replacement: the CLI stays free of
# any X dependency so it still runs on a headless machine or over a serial
# console. Both link the same toolbox.o.
#
# Built against the Motif 1.2 API, which is what IRIX 5.3 ships and 6.5 still
# carries - so the o32 build runs across the whole 5.3-6.5 range. Link order
# matters on IRIX: Sgm before Xm before Xt before X11.
#
# Plain Motif only. -lSgm (SGI's Motif extension widgets) was here, but
# gui_motif.c includes no <Sgm/...> header and calls no Sg* function, so it was
# linking a library it never used - and making the GUI refuse to build on any
# IRIX that has Motif without SGI's extensions. Add it back only alongside an
# actual SgFinder/SgGrid/etc. use.
GUILIBS = -lXm -lXt -lXext -lX11 -lm

irix-gui-o32:
	$(MAKE) scsitbgui \
		OBJS="gui_motif.o toolbox.o wifi.o version.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX -DBUILD_O32" LDFLAGS="$(GUILIBS)"

irix-gui-n32:
	$(MAKE) scsitbgui \
		OBJS="gui_motif.o toolbox.o wifi.o version.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX -DBUILD_N32" LDFLAGS="$(GUILIBS)"

# Build (auto-detecting the OS) then package the binaries + README into
# $(BUILDDIR)/$(TARNAME). Plain tar, not tar.gz: IRIX 5.3's tar can neither
# create nor extract compressed archives, so a .gz would need gzip at both ends
# and "gunzip -c f | tar xvf -" to unpack. The payload is small enough that
# compressing it is not worth the dependency.
# README.md is included only if present (it isn't in the CI scratch build dir).
tar: default
	@mkdir -p $(BUILDDIR); \
	files="irixscsitb"; \
	if [ -f scsitbgui ]; then files="$$files scsitbgui"; fi; \
	if [ -f README.md ]; then files="$$files README.md"; fi; \
	tar cf $(BUILDDIR)/$(TARNAME) $$files; \
	echo "*** Packaged $(BUILDDIR)/$(TARNAME)"

# Host-side smoke test. Builds irixscsitb.c against tests/mock_os.c - a fake SCSI
# bus - so the protocol and device-detection logic can be exercised on a dev
# machine with no IRIX or Linux SCSI headers (e.g. macOS). Uses the host cc and
# the host's own C compiler defaults; nothing here ships in a release.
# Expected: BlueSCSI + ZuluSCSI marked [TOOLBOX], IRIS EMUL DISK NOT marked,
# the page-0x31 liar and the toolbox-disabled ZuluSCSI both reported as
# "claims toolbox, no 0xD9 answer", and the latter making both -b and -i print
# the zuluscsi.ini EnableToolbox advice (a stock ZuluSCSI ships with it off).
# Then the listing/transfer contract: a 4,433,547,264-byte CD image must print
# in full (the 40-bit size never passes through a 32-bit long), CD images carry
# no "/" (entry byte 1 is 1 for a FILE), a real /shared directory does, a fetch
# writes exactly the advertised bytes without reading past the end, a 2 GiB
# file is refused before any transfer (TOOLBOX_TEST_32BIT_OFFSETS makes the
# core behave like an o32/n32 build for that one check), and a CD switch
# refused because the volume is busy exits non-zero (-f goes through, a non-CD
# target is refused too).
# -D_POSIX_C_SOURCE: strict -std=c89 makes glibc hide POSIX declarations
# (getopt/optarg/optind), which only shows up on Linux CI - macOS exposes them
# regardless. Affects this host-side build only; MIPSpro cc ignores the issue.
test: version.h buildhost.h
	$(CC) -std=c89 -Wall -D_POSIX_C_SOURCE=200112L -DOS_IRIX -DTOOLBOX_TEST_32BIT_OFFSETS -I. -o tests/irixscsitb-mock irixscsitb.c toolbox.c wifi.c version.c tests/mock_os.c
	@echo "*** mock bus scan:"
	@./tests/irixscsitb-mock -b
	@echo ""
	@echo "*** mock Wi-Fi status (-W), device found automatically:"
	@./tests/irixscsitb-mock -W
	@echo ""
	@echo "*** mock Wi-Fi scan (-w):"
	@./tests/irixscsitb-mock -w
	@echo ""
	@echo "*** mock -F Wi-Fi status against a DISK target (must refuse - no 0x1C emitted):"
	@./tests/irixscsitb-mock -F -W /dev/mock/sc0d0l0; test $$? -ne 0
	@echo ""
	@echo "*** mock -F Wi-Fi status against the radio itself (floor must not block it):"
	@./tests/irixscsitb-mock -F -W /dev/mock/sc0d7l0
	@echo ""
	@echo "*** mock -b must tell a toolbox-disabled ZuluSCSI how to enable it:"
	@./tests/irixscsitb-mock -b | grep "EnableToolbox = 1"
	@echo ""
	@echo "*** mock -i against a ZuluSCSI with EnableToolbox = 0 (must refuse, and say why):"
	@if ./tests/irixscsitb-mock -i /dev/mock/sc0d9l0 > tests/mock-d9.out 2>&1; then \
		cat tests/mock-d9.out; rm -f tests/mock-d9.out; \
		echo "FAIL: a toolbox-disabled ZuluSCSI was accepted"; exit 1; fi
	@cat tests/mock-d9.out
	@grep -q "EnableToolbox = 1" tests/mock-d9.out || { rm -f tests/mock-d9.out; \
		echo "FAIL: no EnableToolbox advice for a silent ZuluSCSI"; exit 1; }
	@rm -f tests/mock-d9.out
	@echo ""
	@echo "*** mock -l: a 40-bit size prints in full, and CD images carry no '/':"
	@./tests/irixscsitb-mock -l /dev/mock/sc0d5l0 > tests/mock-l.out; cat tests/mock-l.out
	@grep -q "^#1 irix-6-5-22-full.iso 4433547264 bytes" tests/mock-l.out || \
		{ rm -f tests/mock-l.out; echo "FAIL: 40-bit size not printed in full"; exit 1; }
	@if grep -q "iso/" tests/mock-l.out; then rm -f tests/mock-l.out; \
		echo "FAIL: a CD image was marked as a directory"; exit 1; fi
	@rm -f tests/mock-l.out
	@echo ""
	@echo "*** mock -s: a real directory is still marked:"
	@./tests/irixscsitb-mock -s /dev/mock/sc0d3l0 | grep "^#2 games/ 0 bytes"
	@echo ""
	@echo "*** mock -g: one full block plus a 904-byte tail, then an exact multiple - no read past the end:"
	@rm -rf tests/mock-out && mkdir -p tests/mock-out
	@./tests/irixscsitb-mock -g 1 -o tests/mock-out/ /dev/mock/sc0d3l0
	@test "$$(wc -c < tests/mock-out/notes.txt | tr -d ' ')" -eq 5000 || { echo "FAIL: fetched size"; exit 1; }
	@test "$$(od -An -tx1 -j 4095 -N 2 tests/mock-out/notes.txt | tr -d ' ')" = "ff00" || { echo "FAIL: block boundary"; exit 1; }
	@test "$$(od -An -tx1 -j 4999 -N 1 tests/mock-out/notes.txt | tr -d ' ')" = "87" || { echo "FAIL: last byte"; exit 1; }
	@./tests/irixscsitb-mock -g 0 -o tests/mock-out/ /dev/mock/sc0d3l0
	@test "$$(wc -c < tests/mock-out/Doom.iso | tr -d ' ')" -eq 4096 || { echo "FAIL: exact-multiple size"; exit 1; }
	@echo "*** mock -g of a 2 GiB file: a 32-bit build must refuse up front, writing nothing:"
	@if ./tests/irixscsitb-mock -g 3 -o tests/mock-out/ /dev/mock/sc0d3l0 > tests/mock-g.out 2>&1; then \
		cat tests/mock-g.out; echo "FAIL: a file too large for a 32-bit build exited 0"; exit 1; fi
	@cat tests/mock-g.out
	@grep -q "cannot write a file" tests/mock-g.out || { echo "FAIL: no size-limit message"; exit 1; }
	@test ! -e tests/mock-out/big.img || { echo "FAIL: an output file was created"; exit 1; }
	@rm -f tests/mock-g.out
	@rm -rf tests/mock-out
	@echo ""
	@echo "*** mock -c: a switch refused because the volume is busy must exit non-zero:"
	@if ./tests/irixscsitb-mock -c 1 /dev/mock/sc0d5l0; then echo "FAIL: refusal exited 0"; exit 1; fi
	@echo "*** mock -f -c: the forced switch must go through:"
	@./tests/irixscsitb-mock -f -c 1 /dev/mock/sc0d5l0
	@echo "*** mock -c against a non-CD target must exit non-zero:"
	@if ./tests/irixscsitb-mock -c 1 /dev/mock/sc0d3l0; then echo "FAIL: not-a-CD exited 0"; exit 1; fi

# Syntax-check the IRIX-only sources against a REAL IRIX header tree, without
# needing an IRIX machine. gui_motif.c and irix.c cannot be COMPILED on the dev
# box - there is no <Xm/Xm.h> or <sys/dsreq.h> - but pointed at a copy of an
# IRIX /usr/include they parse fine under the host compiler, which catches the
# mistakes that actually matter here: a widget or resource that does not exist
# in Motif 1.2, a struct field with the wrong type, a nested comment, a C99-ism.
#
#   make irix-syntax IRIX_INCLUDE=/path/to/irix/usr/include
#
# The -D flags stand in for what MIPSpro predefines (sgidefs.h hard-errors
# without _MIPS_SZINT et al) and for the couple of types the host compiler
# supplies as builtins. Warnings from inside the IRIX headers themselves are
# expected - they are K&R-era declarations - so only our own files' output
# matters. gui-syntax is kept as an alias for the GUI-only check.
IRIX_INCLUDE =
IRIX_ONLY_SRCS = gui_motif.c irix.c
IRIX_FAKE_CC = -std=gnu89 -Wall -fsyntax-only -nostdinc \
	-D_LANGUAGE_C -D_MIPS_SZINT=32 -D_MIPS_SZLONG=32 -D_MIPS_SZPTR=32 \
	-D_LONGLONG -D_SGI_SOURCE -D__EXTENSIONS__ -D_WCHAR_T \
	-Dwchar_t=int "-DXIM=void *" -Dbitlen_t=long

irix-syntax gui-syntax:
	@if [ -z "$(IRIX_INCLUDE)" ]; then \
		echo "Set IRIX_INCLUDE to an IRIX /usr/include tree, e.g."; \
		echo "  make irix-syntax IRIX_INCLUDE=/tmp/irix/include"; exit 1; \
	fi
	@for f in $(IRIX_ONLY_SRCS); do \
		echo "--- $$f"; \
		$(CC) $(IRIX_FAKE_CC) -DOS_IRIX -I. -I$(IRIX_INCLUDE) $$f 2>&1 \
			| grep "^$$f" | grep -v deprecated-non-prototype || true; \
	done
	@echo "*** no errors against $(IRIX_INCLUDE)"

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

wifi.o: wifi.c irixscsitb.h os.h
	$(CC) $(CFLAGS) -c wifi.c

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

