# IRIX cross-toolchain image

Builds `mips-sgi-irix6.5-gcc`, used by `.github/workflows/release.yml` to
cross-compile `irixscsitb` for IRIX. The image is set via the repo variable
`IRIX_TOOLCHAIN_IMAGE` (and `IRIX_CC` if your compiler is named differently).

## You must supply an IRIX sysroot

SGI's headers and libraries are proprietary and are **not** included here. Copy
them from your own licensed IRIX install (or the IRIS emulator's IRIX tree) into
`./sysroot/` before building. The sysroot is git-ignored — never commit it.

Expected layout (multilib: `lib`/`usr/lib` = o32, `lib32`/`usr/lib32` = n32):

```
docker/irix-toolchain/
├── Dockerfile
└── sysroot/
    ├── usr/include/...      # sys/dsreq.h, invent.h, stdio.h, string.h, ...
    ├── usr/lib/   lib/      # o32 libs  -> -mabi=32 -mips2  (IRIX 5.3–6.5)
    ├── usr/lib32/ lib32/    # n32 libs  -> -mabi=n32 -mips3 (IRIX 6.x)
    └── ...
```

A quick way to populate it from a running/emulated IRIX host:

```sh
mkdir -p sysroot/usr
rsync -a irix-host:/usr/include sysroot/usr/
rsync -a irix-host:/usr/lib  irix-host:/usr/lib32  sysroot/usr/
rsync -a irix-host:/lib       irix-host:/lib32      sysroot/
```

## Build, publish, wire up

```sh
docker build -t ghcr.io/<you>/irix-toolchain:6.5 docker/irix-toolchain
docker push  ghcr.io/<you>/irix-toolchain:6.5

gh variable set IRIX_TOOLCHAIN_IMAGE -b ghcr.io/<you>/irix-toolchain:6.5
# optional, if the compiler is named differently:
# gh variable set IRIX_CC -b mips-sgi-irix6.5-gcc
```

The build runs a smoke test that links a tiny program for both the o32 and n32
ABIs, so a successful `docker build` means the toolchain + sysroot can actually
produce IRIX binaries.

## Version notes

`BINUTILS_VER` / `GCC_VER` are `--build-arg`s. The defaults (binutils 2.35.2,
gcc 9.5.0) are the newest that still carry the `mips-sgi-irix` target. If the
build fails on the target, step down a major version. **IRIX 5.3** (o32 only)
may need an older gcc (3.x/4.x era) and a 5.3-era sysroot — build a second image
(e.g. `:5.3`) and point a separate workflow run at it if you need a binary built
specifically against 5.3 libraries (o32 binaries built against 6.5 libs usually
still run on 5.3, which is why the default image targets 6.5).
