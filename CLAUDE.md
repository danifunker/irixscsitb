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
| `toolbox.c` | **The core.** OS- and UI-agnostic: all `toolbox_*` command builders, two-stage detection, single-node probe. Returns data, never prints a result. |
| `irixscsitb.c` | CLI front end: `getopt`, `main`/`do_drive`, and the `cli_*` functions that turn core results into stdout text. Speaks no SCSI itself. |
| `gui_motif.c` | IRIS IM (Motif 1.2) GUI front end. IRIX-only, links the same `toolbox.o`. |
| `version.c` | Build identification. The only file that includes the generated headers. |
| `irixscsitb.h` | Protocol constants (command opcodes, `TOOLBOX_API_VER`), `scsi_inquiry` / `ToolboxFileEntry` / `ToolboxDetect` / `ToolboxScanEntry` structs, modes/enums, `extern` globals, and the core API prototypes. |
| `os.h` | The OS-backend contract (see below). |
| `irix.c` | IRIX backend: `<sys/dsreq.h>` `DS_ENTER` ioctls, `mediad` start/stop. |
| `linux.c` | Linux backend: `<scsi/sg.h>` `SG_IO` ioctls. |
| `Makefile` | `uname`-based OS detection; sets `-DOS_IRIX` / `-DOS_LINUX`. |
| `meson.build` | Linux/CI build only. |
| `scripts/mkversion.sh` | Stamps `version.h` from git. Runs on the *host*, never on IRIX. |
| `scripts/sync-irix-drop.sh` | Assembles the IRIX/IRIS drop folder. The only supported way. |
| `scripts/irix-native-build.sh` | Shipped into the drop as `build.sh`; builds natively inside IRIX. |
| `docs/HOWTO-IRIS.txt` | Shipped into the drop; instructions for the IRIX side. |

**Three-layer split:** transport (`irix.c`/`linux.c`) → protocol (`toolbox.c`) →
presentation (`irixscsitb.c`, `gui_motif.c`). The core must stay printless: it
may write errors and `verbose` diagnostics to stderr, but a *result* always goes
back as a return value or a filled struct, because the GUI needs it as widget
state rather than as text. Adding an operation means a `toolbox_*` function in
`toolbox.c` plus a `cli_*` printer in `irixscsitb.c`.

**OS abstraction contract** (`os.h`) — `toolbox.c` only ever calls these; each
backend implements them:
`scsi_open`, `scsi_close`, `scsi_send_command` (read / data-in),
`scsi_send_commandw` (write / data-out), `scsi_send_command_probe` (data-in,
single attempt, silent — used by the `-b` scanner so unanswered nodes don't cost
10 retries + warnings), `scsi_enum_devices` (list the host's generic SCSI node
paths), `path_to_devnum` (device path → SCSI ID), `mediad_start` /
`mediad_stop`. When adding a feature, keep protocol logic in `toolbox.c` and
only touch `irix.c`/`linux.c` for transport.

## Build & run

```sh
make           # detects OS via uname, builds ./irixscsitb
make tar       # build + package binary/README into build/irixscsitb.tar.gz
make test      # host-side smoke test against a mock SCSI bus (see below)
make clean
meson setup build && meson compile -C build   # Linux/CI only

# IRIX only - the Motif GUI, a second binary alongside the CLI
make irix-gui-o32   # portable 5.3-6.5
make irix-gui-n32   # faster, 6.x only

# dev box - parse gui_motif.c against a real IRIX header tree (see GUI below)
make gui-syntax IRIX_INCLUDE=/path/to/irix/usr/include
```

**`make test` is how you verify changes without hardware.** The dev machine is
macOS with no IRIX/Linux SCSI headers, so `irix.c`/`linux.c` cannot be compiled
here — but `toolbox.c` + `irixscsitb.c` (all the protocol, detection and CLI
logic) *can*, by linking them against `tests/mock_os.c`, a fake 7-device SCSI bus
implementing the `os.h` contract. It covers a plain disk, an IRIS EMUL DISK, a CD-ROM, a real
BlueSCSI, a real ZuluSCSI, a dead node, and a "liar" that serves page 0x31 but
never implements `0xD9`. Expected: only BlueSCSI and ZuluSCSI are `[TOOLBOX]`;
IRIS is not; the liar reads `claims toolbox, no 0xD9 answer`. **Run it after any
change to detection or the command builders** — it catches regressions that a
syntax check cannot. Add a device to `mock_paths[]`/`mock_command()` to cover
new firmware.

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
- **No `snprintf(3)` on 5.3 either** — verified absent from both `<stdio.h>` and
  `libc.so.1` on a real 5.3 install, so it fails to link the same way `usleep`
  does. There is therefore *no* size-limited formatter available. The way to keep
  a message buffer safe is to clamp what goes **into** it: `gui_motif.c` has a
  `copy_clamped()` helper that every variable-length value (a path out of a file
  dialog, say) passes through before it reaches `sprintf`. Protocol-bounded data
  — device paths (`SCSI_PATH_MAX`), identities (`TOOLBOX_IDENTITY_MAX`), file
  names (`NAME_BUF_SIZE`) — is safe to format directly, provided the destination
  is sized against those constants rather than a guessed number.

## How toolbox detection works

See `docs/toolbox-protocol.md` for the full byte-level contract. Detection is
**two-stage** and lives in `toolbox_detect()` (`toolbox.c`). Both stages must pass.
It is available in halves — `toolbox_identify()` (INQUIRY, identity string, API
version) and `toolbox_qualify()` (claim + confirm, the only part that sends
vendor opcodes) — so a front end can show what a device says it is before paying
for the slower qualification. `toolbox_detect()` just runs both.

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

## The Motif GUI (`gui_motif.c`)

A **second binary** (`scsitbgui`), not a replacement. The CLI keeps zero X
dependencies so it still works headless and over a serial console; both link the
same `toolbox.o`. There is no protocol code in `gui_motif.c` at all — only
widgets and the open/act/close sequence around each core call, so the two front
ends cannot drift.

**Layout:** menu bar (File / Device / Help) over a paned window — upper pane is
the bus scan, lower pane is either the `/shared` listing or the CD images
(radio-selected) — with a status line in the MainWindow message area. Full
coverage of the CLI: `-b` (Rescan), `-i` (Interrogate), `-t` (Emulated Targets),
`-s`/`-l` (the content pane), `-g` (Get File, prompts for an output dir), `-p`
(Put File, file-selection dialog), `-c` (Switch To CD), `-D`/`-d` (the Debug
menu items), `-F` (a Force detection toggle that rescans).

**Operation buttons are gated on `confirmed`**, not on `claims` — a device that
advertised the toolbox but failed 0xD9 leaves them greyed, and the dialog
explains which of the two stages it failed. Same rule as the CLI, surfaced.

**Column widths are computed, not hardcoded.** `scan_bus()` measures the longest
path and type string it actually got, builds the `sprintf` format from those, and
then `fit_list_width()` measures the widest rendered row in *pixels* (via the
list's `XmFontList` → `XFontStruct` → `XTextWidth`) and sets `XmNwidth` from it.
This happens before `XtRealizeWidget`, so the window opens correctly sized rather
than jumping. Two reasons it has to be dynamic: a `/dev/scsi/sc0d1l0` path and a
`/dev/sg0` path differ by nine characters, and the firmware name — the part you
care about — sits at the *end* of the identity string, so clipping loses exactly
the wrong thing. Lists are `XmCONSTANT`, so anything still too wide scrolls
sideways instead of shoving the window off screen. The lists are a fixed font for
the same reason; a proportional one would make the measurement meaningless.

**Toolkit: IRIS IM — SGI's OSF/Motif.** Write to the **Motif 1.2 API only**.
This was verified against a real IRIX 5.3 install (`/usr/include/Xm/Xm.h` reports
`OSF/Motif Version 1.2.4`), and 6.5 still carries 1.2, so one o32 binary covers
the whole 5.3–6.5 range. If a 6.5 box has both `/usr/Motif-1.2` and
`/usr/Motif-2.1` trees, pin the include path at 1.2 — linking 2.1 silently breaks
5.3. **Do not reach for Motif 2.x-only calls.**

What a stock 5.3 actually has (all confirmed present):

| Piece | Detail |
|-------|--------|
| Motif | `libXm.so.1`, 121 headers in `/usr/include/Xm` |
| X11 | `libXt`, `libX11`, `libXext`, `libXmu`, `libXi` |
| SGI extensions | `/usr/include/Sgm` + `libSgm.so.1` — `SgFinder`, `SgGrid`, `SgThumbWheel`, … |
| SGI look | `/usr/lib/X11/schemes/` (IndigoMagic et al), `Xsgi`, `4Dwm` |

**ViewKit is runtime-only** — `libvk.so.1` ships but there is no
`/usr/include/Vk`, so you cannot build against it. That, plus its C++/MIPSpro
`CC` requirement, is why the GUI is plain IRIS IM in C. There is also **no `uil`
binary** installed, so the widget tree is built in C rather than from UIL files.

**The SGI look comes from resources, not code.** `fallback_resources[]` sets
`*useSchemes: all` / `*schemeFileList: SgiSpec` / `*scheme: Base` / `*sgiMode:
true` — the same four every stock IRIX app sets (grep `useSchemes` in
`/usr/lib/X11/app-defaults`). Widget labels live there too, so they can be
changed without a rebuild. On a system with no schemes the fallbacks are simply
ignored and it renders as ordinary Motif.

**X11R4 gotcha:** IRIX 5.3's `Intrinsic.h` reports `XtSpecificationRelease 4`, and
`XtVaAppInitialize()` takes `Cardinal *argc` there but `int *argc` from R6 (6.5).
`gui_motif.c` picks the right type with an `XtSpecificationRelease >= 6` guard —
keep that in mind for any other Xt call that takes an argc.

**Testing it on the dev box:** `gui_motif.c` cannot be *compiled* on macOS, but
`make irix-syntax IRIX_INCLUDE=<tree>` parses it — and `irix.c` — against a copy
of a real IRIX `/usr/include` under the host compiler. That catches what matters — a widget or
resource that doesn't exist in 1.2, a nested `/*`, a C99-ism — without an IRIX
machine. The `-D` flags in the `IRIX_FAKE_CC` variable stand in for what MIPSpro
predefines (`sgidefs.h` hard-errors without `_MIPS_SZINT` et al). Warnings from
inside the IRIX headers are expected (K&R declarations); only our own files'
output matters. **Run it after any change to `gui_motif.c` or `irix.c`** — those
are the two files no other check covers, and it has already caught an X11R4-vs-R6
signature mismatch, a nested `/*`, and a wrong `printf` type in `test_dsreq_flags`.

## Build identification

`-version` on the CLI and Help > About in the GUI both report the same block.
Two generated headers back it, and the split matters:

| Header | Question | Lifecycle |
|---|---|---|
| `version.h` | *What source is this?* git rev, `-dirty`, stamp time | Generated **on the host**, then **shipped** as a static build input |
| `buildhost.h` | *Where was it compiled?* `uname -s` / `-r` | A real build product, regenerated every build |

**IRIX has no git, so the revision has to be stamped on the host.** That is the
whole reason `version.h` travels as a source file rather than being generated at
build time: anything regenerating it on the far side could only downgrade a good
revision to `unknown`. `mkversion.sh` enforces this itself — with git it writes
the header, without git it *refuses to overwrite* an existing one, and only
writes an `unknown` stub when there is nothing to preserve. The Makefile rule has
no prerequisites, so when the shipped header is present the recipe never fires.

`version.c` `#ifndef`-defaults every macro, because a shipped header can
legitimately be older than the source that reads it. A field reporting `unknown`
beats a build dying on a machine with no git and no way to regenerate.

Timestamps are reported twice on purpose: **Stamped** is when the source snapshot
was taken (identical across every artifact from one sync, ties a binary to a
commit) and **Compiled** is `__DATE__`/`__TIME__` from the build machine. And
**Built on** is what answers "5.3 or 6.5 libraries?" — a question only the
machine doing the linking can answer, which is why `buildhost.h` exists
separately.

Do not commit either header; both are `.gitignore`d. `make clean` deletes
`buildhost.h` but **not** `version.h` — the latter is an input on IRIX and
deleting it there would be unrecoverable.

## Syncing to the IRIX/IRIS drop folder

```sh
scripts/sync-irix-drop.sh [dest]     # default ~/Downloads/irixscsitb53
```

Stamps `version.h` from git, then copies the sources, `Makefile`, `README.md`,
`mkversion.sh` (as a fallback), `irix-native-build.sh` (renamed to `build.sh`)
and `HOWTO-IRIS.txt` into the drop. It leaves the drop's `build/` alone — that
is where IRIX writes its output and its log.

**Assemble the drop only with this script.** It was previously done by hand,
which meant the vintage machine's build inputs were neither reproducible nor
under version control. `linux.c` is deliberately excluded: the Makefile never
selects it on IRIX and it would not compile there.

## Known issues / gotchas

- **CD swap needs the volume UNMOUNTED first (fixed, needs re-testing):** confirmed
  on a real Indigo + BlueSCSI v2 — the new disc does not appear until `/CDROM` is
  unmounted, because the host still holds cached metadata for the disc that left.
  `mediad -k` unmounts what *mediad* mounted but not a hand-mounted volume, so
  the swap path now calls `toolbox_prepare_cd_swap()`: find the mount via
  `/etc/mtab` (matching the `dks<c>d<id>` stem), `umount` it, and on failure ask
  `fuser -c` who is holding it. A busy volume **blocks** the swap — CLI needs
  `-f`, the GUI shows a "Volume busy" dialog defaulting to Cancel.
- **`mediad` restart race (fixed, needs re-testing on hardware):** `/etc/init.d/mediad
  stop` runs `mediad -k`, which only *asks* the daemon to exit — it then unmounts
  its media before actually going. Restarting immediately raced that shutdown and
  mediad refused with *"Another mediad is running. Only one is allowed at a
  time."* The failure mode is worse than it sounds: the new mediad refuses, the
  old one then exits, and the system is left with **no** mediad — so a
  newly-switched CD never remounts. `mediad_stop()` now polls `ps` until the
  daemon is really gone (10s cap), `mediad_start()` skips a redundant start and
  warns if `chkconfig mediad` is off. Reported from a real Indigo + BlueSCSI v2.
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
