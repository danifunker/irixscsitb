CC = cc
CFLAGS =
LDFLAGS =
SRCS =
OBJS =

.PHONY: default detect irix-o32 irix-n32 clean

# Default target
default: detect

# OS detection and conditional build
detect:
	@OS=`uname -s`; \
	if [ "$$OS" = "Linux" ]; then \
		echo "*** Compiling for Linux"; \
		$(MAKE) bstoolbox \
			BUILD_OS=LINUX \
			SRCS="bstoolbox.c linux.c" \
			OBJS="bstoolbox.o linux.o" \
			CFLAGS="-O2 -DOS_LINUX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX64" ]; then \
		echo "*** Compiling for IRIX64 (n32/mips3)"; \
		$(MAKE) bstoolbox \
			BUILD_OS=IRIX \
			SRCS="bstoolbox.c irix.c" \
			OBJS="bstoolbox.o irix.o" \
			CFLAGS="-mips3 -n32 -O2 -DOS_IRIX" \
			LDFLAGS=""; \
	elif [ "$$OS" = "IRIX" ]; then \
		echo "*** Compiling for IRIX (o32/mips2 - portable 5.3-6.5)"; \
		$(MAKE) bstoolbox \
			BUILD_OS=IRIX \
			SRCS="bstoolbox.c irix.c" \
			OBJS="bstoolbox.o irix.o" \
			CFLAGS="-32 -mips2 -O2 -DOS_IRIX" \
			LDFLAGS=""; \
	else \
		echo "Unsupported OS: $$OS"; exit 1; \
	fi

# Explicit IRIX targets (useful when cross-building for packaging on an EFS ISO).
# o32/mips2 produces ONE binary that runs across IRIX 5.3 through 6.5; n32/mips3
# is faster but 6.x-only. uname reports "IRIX" on 5.3 and "IRIX64" on 6.x.
irix-o32:
	$(MAKE) bstoolbox \
		SRCS="bstoolbox.c irix.c" OBJS="bstoolbox.o irix.o" \
		CFLAGS="-32 -mips2 -O2 -DOS_IRIX" LDFLAGS=""

irix-n32:
	$(MAKE) bstoolbox \
		SRCS="bstoolbox.c irix.c" OBJS="bstoolbox.o irix.o" \
		CFLAGS="-mips3 -n32 -O2 -DOS_IRIX" LDFLAGS=""

# Build target
bstoolbox: $(OBJS)
	$(CC) $(CFLAGS) -o bstoolbox $(OBJS) $(LDFLAGS)

# Object file rules
bstoolbox.o: bstoolbox.c
	$(CC) $(CFLAGS) -c bstoolbox.c

irix.o: irix.c
	$(CC) $(CFLAGS) -c irix.c

linux.o: linux.c
	$(CC) $(CFLAGS) -c linux.c

# Clean rule
clean:
	@echo "*** Cleaning up..."
	@-rm -f *.o bstoolbox core

