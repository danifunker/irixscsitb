# CLAUDE.md

Guidance for working in this repository.

## What this is

`bstoolbox` is the **host-side** command-line companion for the
[BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) toolbox API. It runs on
the vintage host (SGI **IRIX**) or on **Linux**, talks to a BlueSCSI over the
SCSI bus using vendor command codes `0xD0`–`0xDA`, and lets you list/fetch/send
files in the BlueSCSI `/shared` directory, list and switch CD images, toggle
debug, and interrogate the device.

This repo is a fork of [SonnyJim/bstoolbox](https://github.com/SonnyJim/bstoolbox)
(local dir `irixtoolbox`, remote `danifunker/irisstoolbox`). The goal of the
fork is to make the toolbox solid on **IRIX 5.3 through 6.5** and to support the
**IRIS emulator**, where BlueSCSI behaviour is emulated rather than real
hardware. Binaries are intended to be packaged for IRIX (e.g. on an **EFS** ISO
built with Rusty-Backup / `rb-cli`, or written into a disk image).

Upstream useful changes here are candidates to PR back to SonnyJim/bstoolbox.

## Layout

| File | Role |
|------|------|
| `bstoolbox.c` | OS-agnostic: CLI (`getopt`), BlueSCSI toolbox protocol, all `bluescsi_*` command builders, `main`/`do_drive`. |
| `bstoolbox.h` | Protocol constants (command opcodes, `BLUESCSI_TOOLBOX_API_VER`), `scsi_inquiry` and `ToolboxFileEntry` structs, modes/enums, globals. |
| `os.h` | The OS-backend contract (see below). |
| `irix.c` | IRIX backend: `<sys/dsreq.h>` `DS_ENTER` ioctls, `mediad` start/stop. |
| `linux.c` | Linux backend: `<scsi/sg.h>` `SG_IO` ioctls. |
| `Makefile` | `uname`-based OS detection; sets `-DOS_IRIX` / `-DOS_LINUX`. |
| `meson.build` | Linux/CI build only. |

**OS abstraction contract** (`os.h`) — `bstoolbox.c` only ever calls these; each
backend implements them:
`scsi_open`, `scsi_close`, `scsi_send_command` (read / data-in),
`scsi_send_commandw` (write / data-out), `path_to_devnum` (device path → SCSI
ID), `mediad_start` / `mediad_stop`. When adding a feature, keep protocol logic
in `bstoolbox.c` and only touch `irix.c`/`linux.c` for transport.

## Build & run

```sh
make           # detects OS via uname, builds ./bstoolbox
make clean
meson setup build && meson compile -C build   # Linux/CI only
```

Run **as root**. Device path is positional:
- IRIX:  `bstoolbox -s /dev/scsi/sc0d1l0`
- Linux: `bstoolbox -s /dev/sg2`

Options: `-i` interrogate, `-l` list CDs, `-s` list `/shared`, `-c N` switch CD,
`-g N` get file N, `-p FILE` put file, `-o DIR` output dir, `-d 0|1` debug,
`-v` verbose.

## Packaging & release

The binary is distributed inside SGI EFS images built by **rb-cli**
(github.com/danifunker/rusty-backup), so the IRIS emulator can mount them.
`scripts/package-efs.sh` wraps the verified rb-cli command sequence and produces
two artifacts from a built binary:

- **EFS CD image** — `rb-cli new --fs efs IMG` + `put IMG …` → a bare EFS
  superfloppy served as the emulated CD-ROM (`mount -t efs`).
- **SGI EFS HDD image** — `rb-cli new-sgi-hdd IMG --heads 16 --sectors 63` +
  `put IMG@1 …` → a dvh volume-header + EFS-root disk (geometry 16×63 matches the
  IRIS emulator). The EFS root is partition **`@1`**. Needs an rb-cli build with
  the `new-sgi-hdd` verb.

```sh
scripts/package-efs.sh --bin bstoolbox-o32 --version 2026-06-17 \
  --rb-cli ./rb-cli --extra README.md
```

The script `fsck`s and round-trip-verifies both images. `.github/workflows/release.yml`
runs the full pipeline (cross-compile → fetch rb-cli → package → GitHub release).
**It requires the repo variable `IRIX_TOOLCHAIN_IMAGE`** — a container with a
mips-sgi-irix gcc + IRIX sysroot — because CI cannot compile `irix.c` without the
proprietary SGI headers/libs. (`IRIX_CC` overrides the compiler name.)

## IRIX 5.3 → 6.5 portability (important)

The dev machine is macOS and has no IRIX headers, so `irix.c` cannot be compiled
or tested here — clang will flag `sys/dsreq.h`, `uint`, `dsconf_t`, etc. Those
are not real errors. The **IRIS emulator is the practical test rig** for the
IRIX path; otherwise it needs real hardware.

- **One binary for 5.3–6.5: build with the old 32-bit ABI.** `-n32` only exists
  on IRIX 6.x; it will not run on 5.3. The `o32` ABI (`cc -32 -mips2 -O2`) runs
  on every IRIX from 5.3 through 6.5. The Makefile now auto-selects: `uname`
  `IRIX64` (6.x) → `-mips3 -n32`; `uname` `IRIX` (5.3) → `-32 -mips2`. For
  packaging from a 6.x host, force the portable build with **`make irix-o32`**
  (or `make irix-n32` for the faster 6.x-only binary).
- **Compiler is MIPSpro `cc`**, not gcc. Keep the code C89-friendly: declare
  variables at the top of blocks, avoid C99-isms. `// comments` happen to work
  but prefer `/* */`. Note `%lld` / `(long long)` (in `bluescsi_sendfile`'s stat
  print) is shaky on the oldest libc — avoid relying on it.
- The generic SCSI dslib (`<sys/dsreq.h>`, `DS_ENTER`, `dsreq_t`, `DS_CONF`) is
  stable across 5.3–6.5, so the transport approach is sound.

## How BlueSCSI detection works

See `docs/bluescsi-protocol.md` for the full byte-level contract. `bluescsi_inquiry()`
in `bstoolbox.c` accepts a target if **either** signal matches:

1. **INQUIRY identity** — sends `INQUIRY` (`0x12`), builds `"<vendor> <product>
   <rev>"`, and matches it against `toolbox_accept_ids[]` (currently `"BlueSCSI"`
   and `"IRIS EMUL DISK"`). Real BlueSCSI appends `"BlueSCSI<ver>"` to
   `product_rev`; the **IRIS emulator** presents vendor `SGI` / product `IRIS
   EMUL DISK` / rev `1.0`. Add new accepted devices by appending to that array.
2. **MODE SENSE page 0x31** — if the identity doesn't match, sends `MODE
   SENSE(6)` for the BlueSCSI vendor page `0x31` and scans (NUL-tolerant) for an
   accepted id. The firmware page magic is `"BlueSCSI is the BEST STOLEN FROM
   BLUESCSI"`; it is emitted only when toolbox mode is enabled. This is the
   authoritative toolbox signal (`bluescsi_modesense_toolbox()`).

A soft Toolbox API-version check (`buf[buf[4]+4]`) only warns. The `0xD9`
device-type map (`0x00` HDD, `0x02` CD, `0xFF` absent) is fetched after
acceptance to gate CD ops and is **non-fatal** if it fails.

## Known issues / gotchas

- **Issue #3 (PUT to `/shared` "hangs" on IRIX):** the IRIX write path
  (`scsi_send_commandw`) used to do a redundant per-command TEST UNIT READY loop
  plus an unconditional `usleep(100ms)` after *every* 512-byte block, so
  multi-KB/MB transfers took minutes and looked like a hang. Fixed in `irix.c`
  (single readiness check, no per-block sleep). Still needs end-to-end
  verification on the emulator/hardware.
- **32-char filename limit** on `/shared` listings: structural to the protocol
  (`ToolboxFileEntry.name` is 32 bytes); not a bug.
- **Firmware `SEND_FILE_10` offset semantics:** the documented spec says
  `CDB[3..5]` is the *block number*; firmware `v2026.04.27` seeks with
  `seekCur(offset*512)` (relative), which corrupts multi-block writes. The host
  sends an incrementing block index per spec — the **IRIS emulator should use
  absolute `seek(blocknum*512)`**.
- **`%lld` / `long long`** is avoided (the `stat` size print uses `(long)` /
  `%ld`) for old `o32` libc compatibility.

## Conventions

Tabs for indentation; functions are `static` and named `bluescsi_<verb>`.
Errors print to `stderr` with `strerror(errno)`. The maintainers care about
clean compiles — avoid introducing MIPSpro warnings.
