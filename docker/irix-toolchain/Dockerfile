# syntax=docker/dockerfile:1
#
# IRIX o32 cross-toolchain (gcc 4.7.4) for bstoolbox, targeting IRIX 5.3 - 6.5.
#
# gcc removed the IRIX target after 4.7, and 4.7 won't build on a modern host, so
# this reuses the PRE-PATCHED gcc-4.7-irix source from mroach/irix-builder (GPL)
# on a debian:buster base (its gcc-8 host builds 4.7 cleanly). Unlike mroach's
# recipe - which is n32/IRIX-6.5 (`--disable-multilib`, irix6.5 default = n32) -
# this configures o32 (`--with-abi=32`) against YOUR IRIX 5.3 sysroot, so the
# resulting binary runs across 5.3 - 6.5.
#
# Supply the sysroot as docker/irix-toolchain/sysroot.tgz
# (scripts/make-irix-sysroot.sh --abi o32). Proprietary - never committed/pushed.

ARG TARGET=mips-sgi-irix6.5
ARG PREFIX=/opt/irix
ARG SYSROOT=/opt/irix/sysroot
ARG BINUTILS_VER=2.17a
ARG BINUTILS_BASE=2.17
ARG GCC_URL=http://dl.mroach.com/irix/buildtools/gcc-4.7-irix.tar.xz

FROM debian:buster-slim AS build
ARG TARGET
ARG PREFIX
ARG SYSROOT
ARG BINUTILS_VER
ARG BINUTILS_BASE
ARG GCC_URL

# buster is EOL -> use the Debian archive mirror.
RUN { echo 'deb http://archive.debian.org/debian buster main'; \
      echo 'deb http://archive.debian.org/debian-security buster/updates main'; } > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends \
      build-essential bison flex texinfo gettext curl ca-certificates xz-utils bzip2 file \
      autotools-dev libgmp-dev libmpfr-dev libmpc-dev \
 && rm -rf /var/lib/apt/lists/*

# Your IRIX 5.3 o32 sysroot, extracted INSIDE the (case-sensitive) container.
COPY sysroot.tgz /tmp/sysroot.tgz
RUN mkdir -p ${SYSROOT} && tar -xzf /tmp/sysroot.tgz -C ${SYSROOT} && rm /tmp/sysroot.tgz

WORKDIR /src
ENV PATH="${PREFIX}/bin:${PATH}"

# binutils, era-matched to gcc 4.7, targeting IRIX.
RUN curl -fsSL "http://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.bz2" | tar xj \
 && find binutils-${BINUTILS_BASE} -name config.guess -exec cp /usr/share/misc/config.guess {} \; \
 && find binutils-${BINUTILS_BASE} -name config.sub   -exec cp /usr/share/misc/config.sub   {} \; \
 && mkdir b-binutils && cd b-binutils \
 && ../binutils-${BINUTILS_BASE}/configure \
      --target=${TARGET} --prefix=${PREFIX} \
      --with-sysroot=${SYSROOT} --disable-nls --disable-werror \
 && make -j"$(nproc)" && make install \
 && cd /src && rm -rf b-binutils binutils-${BINUTILS_BASE}

# Pre-patched gcc 4.7.4 (GPL), configured o32 against the 5.3 sysroot.
# MAKEINFO=true skips the doc build (avoids texinfo-version friction).
RUN curl -fsSL "${GCC_URL}" | tar xJ && mv gcc-4.7-irix gcc \
 && find gcc -name config.guess -exec cp /usr/share/misc/config.guess {} \; \
 && find gcc -name config.sub   -exec cp /usr/share/misc/config.sub   {} \; \
 && for f in $(grep -rl --include='*.h' mips_fallback_frame_state gcc); do \
        echo "/* md-unwind signal-frame fallback disabled: needs IRIX 6.5 ucontext (CTX_SP); irrelevant for a C program on 5.3 */" > "$f"; \
    done \
 && for f in $(find gcc -path '*config/mips/irix-crt*.S'); do \
        sed -i -e 's/\bdaddiu\b/addiu/g' -e 's/\bsd\b/sw/g' -e 's/\bld\b/lw/g' "$f"; \
    done \
 && mkdir b-gcc && cd b-gcc \
 && AS_FOR_TARGET=${TARGET}-as LD_FOR_TARGET=${TARGET}-ld \
    NM_FOR_TARGET=${TARGET}-nm RANLIB_FOR_TARGET=${TARGET}-ranlib \
    ../gcc/configure \
      --target=${TARGET} --prefix=${PREFIX} \
      --with-sysroot=${SYSROOT} --with-build-sysroot=${SYSROOT} \
      --enable-obsolete --disable-multilib --with-abi=32 --with-arch=mips2 \
      --enable-languages=c --disable-nls --disable-shared \
      --disable-threads --disable-libssp \
 && make -j"$(nproc)" all-gcc all-target-libgcc MAKEINFO=true \
 && make install-gcc install-target-libgcc MAKEINFO=true \
 && cd /src && rm -rf b-gcc gcc

# Slim runtime image with the toolchain + gcc's host-lib deps.
FROM debian:buster-slim
ARG TARGET
ARG PREFIX
RUN { echo 'deb http://archive.debian.org/debian buster main'; \
      echo 'deb http://archive.debian.org/debian-security buster/updates main'; } > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends make file libgmp10 libmpfr6 libmpc3 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build ${PREFIX} ${PREFIX}
ENV PATH="${PREFIX}/bin:${PATH}"

# Smoke test: fully link a tiny o32 binary against the IRIX system libs.
RUN printf 'int main(void){return 0;}\n' > /tmp/t.c \
 && "${TARGET}-gcc" -mabi=32 -mips2 -o /tmp/t-o32 /tmp/t.c \
 && file /tmp/t-o32 \
 && rm -f /tmp/t.c /tmp/t-o32
