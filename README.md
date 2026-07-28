# irixscsitb — toolbox for emulated SCSI devices on SGI IRIX and Linux

`irixscsitb` is the host-side companion for **BlueSCSI** and **ZuluSCSI** emulated SCSI
devices. It speaks the Toolbox API (SCSI vendor commands `0xD0`–`0xDA`) to list
and transfer files in the device's shared directory, list and swap CD images,
enumerate the emulated targets, and toggle firmware debug logging.

It is the IRIX/Linux counterpart to
[escsitoolbox](https://github.com/nielsmh/escsitoolbox) (the DOS/Windows tool for
the same firmware), and started life as a fork of
[SonnyJim/bstoolbox](https://github.com/SonnyJim/bstoolbox).

Download the files and run `make`. On IRIX that builds both the CLI and the
Motif GUI; `NOGUI=1 make` builds the CLI alone.

**To rebuild on any IRIX machine** you need MIPSpro/IDO (`cc`, `make`) for the
CLI, plus the Motif development environment (`/usr/include/Xm`, `libXm`) for the
GUI. A machine without Motif dev still gets a working CLI — the GUI link failing
is treated as non-fatal. Targets:

| Host | Status |
|------|--------|
| IRIX 5.3 – 6.5 | o32 build (`make irix-o32`) runs across the whole range |
| IRIX 6.5 | n32 build (`make irix-n32`) is faster, 6.x only |
| Linux (Mint, NixOS, …) | `/dev/sgN` via `SG_IO` |

## Motif GUI (IRIX)

`make irix-gui-o32` builds `scsitbgui`, a native **IRIS IM (Motif)** front end,
alongside the command-line tool. It opens with the SCSI bus already scanned and
the toolbox-capable devices flagged, same as `irixscsitb -b`.

It does everything the CLI does:

| In the GUI | CLI equivalent |
|---|---|
| Upper pane — the SCSI bus, `[TOOLBOX]` flagged | `-b` |
| Lower pane — **Shared files** / **CD images** | `-s` / `-l` |
| **Get File…** (prompts for an output directory) | `-g` |
| **Put File…** (file-selection dialog) | `-p` |
| **Switch To CD** | `-c` |
| Device ▸ **Interrogate…** | `-i` |
| Device ▸ **Emulated Targets…** | `-t` |
| Device ▸ **Show / Turn Debug On / Off** | `-D` / `-d` |
| Device ▸ **Force detection** | `-F` |

Switching a CD unmounts the old volume first — swapping the image under a live
mount leaves the host with cached metadata for a disc that is gone, so the new
disc never appears. If the volume is busy the switch is **refused** and you are
told which processes hold it; `-f` (CLI) or "Switch Anyway" (GUI) overrides.

The operation buttons stay greyed out unless the selected device *proved* it
implements the toolbox, and the dialogs tell you which detection stage a device
failed rather than letting each command fail on its own.

It is a separate binary on purpose — the CLI keeps no X dependency, so it still
works on a headless machine or down a serial console. Both share the same
protocol core, so anything one can do the other can.

Built against the **Motif 1.2** API, which IRIX 5.3 ships and 6.5 still carries,
so the o32 GUI binary runs across the whole 5.3 – 6.5 range just like the CLI.
It picks up the SGI scheme (IndigoMagic and friends) when the host has schemes
installed, and falls back to plain Motif when it doesn't.

## Firmware detection — no hardcoded product names

A device is only driven as a toolbox target if it **proves** it is one:

1. **Claim** — it carries a known toolbox *firmware* name (`BlueSCSI` or
   `ZuluSCSI`, stamped into the vendor area at INQUIRY byte 36), or serves the
   MODE SENSE page `0x31` vendor page.
2. **Confirm** — it actually answers `TOOLBOX_LIST_DEVICES` (`0xD9`) with a valid
   8-byte device-type map.

A claim alone is never enough, and no emulated *product* name is trusted. This
matters in practice: the IRIS emulator presents itself as `SGI / IRIS EMUL DISK`
but implements none of `0xD0`–`0xDA`, so it is correctly ignored rather than
accepted and then failing on every operation.

Any firmware that implements the toolbox is picked up automatically — **no code
change needed**. If a device implements the commands but advertises neither
signal, `-F` skips the claim check and tests it with a real toolbox command.

**Treat this software as ALPHA, back up any important data before using it!**

## Installing on IRIX (release artifacts)

Each release ships the binaries three ways so you can pick whatever your setup
makes easy — for the IRIS emulator or real SGI/BlueSCSI hardware:

| Artifact | What it is | How to use |
|----------|-----------|------------|
| `irixscsitb-*.tar` | binaries (`o32` + `n32`) + README (plain tar — IRIX 5.3 cannot extract `.gz`) | Easiest with the IRIS emulator's built-in NFS server: point `[nfs] shared_dir` at a folder, drop the extracted files in, then inside IRIX `mount 192.168.0.1:/ /mnt && cp /mnt/irixscsitb-o32 /usr/local/bin/irixscsitb`. Also works over ftp/rcp. Carries the executable bit. |
| `irixscsitb-*.iso` | IRIX EFS CD-ROM image | Attach as a CD in IRIS (`cdrom = true`) or burn it. On IRIX: `mount -t efs -o ro /dev/dsk/dks0d<N>s7 /CDROM`, then copy `irixscsitb` off and `chmod +x` it. |
| `irixscsitb-*.hda` | SGI EFS hard-disk image (dvh + EFS root) | Attach as a SCSI disk in IRIS (`cdrom = false`). Mount the EFS root, copy `irixscsitb` off, `chmod +x`. |

Use the **`o32`** binary for anything from IRIX 5.3 through 6.5; the **`n32`**
binary is a faster 6.x-only build. The `.iso`/`.hda` store the binary mode 0644,
so `chmod +x irixscsitb` after copying it off; the `.tar.gz` is already executable.

## CI: built natively on IRIX, inside the IRIS emulator

The release artifacts are compiled by **real IRIX**. The pipeline boots
installed IRIX 5.3 and 6.5 system disks headless in the
[IRIS emulator](https://github.com/danifunker/iris) and drives each guest's
own MIPSpro `cc` over the emulated serial console. This repo doubles as a
**sample project** for building any IRIX software this way — the deep-dive is
[`docs/ci-iris.md`](docs/ci-iris.md); the short version:

- **Native, twice.** The o32 binaries come from an IRIX 5.3 guest (a GNU
  cross-toolchain cannot *link* for 5.3, and no cross sysroot has Motif
  headers for the GUI); the faster n32 binaries come from an IRIX 6.5 guest.
- **No networking in the guest.** Sources ride in — and binaries ride out —
  on an EFS "work disk" built per run by
  [rb-cli](https://github.com/danifunker/rusty-backup) and attached as a
  second SCSI drive. No DHCP, no NVRAM MAC, no NFS setup in your image.
- **Your boot disk is never modified.** Copy-on-write: every guest write goes
  to a `<image>.chd.diff.chd` sidecar; delete it (or pass `--fresh`) to reset.
- **One code path.** The GitHub Actions workflow, a self-hosted runner, and a
  plain local run all execute the *same* scripts (`scripts/fetch-iris.sh`,
  `ensure-rbcli.sh`, `fetch-image.sh`, `iris-build.sh`, `package-dist.sh`,
  `publish-release.sh`) — the YAML step bodies are one-liners.

### Setup (once): `ci/local.conf`

All per-machine configuration lives in one gitignore'd file:

```sh
scripts/fetch-iris.sh --dir ../iris                     # prebuilt emulator pair
cp ci/local.conf.example ci/local.conf && $EDITOR ci/local.conf
```

Every key is optional except the image(s) you build from, and every key can
also arrive as an environment variable or a command-line flag (flag > env >
conf):

| `ci/local.conf` key | What it sets |
|---|---|
| `IRIX53_IMAGE` / `IRIX65_IMAGE` | paths to your installed dev boot disks (`.chd`) |
| `IRIX53_DISK_URL` / `IRIX65_DISK_URL` | private download URLs instead of local paths |
| `BUILD_O32=0` / `BUILD_N32=0` | disable a flavor (only have one image? turn the other off) |
| `IRIS_DIR` | where the emulator lives [`../iris`] |
| `IRIS_RELEASE_REPO` / `IRIS_TAG` | which iris releases to fetch, optional version pin [`danifunker/iris` @ `latest`] |
| `RB_CLI` | rb-cli binary [PATH, else auto-downloaded] |

### Everyday commands

```sh
scripts/iris-build.sh --flavor o32 --version 1.0   # o32 CLI+GUI + .iso/.hda/.tar.gz
scripts/iris-build.sh --flavor n32                 # n32 CLI+GUI (binaries only)
scripts/release-local.sh --dry-run                 # rehearse a full release
scripts/release-local.sh                           # build both + publish via gh
```

### Three ways to cut a release

| Mode | Where the images live | Needs |
|---|---|---|
| **Hosted Actions** (tag push or dispatch) | private URLs | `IRIX53_DISK_URL` + `IRIX65_DISK_URL` secrets |
| **Self-hosted Actions** (dispatch with `runner_label` + `irix53_image`/`irix65_image`) | on your runner | a registered runner |
| **`scripts/release-local.sh`** | on your machine | just `gh` (`--dry-run` to rehearse, `--draft` to stage) |

All three publish the identical artifact set. A flavor can be skipped
anywhere: `BUILD_O32`/`BUILD_N32` in `ci/local.conf`, the `build_o32`/
`build_n32` dispatch inputs (or repo variables) in Actions, or
`--skip-o32`/`--skip-n32` on `release-local.sh` — packaging adapts to
whatever was built (an n32-only release loudly notes its media is 6.x-only).

## Usage

Not sure which device is your BlueSCSI/ZuluSCSI? Run **`irixscsitb -b`** — it scans
every generic SCSI node, sends `INQUIRY` to each, and prints what answered with a
`[TOOLBOX]` marker on the ones you can actually drive. It is the only option that
takes no device path:

```
# irixscsitb -b
Scanning 4 SCSI device node(s)...

DEVICE                 TYPE     IDENTITY
/dev/scsi/sc0d0l0      Disk     SGI IBM DORS-32160 1.0
/dev/scsi/sc0d1l0      Disk     SGI IRIS EMUL DISK 1.0
/dev/scsi/sc0d2l0      CD-ROM   SONY CD-ROM CDU-76S 1.0
/dev/scsi/sc0d3l0      Disk     QUANTUM ZuluSCSI 1.0 ZuluSCSI v2024.05.17  [TOOLBOX]

4 device(s) answered, 1 toolbox-capable.
Pass one of the [TOOLBOX] paths to the other options, e.g. -i <device>
```

A device that advertises the toolbox but fails the `0xD9` confirmation is shown
as `[claims toolbox, no 0xD9 answer]` rather than being silently trusted.

Every other option needs a device path, and because IRIX `getopt` does not
reorder arguments the **options must come before the path**
(`irixscsitb -s /dev/scsi/sc0d1l0`, not `irixscsitb /dev/scsi/sc0d1l0 -s`).

```
Usage:   irixscsitb [options] [device]

Example: irixscsitb -s /dev/scsi/sc0d1l0

Options:
        -h      : display this help message and exit
        -v      : be verbose
        -b      : scan the SCSI bus and list every device (NO device path needed)
        -i      : interrogate the device and report firmware/toolbox info
        -t      : list emulated SCSI targets (device map)
        -l      : list available CDs
        -s      : List /shared directory
        -c num  : change to CD number (1, 2, etc)
        -g num  : get file from shared directory (1, 2, etc)
        -p file : put file to shared directory
        -o dir  : set output directory, defaults to current
        -d num  : set debug mode (0 = off, 1 = on)
        -D      : show current debug mode
        -F      : skip the identity check; test the device with a real toolbox command
        -V      : show build revision/date and exit (also -version)
```

`irixscsitb -version` (or `-V`, or Help > About in the GUI) reports the git
revision the binary was built from, whether that tree was dirty, when it was
stamped and compiled, the ABI, and which system's libraries it was linked
against — so a binary found on a disk years later can still be traced back.

Run as root — the generic SCSI nodes are root-owned. `irixscsitb` checks `geteuid()`
and only warns about this when you are *not* root, so the reminder appears when
it is actually relevant.
