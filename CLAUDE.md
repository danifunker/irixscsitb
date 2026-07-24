# CLAUDE.md

Guidance for working in this repository.

## What this is

`irixscsitb` is the **host-side** command-line companion for the Toolbox API shared by
[BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) and
[ZuluSCSI](https://github.com/ZuluSCSI/ZuluSCSI-firmware). It runs on the vintage
host (SGI **IRIX**) or on **Linux**, talks to the device over the SCSI bus using
vendor command codes `0xD0`–`0xDA`, and lets you list/fetch/send files in the
device's `/shared` directory, list and switch CD images, enumerate emulated
targets, toggle debug, and interrogate/scan the bus.

It is the IRIX/Linux sibling of **`../escsitoolbox`** (DOS/Windows, same
firmware). Keep the protocol constants in `irixscsitb.h` named to match that project's
`include/toolbox.h` (`TOOLBOX_LIST_FILES`, `TOOLBOX_GET_FILE`, …) so the two
stay easy to cross-read.

Originally a fork of [SonnyJim/bstoolbox](https://github.com/SonnyJim/bstoolbox)
(local dir `irixtoolbox`), genericised from BlueSCSI-only `bstoolbox` into the
firmware-neutral `irixscsitb` and destined for its own repo. Goals: solid on **IRIX
5.3 through 6.5**, and vendor-neutral. Binaries are packaged for IRIX (e.g. on an
**EFS** ISO built with Rusty-Backup / `rb-cli`, or written into a disk image).

**The IRIS emulator does NOT implement the toolbox.** Verified in
`../iris/src/scsi.rs`: its opcode table stops at `SGI_HD2CDROM = 0xc9`, there is
no `0xD0`–`0xDA` handler and no MODE SENSE page `0x31`; it only presents
`b"IRIS EMUL DISK  "` in INQUIRY. It is therefore correctly *rejected* — do not
re-add a product-name special case for it. If IRIS ever gains toolbox support it
will be detected automatically (see detection below); `-F` tests it meanwhile.

Upstream useful changes here are candidates to PR back to SonnyJim/bstoolbox.

## Layout

| File | Role |
|------|------|
| `irixscsitb.c` | OS-agnostic: CLI (`getopt`), toolbox protocol, all `toolbox_*` command builders, `main`/`do_drive`. |
| `irixscsitb.h` | Protocol constants (command opcodes, `TOOLBOX_API_VER`), `scsi_inquiry` and `ToolboxFileEntry` structs, modes/enums, globals. |
| `os.h` | The OS-backend contract (see below). |
| `irix.c` | IRIX backend: `<sys/dsreq.h>` `DS_ENTER` ioctls, `mediad` start/stop. |
| `linux.c` | Linux backend: `<scsi/sg.h>` `SG_IO` ioctls. |
| `Makefile` | `uname`-based OS detection; sets `-DOS_IRIX` / `-DOS_LINUX`. |
| `meson.build` | Linux/CI build only. |

**OS abstraction contract** (`os.h`) — `irixscsitb.c` only ever calls these; each
backend implements them:
`scsi_open`, `scsi_close`, `scsi_send_command` (read / data-in),
`scsi_send_commandw` (write / data-out), `scsi_send_command_probe` (data-in,
single attempt, silent — used by the `-b` scanner so unanswered nodes don't cost
10 retries + warnings), `scsi_enum_devices` (list the host's generic SCSI node
paths), `path_to_devnum` (device path → SCSI ID), `mediad_start` /
`mediad_stop`. When adding a feature, keep protocol logic in `irixscsitb.c` and
only touch `irix.c`/`linux.c` for transport.

## Build & run

```sh
make           # detects OS via uname, builds ./irixscsitb
make clean
meson setup build && meson compile -C build   # Linux/CI only
```

Run **as root**. Device path is positional:
- IRIX:  `irixscsitb -s /dev/scsi/sc0d1l0`
- Linux: `irixscsitb -s /dev/sg2`

Options: `-b` scan the bus (the **only** mode that needs no device path), `-i`
interrogate, `-t` list emulated targets (device map), `-l` list CDs, `-s` list
`/shared`, `-c N` switch CD, `-g N` get file N, `-p FILE` put file, `-o DIR`
output dir, `-d 0|1` set debug, `-D` show debug, `-v` verbose. Options must
precede the device path (IRIX `getopt` does not permute argv).

**Device nodes:** always the *generic SCSI character* devices — IRIX
`/dev/scsi/scNdNlN` (the `ds`/`dsreq` driver), Linux `/dev/sgN` (the `sg`
driver). Those are the only nodes that pass arbitrary vendor CDBs through.
IRIX's `/dev/dsk/*` (block) and `/dev/rdsk/*` (raw char) belong to the `dks`
*disk* driver and accept only disk I/O — they cannot carry `0xD0`–`0xDA`.
`scsi_enum_devices()` in each backend walks the right node namespace for `-b`.

## Packaging & release

`scripts/package.sh` wraps the verified **rb-cli** (github.com/danifunker/rusty-backup)
command sequence and produces **three** distributable artifacts from a built
binary:

- **`.iso`** — IRIX EFS CD-ROM: `rb-cli optical new sgi-efs IMG.iso` + `put
  IMG.iso@1 …`. SGI volume header + EFS in **slot 7 (typed SYSV)**, CD geometry —
  the shape real IRIX 5.3/6.5 distribution CDs use. In IRIS attach it as a
  `cdrom = true` device (or a `discs = […]` entry); on IRIX `mount -t efs -o ro
  /dev/dsk/dks0d<N>s7 /CDROM`.
- **`.hda`** — SGI EFS hard disk: `rb-cli new hd sgi-efs IMG.hda --heads 16
  --sectors 63` + `put IMG.hda@1 …`. dvh volume-header + EFS root at **slot 0**
  (geometry 16×63 matches the IRIS emulator). In IRIS attach as a `cdrom = false`
  device.
- **`.tar.gz`** — plain gzip tar of the binaries + README. The friendliest
  vector now that IRIS ships a built-in NFS server: drop the contents in a
  `[nfs] shared_dir` folder and untar from inside IRIX. Unlike the images (which
  land the binary 0644), the tarball carries the executable bit.

Both the CD and HDD address the EFS partition as **`@1`** for `put`/`get`/`ls`/`fsck`
(rb-cli maps `@1` to the sole EFS partition regardless of its slot number). The
old top-level `new --fs efs` / `new-sgi-hdd` / `new-sgi-cdrom` verbs were renamed
to the above; `package.sh` preflights for the new grammar and errors clearly if
rb-cli is too old.

```sh
scripts/package.sh --bin irixscsitb-o32 --tar-bin irixscsitb-n32 \
  --version 2026-06-17 --rb-cli ./rb-cli --extra README.md
```

The script `fsck`s and round-trip-verifies the `.iso` and `.hda`.
`.github/workflows/release.yml` runs the full pipeline with **two builders in
parallel** (see below), then packages and cuts a GitHub release. It needs both:
the secret **`IRIX53_DISK_URL`** (an installed 5.3 boot disk, `.chd` or a `.zip`
with one — feeds the native o32 job) and the variable **`IRIX_TOOLCHAIN_IMAGE`**
(a mips-sgi-irix gcc + IRIX sysroot container — feeds the n32 cross job).
Optional: `IRIS_REPO`/`IRIS_REF` (default `chronic8000/iris @ main`), `IRIX_CC`.

### Two builders: native o32, cross n32

- **o32 (5.3-capable)** is built **natively in IRIS** — the cross-compiled o32
  may not link for 5.3 (GNU binutils can't link 5.3's o32 shared libs; see
  [[irix-o32-cross-link-blocked]]). The `build-o32-native` release job builds
  iris from source (`--features chd,lightning`), fetches + DHCP-enables the 5.3
  disk, and runs `scripts/iris-build.sh --no-package --bin-out`.
- **n32 (6.x only)** is cross-compiled (`build-n32-cross`); GNU ld links 6.x's
  n32 libs fine.

`scripts/iris-build.sh` launches IRIS headless with `--ci --nfs-dir`, exports the
source over IRIS's **built-in NFS server**, mounts it in the guest (`mount
192.168.0.1:/ /mnt`), runs `make`, and reads the binary straight back off the
share — no scratch volume, no tar, no `iris-ci push/pull`. NFS needs the guest
networked: `scripts/irix-enable-dhcp.sh` flips the boot disk's DHCP flags on so
the IRIS NAT can hand it 192.168.0.2. `--no-package`/`--bin-out` let the CI job
emit just the o32 binary so packaging stays centralised in the `package` job.
(The old scratch-volume path is left commented in `ci/iris-irix53.toml` as a
no-networking fallback.)

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
  variables at the top of blocks, avoid C99-isms. **Never use `//` comments** —
  the o32/5.3 front-end (`cfe`) rejects every one as a syntax error, and a `//`
  in a `#define` swallows the rest of the macro body (a `//` on the
  `SCSI_CMD_LENGTH` define broke every call site). Always `/* */`. Note `%lld` /
  `(long long)` (in `bluescsi_sendfile`'s stat print) is shaky on the oldest
  libc — avoid relying on it.
- The generic SCSI dslib (`<sys/dsreq.h>`, `DS_ENTER`, `dsreq_t`, `DS_CONF`) is
  stable across 5.3–6.5, so the transport approach is sound.
- **No `usleep(3)` on 5.3** — it compiles but fails to *link* (`ld: Unresolved:
  usleep`). `irix.c` uses a `select()`-based `ms_sleep()` helper instead, which
  links across 5.3–6.5. Reach for `select()`/`sginap()`, not `usleep`/`nanosleep`.

## How toolbox detection works

See `docs/toolbox-protocol.md` for the full byte-level contract. Detection is
**two-stage** and lives in `toolbox_inquiry()` (`irixscsitb.c`). Both stages must pass.

**Stage 1 — claim** (the device says it supports the toolbox):

1. **INQUIRY firmware name** — sends `INQUIRY` (`0x12`), builds `"<vendor>
   <product> <rev>"` (with each space-padded field trimmed), and substring-matches
   it against `toolbox_firmware_ids[]` = `{"BlueSCSI", "ZuluSCSI"}`. Both
   firmwares stamp their name at **INQUIRY byte 36**, inside the range our
   `product_rev` field spans (bytes 32–63), so the match works without a separate
   field. This mirrors `escsitoolbox`'s `dos/scsiintf.cpp` check.
2. **MODE SENSE page 0x31** — if the name doesn't match, sends `MODE SENSE(6)`
   for vendor page `0x31` and scans it (NUL-tolerant) for the same names. The
   BlueSCSI page magic is `"BlueSCSI is the BEST STOLEN FROM BLUESCSI"`, emitted
   only when toolbox mode is enabled (`toolbox_modesense_page31()`).

**Stage 2 — confirm** (`toolbox_confirm()`): issue `TOOLBOX_LIST_DEVICES`
(`0xD9`) and require a plausible 8-byte device-type map back — every byte either
a valid type (`0x00`–`0x07`) or `0xFF`, and at least one target enabled. Only
then is the device accepted, and the map populates `device_list[]` to gate CD
ops. **A claim alone is never sufficient.**

`toolbox_firmware_ids[]` holds **firmware** names only — never emulated *product*
names. A product name says what a device pretends to be, not which commands it
implements. This is why the old `"IRIS EMUL DISK"` entry was removed (see above).

`-F` (`force_toolbox`) skips stage 1 and goes straight to stage 2, for firmware
we don't yet know by name. Because of this, a new toolbox-capable firmware needs
**no code change** to be usable, and none at all if it advertises either signal.

A soft Toolbox API-version check (`buf[buf[4]+4]`) only warns.

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

Tabs for indentation; functions are `static` and named `toolbox_<verb>`.
Errors print to `stderr` with `strerror(errno)`. The maintainers care about
clean compiles — avoid introducing MIPSpro warnings.
