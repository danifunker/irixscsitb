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

.PHONY: default detect irix-o32 irix-n32 tar clean

# Default target
default: detect

# OS detection and conditional build
detect:
	@OS=`uname -s`; \
	if [ "$$OS" = "Linux" ]; then \
		echo "*** Compiling for Linux"; \
		$(MAKE) irixscsitb \
			BUILD_OS=LINUX \
			SRCS="irixscsitb.c linux.c" \
			OBJS="irixscsitb.o linux.o" \
			CFLAGS="-O2 -DOS_LINUX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX64" ]; then \
		echo "*** Compiling for IRIX64 (n32/mips3)"; \
		$(MAKE) irixscsitb \
			BUILD_OS=IRIX \
			SRCS="irixscsitb.c irix.c" \
			OBJS="irixscsitb.o irix.o" \
			CFLAGS="-mips3 -n32 -O2 -DOS_IRIX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX" ]; then \
		echo "*** Compiling for IRIX (o32/mips2 - portable 5.3-6.5)"; \
		$(MAKE) irixscsitb \
			BUILD_OS=IRIX \
			SRCS="irixscsitb.c irix.c" \
			OBJS="irixscsitb.o irix.o" \
			CFLAGS="-32 -mips2 -O2 -DOS_IRIX" \
			LDFLAGS=""; \
	else \
		echo "Unsupported OS: $$OS"; exit 1; \
	fi

# Explicit IRIX targets (useful when cross-building for packaging on an EFS ISO).
# o32/mips2 produces ONE binary that runs across IRIX 5.3 through 6.5; n32/mips3
# is faster but 6.x-only. uname reports "IRIX" on 5.3 and "IRIX64" on 6.x.
irix-o32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c irix.c" OBJS="irixscsitb.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX" LDFLAGS=""

irix-n32:
	$(MAKE) irixscsitb \
		SRCS="irixscsitb.c irix.c" OBJS="irixscsitb.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX" LDFLAGS=""

# Build (auto-detecting the OS) then package the binary + README into
# $(BUILDDIR)/$(TARNAME) for easy distribution. Uses `tar cf - | gzip` rather
# than `tar czf` because IRIX 5.3 tar has no `z` flag; gzip must be on PATH.
# README.md is included only if present (it isn't in the CI scratch build dir).
tar: default
	@mkdir -p $(BUILDDIR); \
	files="irixscsitb"; \
	if [ -f README.md ]; then files="$$files README.md"; fi; \
	tar cf - $$files | gzip -c > $(BUILDDIR)/$(TARNAME); \
	echo "*** Packaged $(BUILDDIR)/$(TARNAME)"

# Build target
irixscsitb: $(OBJS)
	$(CC) $(CFLAGS) -o irixscsitb $(OBJS) $(LDFLAGS)

# Object file rules
irixscsitb.o: irixscsitb.c
	$(CC) $(CFLAGS) -c irixscsitb.c

irix.o: irix.c
	$(CC) $(CFLAGS) -c irix.c

linux.o: linux.c
	$(CC) $(CFLAGS) -c linux.c

# Clean rule
clean:
	@echo "*** Cleaning up..."
	@-rm -f *.o irixscsitb core
	@-rm -rf $(BUILDDIR)

