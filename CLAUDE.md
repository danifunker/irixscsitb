# CLAUDE.md

Guidance for working in this repository.

## What this is

`irixscsitb` is the **host-side** command-line companion for the Toolbox API shared by
[BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) and
[ZuluSCSI](https://github.com/ZuluSCSI/ZuluSCSI-firmware). It runs on the vintage
host (SGI **IRIX**) or on **Linux**, talks to the device over the SCSI bus using
vendor command codes `0xD0`–`0xDA`, and lets you list/fetch/send files in the
device's `/shared` directory, list and switch CD images, enumerate emulated
targets, toggle debug, and interrogate/scan the bus. It also speaks the Toolbox
**Wi-Fi** commands (`0x1C`) to the firmware's emulated network target — scan,
status, join.

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
| `wifi.c` | **The Wi-Fi core.** The `0x1C` command builders and the two-stage Wi-Fi target detection. Split from `toolbox.c` because it is a different command family (six-byte CDB, subcommands) on a *different target* (the emulated DaynaPort). Same printless contract. |
| `irixscsitb.c` | CLI front end: `getopt`, `main`/`do_drive`, and the `cli_*` functions that turn core results into stdout text. Speaks no SCSI itself. |
| `gui_motif.c` | IRIS IM (Motif 1.2) GUI front end. IRIX-only, links the same `toolbox.o`. |
| `version.c` | Build identification. The only file that includes the generated headers. |
| `irixscsitb.h` | Protocol constants (command opcodes, `TOOLBOX_API_VER`, the `0x1C` Wi-Fi subcommands and wire sizes), `scsi_inquiry` / `ToolboxFileEntry` / `ToolboxDetect` / `ToolboxScanEntry` / `ToolboxWifiNetwork` structs, modes/enums, `extern` globals, and the core API prototypes. |
| `os.h` | The OS-backend contract (see below). |
| `irix.c` | IRIX backend: `<sys/dsreq.h>` `DS_ENTER` ioctls, `mediad` start/stop. |
| `linux.c` | Linux backend: `<scsi/sg.h>` `SG_IO` ioctls. |
| `Makefile` | `uname`-based OS detection; sets `-DOS_IRIX` / `-DOS_LINUX`. |
| `meson.build` | Linux build. **Currently out of date** — missing `version.c` and the generated headers, so it does not link; see `.github/workflows/build.yaml`. |
| `scripts/mkversion.sh` | Stamps `version.h` from git. Runs on the *host*, never on IRIX. |
| `scripts/sync-irix-drop.sh` | Assembles the IRIX/IRIS drop folder. The only supported way. |
| `scripts/irix-native-build.sh` | Shipped into the drop as `build.sh`; builds natively inside IRIX. |
| `docs/HOWTO-IRIS.txt` | Shipped into the drop; instructions for the IRIX side. |
| `docs/ci-iris.md` | The IRIS-in-CI sample-project guide: work-disk transfer, CoW overlays, PROM scripting, adapting to other IRIX projects. |

**Three-layer split:** transport (`irix.c`/`linux.c`) → protocol (`toolbox.c`,
`wifi.c`) → presentation (`irixscsitb.c`, `gui_motif.c`). The core must stay printless: it
may write errors and `verbose` diagnostics to stderr, but a *result* always goes
back as a return value or a filled struct, because the GUI needs it as widget
state rather than as text. Adding an operation means a `toolbox_*` function in
`toolbox.c` (or `wifi.c`) plus a `cli_*` printer in `irixscsitb.c`.

**OS abstraction contract** (`os.h`) — `toolbox.c` and `wifi.c` only ever call
these; each backend implements them:
`scsi_open`, `scsi_close`, `scsi_send_command` (read / data-in),
`scsi_send_commandw` (write / data-out), `scsi_send_command_probe` (data-in,
single attempt, silent — used by the `-b` scanner so unanswered nodes don't cost
10 retries + warnings), `scsi_enum_devices` (list the host's generic SCSI node
paths), `path_to_devnum` (device path → SCSI ID), `mediad_start` /
`mediad_stop`. When adding a feature, keep protocol logic in `toolbox.c` and
only touch `irix.c`/`linux.c` for transport. **Adding a source file means
updating six places**: the Makefile (five flavour targets, the object rule and
the `test` target), `meson.build`, `scripts/sync-irix-drop.sh`,
`scripts/iris-build.sh` and `scripts/irix-native-build.sh` — miss one and the
IRIX build breaks in CI rather than here.

## What a build machine needs

The point is that **any** IRIX box with the right dev kit can rebuild this — no
particular disk image, and nothing hardcoded to one machine.

| For | Needs |
|---|---|
| CLI (`irixscsitb`) | IRIX 5.3–6.5, MIPSpro/IDO `cc`, `make`. Nothing else. |
| GUI (`scsitbgui`) | the above **plus** the Motif development environment — `/usr/include/Xm` and `libXm`. Plain Motif 1.2 is enough. |
| Running either | root, for the generic SCSI nodes |

The GUI links `-lXm -lXt -lXext -lX11 -lm` and deliberately **not** `-lSgm`: no
`Sg*` symbol is used, and linking it would rule out any IRIX carrying Motif
without SGI's extensions. `make` builds both and treats a failed GUI link as
non-fatal, so a machine without Motif dev still gets a working CLI.

Install locations are all overridable (`BINDIR`, `PREFIX`, `FTDIR`,
`CHESTDIR`); the only absolute paths the code itself assumes are `/dev/scsi/sc*`
and `/etc/mtab`, both standard IRIX.

**No personal paths or disk-image names belong anywhere in this repo.**
`scripts/irix-enable-dhcp.sh` therefore *requires* its image argument rather
than defaulting to one — a default pointing at one person's local file is
useless to everyone else and leaks a path.

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
logic) *can*, by linking them against `tests/mock_os.c`, a fake 9-device SCSI bus
implementing the `os.h` contract. It covers a plain disk, an IRIS EMUL DISK, a CD-ROM, a real
BlueSCSI, a real ZuluSCSI, a dead node, a "liar" that serves page 0x31 but
never implements `0xD9`, an emulated DaynaPort that answers the `0x1C` Wi-Fi
commands, and a *genuine* Dayna SCSI/Link with the byte-identical INQUIRY
identity and no radio. Expected: only BlueSCSI and ZuluSCSI are `[TOOLBOX]`;
IRIS is not; the liar reads `claims toolbox, no 0xD9 answer`; only the emulated
DaynaPort is `[WIFI]`. The test then runs `-W` and `-w` against the mock, which
exercises the whole Wi-Fi path — including the six-byte CDB (the mock rejects a
ten-byte one) and the signed-RSSI decode. **Run it after any
change to detection or the command builders** — it catches regressions that a
syntax check cannot. Add a device to `mock_paths[]`/`mock_command()` to cover
new firmware.

Run **as root**. Device path is positional:
- IRIX:  `irixscsitb -s /dev/scsi/sc0d1l0`
- Linux: `irixscsitb -s /dev/sg2`

Options: `-b` scan the bus, `-i` interrogate, `-t` list emulated targets (device
map), `-l` list CDs, `-s` list `/shared`, `-c N` switch CD, `-g N` get file N,
`-p FILE` put file, `-o DIR` output dir, `-d 0|1` set debug, `-D` show debug,
`-v` verbose. Wi-Fi: `-w` scan, `-W` status, `-j SSID` join (`-k KEY`,
`-n CHAN`). `-b` and the Wi-Fi options are the modes that need **no** device
path — `-b` because it is how you find one, Wi-Fi because the node it needs is a
different target from the disk and is located automatically. Options must
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
- **`.tar.gz`** — the same `dist53/` + `dist65/` tree as the media, plus the
  READMEs. The friendliest vector now that IRIS ships a built-in NFS server:
  drop the contents in a `[nfs] shared_dir` folder and copy from inside IRIX.
  Unlike the images (which land files 0644), the tarball carries the
  executable bits.

Both the CD and HDD address the EFS partition as **`@1`** for `put`/`get`/`ls`/`fsck`
(rb-cli maps `@1` to the sole EFS partition regardless of its slot number). The
old top-level `new --fs efs` / `new-sgi-hdd` / `new-sgi-cdrom` verbs were renamed
to the above; `package.sh` preflights for the new grammar and errors clearly if
rb-cli is too old.

```sh
scripts/package.sh --dist53-bin irixscsitb-o32 --dist53-gui scsitbgui-o32 \
  --dist65-bin irixscsitb-n32 --dist65-gui scsitbgui-n32 \
  --version 2026-06-17 --rb-cli ./rb-cli --extra README.md
```

**Each OS packages its own build.** Media carry `/dist53` and `/dist65` —
each a Software Manager distribution generated by THAT flavor's guest inside
the build session: `iris-build.sh` runs the guest's native `gendist` over
`inst/irixscsitb.spec` + `.idb` (templated per flavor by ci-lib's
`stage_inst_inputs`; one subsystem per product, same product name so
installing the other flavor replaces it), emitting `$OUTDIR/inst53` /
`inst65`. The products install the binaries AND the Toolchest fragment
(`desktop/scsitoolbox.chest` → `/usr/lib/X11/app-chests/`; static /usr/sbin
path — install.sh keeps generating its own to honor BINDIR, keep the menu
text in sync); a GUI-less build drops both the GUI and the chest from the
idb. Desktop-icon FTR activation stays install.sh-only (it edits the
filetype Makefile + runs make — not something a package should do
silently). The 5.3 product format is readable by every inst 5.3–6.5; the 6.5
product (`pd001V630...`) is 6.x-only — which is exactly why per-OS packaging
is right. `--no-gendist` / `BUILD_INST=0` skip it; a guest without
`/usr/sbin/gendist` (the `inst_dev.sw` "Software Packager" subsystem) skips
with a warning, and `scripts/iris-gendist.sh` remains as the standalone
regenerator + 5.3 provisioner (installs inst_dev.sw from `IRIX53_IDO_ISO`
into the overlay; its scripted inst session NEVER quits inst, resolves
conflicts, or verifies afterwards — after `go` reports success it suspends
inst with Ctrl-Z, because inst's quit-time machine check can demand removal
of legitimate software under an emulated hinv, which once genuinely broke a
system).

`package.sh` takes `--inst53-dir/--inst65-dir` (product trios → `/dist53`,
`/dist65`, per-flavor `.tardist`s) and `--bin53/--gui53/--bin65/--gui65`
(raw binaries → tarball `bin53/`/`bin65/` with exec bits; also the media
fallback for a flavor without a product). One payload manifest drives
populate, round-trip verify (every file `cmp`'d back out) and the tarball.
The `.iso`/`.hda` are also emitted **gzipped** (`.iso.gz`/`.hda.gz` — the
distribution artifacts; the images are mostly empty space) with raws kept
for direct IRIS attachment; `--no-gzip` skips. EFS volume label defaults to
**`SCSITB`** (`--name`, ≤6 bytes — what the IRIX desktop shows; `BSTOOL`
was a bstoolbox-era leftover). Serial-driving lessons (inst pagers, prompt
sync, never-quit-inst, 5.3 `mkdir -p` erroring on existing dirs) are in
docs/ci-iris.md.

**The products are load-bearing, so nothing may drop them silently.** The
whole `dist/` tree — binaries *and* `inst53/`/`inst65/` — is what moves
between the Actions jobs; uploading only the binaries is exactly how
v2026-08-15-14-58 shipped with no `.tardist` and raw binaries inside its
media, while every step stayed green. Three guards now close that: the
build passes `--require-gendist` (a guest that cannot package **fails the
job**), `package-dist.sh` refuses a flavor whose binary is present but whose
product is not, and `publish-release.sh` warns on the same mismatch. All
three are governed by the single `BUILD_INST` switch (`build_inst` dispatch
input / repo var, `--skip-inst` locally) — turn it off and the degraded
build is allowed *because it was asked for*. The workflow also passes
`--version` to the builds, so the inst version comes from the release rather
than from each job's own clock.

`.github/workflows/release.yml` builds BOTH flavors natively in IRIS — one
matrixed `build-native` job, prebuilt emulator binaries via
`scripts/fetch-iris.sh` (no Rust toolchain in CI) — then packages and cuts the
release. Hosted mode needs the secrets **`IRIX53_DISK_URL`** and
**`IRIX65_DISK_URL`** (installed boot disks, bare `.chd` or a `.zip` with one;
licensed IRIX — host them privately); downloads are cached keyed on the URL
hash. Self-hosted mode: dispatch with `runner_label` + `irix53_image` /
`irix65_image` local paths and the images never leave the machine (works on
Linux and macOS runners — but note the iris release tarballs currently bundle
`iris-ci` on Linux only; a self-hosted Mac needs a source-built iris-ci, and
`fetch-iris.sh` says exactly that). Optional: `IRIS_RELEASE_REPO` /
`IRIS_TAG` vars (default `danifunker/iris` @ latest) and the `iris_tag`
dispatch input. The old `IRIX_TOOLCHAIN_IMAGE` cross-compile variable is gone.

**`scripts/release-local.sh`** is the third mode: the whole pipeline on the
local machine — preflight (clean tree, HEAD pushed, `gh auth`), both native
builds, package, publish. `--dry-run` builds + packages and prints the gh
command instead; `--draft`, `--skip-n32`, `--allow-dirty` cover the partial
cases. For people whose images can live neither at a secret URL nor on a
registered runner.

**One code path everywhere:** the workflow's step bodies are one-line calls
into the same scripts release-local.sh runs — `ensure-rbcli.sh` (rb-cli from
$RB_CLI/PATH/release download), `fetch-image.sh` (boot disk from
IRIX*_IMAGE env = the dispatch inputs → ci/local.conf → IRIX*_DISK_URL
download; `--check-only` is the preflight, `--cache-key` feeds actions/cache),
`package-dist.sh` (assembles package.sh args from what was actually built —
falling back to an n32-based image set, loudly, when o32 is disabled),
`publish-release.sh` (canonical notes + artifact set). Shared flavor/conf
resolution lives in `scripts/ci-lib.sh` (sourced, parses the conf, never
executes it): **`ci/local.conf`** is the single per-machine config —
IRIX*_IMAGE, IRIX*_DISK_URL, BUILD_O32/BUILD_N32 (flavor on/off), IRIS_DIR,
IRIS_RELEASE_REPO/IRIS_TAG, RB_CLI — loaded by `load_local_conf` with flag >
env > conf precedence. Flavor toggles surface as `--skip-o32/--skip-n32` on
release-local.sh and as the `build_o32/build_n32` dispatch inputs (or
BUILD_* repo vars) in Actions, where the preflight job computes the build
matrix (`fetch-image.sh --enabled`). Only Actions-specific plumbing stays in
YAML: topology/matrix, runner selection, secret→env mapping, cache/artifact
transport, runner apt.

### Two native builders in IRIS: o32 (5.3 guest) and n32 (6.5 guest)

- **o32 (5.3-capable)** must be built **natively** — a cross-compiled o32
  may not link for 5.3 (GNU binutils can't link 5.3's o32 shared libs; see
  [[irix-o32-cross-link-blocked]]).
- **n32 (6.x only)** is built natively too, in a 6.5 guest: same script, and
  native gets the Motif GUI built where a cross sysroot has no Xm headers. On
  an Indy/IP22 running 6.5 `uname -s` is `IRIX` (not `IRIX64`), so the script
  invokes `make irix-n32` explicitly — plain `make` would auto-pick o32 there.

`scripts/iris-build.sh --flavor o32|n32 --image <boot.chd>` launches IRIS
headless (`--ci`), drives the PROM + shell over the serial control socket, and
— when `--image` is omitted — takes the boot disk from `$IRIX53_IMAGE`/
`$IRIX65_IMAGE` or from **`ci/local.conf`** (per-machine paths;
`.gitignore`d, copy `ci/local.conf.example`; may also set `IRIS_DIR`;
parsed KEY=VALUE, never sourced). It
moves files on a **work disk**: an SGI EFS `.hda` that rb-cli fills from the
staged sources (`new hd sgi-efs --from-dir`) and that the guest mounts at
`/dev/dsk/dks0d2s0` — sources in, `out/` binaries back, read on the host with
`rb-cli get`. **No networking in the guest at all** (no DHCP, no NVRAM eaddr,
no NFS), and the boot image is **never written**: `ci/iris-irix53.toml` /
`ci/iris-irix65.toml` set `overlay = true`, so guest writes land in
`<image>.chd.diff.chd` (delete it or pass `--fresh` to reset; without the flag
iris folds the diff back into the base on exit — the one thing a
keep-the-image-fresh CI must avoid). The 5.3 flavor boots **single-user** via
`sash` (`initstate=s`) so no rc2 service can hang a headless boot (a tgcware
`prngd` did exactly that during bring-up); 6.5 boots multiuser cleanly.
Guest status comes back via `echo TOKEN-'OK'` sentinels — quoted when typed,
contiguous when printed — never by parsing prompts or `iris-ci run`'s
`\nIRIS-CI-RC=` marker, which bash's ANSI color resets break; guest lines stay
csh-AND-sh clean. `--no-package`/`--bin-out`/`--gui-out` let CI jobs emit bare
binaries so packaging stays centralised in the `package` job. See
`docs/ci-iris.md` for the full sample-project write-up (image requirements,
transfer-channel options, adapting to other projects). The old NFS transfer
path remains available manually (commented `[nfs]` stanza + eaddr/DHCP notes
in `ci/iris-irix53.toml`); `scripts/irix-enable-dhcp.sh` supports it.

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
- **IRIX 5.3 `tar` can neither create nor extract compressed archives** — there
  is no `z` flag, so `tar xzf` fails. Both `make tar` and `build.sh` therefore
  emit a **plain `.tar`**, which IRIX unpacks with a bare `tar xvf foo.tar`:
  no pipe, and no gzip needed at either end. (If you ever do meet a `.tar.gz`
  here, it is `gunzip -c foo.tar.gz | tar xvf -` — `gunzip -c`, not `zcat`,
  which on IRIX is the `compress`/`.Z` one.) The payload is ~140 KB; compressing
  it would save under 100 KB and cost a dependency on both ends.
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

## How Wi-Fi detection works (and why it is a separate device)

See `docs/toolbox-protocol.md` §2.5 for the byte-level contract. The short
version, because it is the thing that trips everyone up:

**The Wi-Fi commands are answered by a different SCSI target than the toolbox
commands, and never by the same one.** The firmware serves `0x1C` on its
emulated NETWORK device (a DaynaPort SCSI/Link) and `0xD0`–`0xDA` on the disk or
CD it is emulating. It is not an accident that the network target fails toolbox
detection: `inquiry.c` explicitly skips appending the `INQUIRY_NAME` toolbox tail
for `S2S_CFG_NETWORK`. So `-b` shows the two on separate rows, `[TOOLBOX]` and
`[WIFI]`, and both tags landing on one row would mean something is wrong.

Because of that, **the Wi-Fi options take no device path** — `toolbox_wifi_find()`
walks the bus and returns the right node. Requiring a path would be requiring
the operator to know which emulated ID carries the radio, which is only visible
in the firmware's own config file.

Detection is two-stage for the same reason the toolbox's is:

1. **Claim** — the INQUIRY identity contains `SCSI/Link`. That one string covers
   both firmware personalities (`Dayna SCSI/Link 2.0f` and
   `AmigaNET SCSI/Link 1.0f`), so `wifi_firmware_ids[]` holds exactly one entry.
2. **Confirm** — `WIFI_CMD_INFO` (`0x1C`/`0x04`) must answer with a two-byte
   big-endian prefix reading exactly **74**. The firmware always reports
   `sizeof(wifi_network_entry)` there whether or not the radio has joined
   anything, so it is a reliable signature.

The confirm step is not ceremony: **a genuine vintage Dayna SCSI/Link presents
the identical INQUIRY identity and has no radio at all**, and `0x1C` is
RECEIVE DIAGNOSTIC RESULTS in standard SCSI — so a plain disk may answer the
opcode rather than reject it. Checking the *shape* of the reply is what
separates them. (That the standard meaning is a read-only command is also why
probing every node with it is safe.)

Because `0x1C` is a standard opcode, the gates in `toolbox_wifi_probe()` are a
**safety boundary, not a speed optimisation**: nothing may send a Wi-Fi CDB
without passing them. Besides the identity claim there is a
peripheral-device-type floor (`wifi_pdt_allowed()`) refusing plainly-non-network
INQUIRY types (disk, tape, CD, …); the floor is the one gate `-F` does **not**
skip — `-F` exists for "an unrecognised BlueSCSI", not for aiming RECEIVE
DIAGNOSTIC RESULTS at a disk — and a forced probe of a device that passes the
floor without being positively identified warns on stderr before sending. The
mock bus enforces the contract: any `0x1C` reaching a storage-type mock node
aborts `make test`.

Three protocol facts worth keeping in mind when editing `wifi.c`:

- **The CDB is six bytes.** Everything else in this codebase sends ten. The
  upstream firmware docs also say the subcommand is `CDB[2]`; it is `CDB[1]`,
  with a big-endian length in `CDB[3..4]`. The firmware's *code* is the
  authority, not its comment — see the note in `irixscsitb.h`.
- **`SCAN_RESULTS` before the scan finishes is a CHECK CONDITION**, not an empty
  list, so `toolbox_wifi_scan()` polls `COMPLETE` and only then fetches. It uses
  `sleep(1)` because IRIX 5.3 has no `usleep`, and second granularity is fine
  for something that takes seconds of radio time anyway.
- **`JOIN` acknowledges the request, not the association.** The firmware hands
  the credentials to the radio and answers GOOD at once, so both front ends wait
  and then issue `INFO` to find out what actually happened.

The wire structs are decoded byte by byte, never memcpy'd into a C struct: the
firmware declares them `__attribute__((packed))`, which MIPSpro has no
equivalent of. `rssi` is signed on the wire and is sign-extended explicitly —
read as a plain `char` where that is unsigned, −67 dBm becomes 189.

## The Motif GUI (`gui_motif.c`)

A **second binary** (`scsitbgui`), not a replacement. The CLI keeps zero X
dependencies so it still works headless and over a serial console; both link the
same `toolbox.o` and `wifi.o`. There is no protocol code in `gui_motif.c` at all — only
widgets and the open/act/close sequence around each core call, so the two front
ends cannot drift.

**Layout:** menu bar (File / Device / Wi-Fi / Help) over a paned window — upper
pane is the bus scan, lower pane is the `/shared` listing, the CD images or the
Wi-Fi networks (radio-selected) — with a status line in the MainWindow message
area. Full coverage of the CLI: `-b` (Rescan), `-i` (Interrogate), `-t`
(Emulated Targets), `-s`/`-l`/`-w` (the content pane), `-g` (Get File, prompts
for an output dir), `-p` (Put File, file-selection dialog), `-c` (Switch To CD),
`-D`/`-d` (the Debug menu items), `-F` (a Force detection toggle that rescans),
`-W` (Wi-Fi ▸ Current Network) and `-j`/`-k` (the Join dialog).

**Operation buttons are gated on `confirmed`**, not on `claims` — a device that
advertised the toolbox but failed 0xD9 leaves them greyed, and the dialog
explains which of the two stages it failed. Same rule as the CLI, surfaced.

**Wi-Fi is gated on something else entirely**, and has to be: `wifi_index()`
finds the Wi-Fi row rather than using the selected one, because the selected
device is by definition *not* the radio (different target — see the Wi-Fi
detection section). Wi-Fi therefore gets its own `require_wifi()`/`open_wifi()`
pair and its own menu rather than more entries under Device, which would imply
it acts on the selection.

Three Wi-Fi-specific pieces of GUI behaviour worth preserving:

- **Switching the lower pane to Wi-Fi does not scan.** The other two listings are
  one quick command; a scan is seconds of radio time during which the core
  blocks and the event loop stops. Clicking a radio button should not do that,
  so it waits for Refresh (or Wi-Fi ▸ Scan For Networks).
- **`set_busy()` exists because the freeze is unavoidable.** It sets the watch
  cursor and pushes the status text out with `XmUpdateDisplay()` — the expose
  that would normally paint it cannot be processed until the core returns.
- **The password field masks by hand.** Motif 1.2 has no password widget, so an
  `XmNmodifyVerifyCallback` applies each edit to `join_key_text` (using the
  callback's `startPos`/`endPos`/`text`, which is exact for insert, delete,
  replace and paste alike) and then overwrites the inserted characters with
  `*` before Motif draws them. Anything that doesn't add up sets `doit = False`
  rather than risking a password nobody typed. The buffer is wiped after use.

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
| SGI extensions | `/usr/include/Sgm` + `libSgm.so.1` — present, but **not linked**: nothing uses `Sg*`, and linking it would exclude any IRIX without SGI's Motif extensions |
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

## Installing on a target IRIX machine

`scripts/irix-install.sh` ships as `output/install.sh` and does three things,
each of which can fail independently:

| Piece | Where | Visible after |
|---|---|---|
| binaries | `$BINDIR`, default **`/usr/sbin`** | immediately |
| Toolchest entry | `/usr/lib/X11/app-chests/scsitoolbox.chest` | WM restart (`tellwm restart`) |
| desktop icon | `/usr/lib/filetype/local/` + rebuilt type DB | desktop restart / reboot |

**`/usr/sbin`, not `/usr/local/bin`** — both tools need root for the generic
SCSI nodes, it is where IRIX keeps administrative binaries, and it is on root's
default PATH, which `/usr/local/bin` is not and may not even exist. `BINDIR`
overrides; `/usr/bin/X11` is the other idiomatic home (where IRIX keeps X
clients).

**The Toolchest entry never edits a system file.** `system.chestrc` ends with
`sinclude /usr/lib/X11/app-chests`, so that directory is a drop-in: a `*.chest`
fragment there is pulled in wholesale. `Menu ToolChest` is additive —
system.chestrc reopens it after the include to append Help — which is what lets
a fragment add a top-level item. The fragment is *generated* by the installer
rather than shipped static, because the path has to match wherever `BINDIR` put
the binary. `f.checkexec.sh` means the entry hides itself if the binary is gone.

**Install from the local staged copy, not from `output/`.** An NFS share
generally will not keep an execute bit on a file the *guest* created, so
`output/install.sh` ends up non-executable even though `build.sh` chmods it —
the chmod silently does nothing. The symptom is confusing: `./install.sh` gives
permission denied while `/bin/sh ./install.sh` works, and `build.sh` itself runs
fine because *it* was made executable on the host by `sync-irix-drop.sh`. The
same package is staged on local disk at
`$HOME/irixscsitb-build/irixscsitb-<rev>/`, where the exec bit sticks;
`build.sh` points at that and warns when the share copy is not executable.

`scripts/irix-uninstall.sh` ships as `output/uninstall.sh` and reverses all
three, including taking its own line back out of the filetype Makefile (by
filtering, *not* by restoring install.sh's backup — that would discard anything
else installed since). It searches the plausible binary directories rather than
only the default, so a `BINDIR=` install is still found. `DRYRUN=1` lists
without touching, and needs no root.

The FTR Makefile patch inserts before the `${NULL}` terminator of `FTR_FILES`
using `awk index()` (no `${...}` escaping), keeps a backup, and is idempotent.
Both it and the whole `build.sh` flow are tested off-target — the awk against
the real 5.3 Makefile, the build against a stub `make`, and the installer
against a fake tree via the `FTDIR`/`CHESTDIR`/`BINDIR` overrides that exist
for exactly that purpose.

## Wrapping a build in EFS (CD / HDD image)

```sh
scripts/make-efs-iso.sh --tar output/irixscsitb-<rev>.tar [--hdd]
```

Host-side (rb-cli is a host tool). Takes whatever `build.sh` packaged and wraps
it in an IRIX EFS CD image, optionally an SGI `.hda` too. Distinct from
`scripts/package.sh`, which builds release artifacts around a bare binary.

The disc carries **just the tarball** — one file, extracted on IRIX with a bare
`tar xvf`, modes restored by tar itself, no `chmod` step.

**EFS holds execute bits perfectly well** — an earlier note here claiming
otherwise was wrong. `rb-cli put` of an 0755 host file lands 0755 in the image.
What is broken is **`rb-cli untar`**, which discards the mode from the tar header
and writes 0644. Minimal repro on the same image:

```sh
chmod 755 x.sh
rb-cli optical new sgi-efs t.iso --size 4M --name MODET
rb-cli put   t.iso@1 x.sh /x.sh      # -> ls -o shows -rwxr-xr-x   correct
tar cf x.tar x.sh                    # tar tvf confirms -rwxr-xr-x
rb-cli untar t.iso@1 x.tar /         # -> ls -o shows -rw-r--r--   WRONG
```

`put` reports `mode 0755 (from host file)`; `untar` reports nothing and stores
0644. So the fault is in rb-cli's tar importer, **not** in the EFS/optical layer
— `put` proves that layer round-trips modes correctly. Worth fixing upstream in
rusty-backup; until then, ship the tar rather than an imported tree.

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
- **Prebuilt iris pinning: don't pin `IRIS_TAG` before v2026-07-28-20-04** —
  older releases bundled `iris-ci` on linux x64/arm64 only (and used varying
  archive layouts). From that release on, every `IRIS-cli-*` archive ships
  `iris` + `iris-ci` flat on all targets (verified 2026-07-28, incl. a full
  o32 build driven by the prebuilt macOS pair). `scripts/fetch-iris.sh`
  extracts layout-tolerantly and fails with the exact workaround if an old
  tag is pinned. Since v2026-08-13-11-14 the variants are per-emulated-CPU
  (`r4400`/`r5000`, no more `lightning`); the script tries `r4400` first and
  falls back to `lightning`, so tags on either side of the rename both work.
- **Wi-Fi is written from the firmware source and two host implementations, and
  has NOT yet been run against real hardware.** The protocol was taken from
  BlueSCSI's `lib/SCSI2SD/src/firmware/network.c` / `network.h` and
  cross-checked against jcs's Macintosh `wifi_da` and SonnyJim's `bswifi`; the
  mock bus in `make test` covers the whole path end to end, but a board with a
  radio is still the real test. SonnyJim's own note on `bswifi` — "mostly
  working apart from info command not returning BSSID correctly" — is worth
  checking here too: this implementation reads the BSSID from bytes 64–69 of
  the entry, which is where `network.h` puts it.
- **Wi-Fi limits are structural, not ours:** 63-character network names and
  passwords (the wire fields are 64 bytes *including* the terminator) and at
  most 10 networks per scan (`WIFI_NETWORK_LIST_ENTRY_COUNT`).
- **DaynaPort drivers on 5.3 are a separate, later job.** The Wi-Fi commands
  work regardless — they are plain vendor SCSI commands to the network target,
  and need no networking stack on the host. Configuring the link once joined
  is what needs the driver.
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
