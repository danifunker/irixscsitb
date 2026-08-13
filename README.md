# irixscsitb — toolbox for emulated SCSI devices on SGI IRIX and Linux

`irixscsitb` is the host-side companion for **BlueSCSI** and **ZuluSCSI** emulated SCSI
devices. It speaks the Toolbox API (SCSI vendor commands `0xD0`–`0xDA`) to list
and transfer files in the device's shared directory, list and swap CD images,
enumerate the emulated targets, and toggle firmware debug logging — and the
Toolbox Wi-Fi commands (`0x1C`) to scan for wireless networks and join one.

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
| Lower pane — **Wi-Fi networks** + Wi-Fi ▸ **Scan For Networks** | `-w` |
| Wi-Fi ▸ **Current Network…** | `-W` |
| Wi-Fi ▸ **Join Network…** / **Join…** button | `-j` / `-k` |

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

## Wi-Fi

BlueSCSI and ZuluSCSI boards with a radio expose it through the Toolbox Wi-Fi
commands (`0x1C`). `irixscsitb` can scan for networks, show which one the board
is on, and join one:

```
# irixscsitb -w
Using Wi-Fi device /dev/scsi/sc0d4l0
Scanning for Wi-Fi networks (this takes a few seconds)...

Found 3 network(s):

#1  Indigo Magic
    ####  -42 dBm   channel 6    secured   de:ad:be:ef:00:00

#2  4Dwm
    ##..  -71 dBm   channel 11   secured   de:ad:be:ef:00:01

#3  open-guest
    ....  -88 dBm   channel 1    open      de:ad:be:ef:00:02

Join one with:  irixscsitb -j '<name>' -k '<password>'

# irixscsitb -j 'Indigo Magic' -k 'hunter2'
# irixscsitb -W
```

**The Wi-Fi commands go to a different device from everything else.** The
firmware answers them on its emulated **network** target — a DaynaPort
SCSI/Link — which is a separate SCSI ID with its own device node, and which does
not implement the toolbox commands at all. The disk you pass to `-s` and `-l` is
the wrong device here and will simply not answer.

Rather than make you work out which node that is, **the Wi-Fi options take no
device path**: they find it themselves. `-b` marks it `[WIFI]` if you want to
see it. Passing a path explicitly still works, and if it is the wrong one you
are told which node to use instead.

Finding it is functional, not name-based, for the same reason toolbox detection
is: a *genuine* vintage Dayna SCSI/Link presents the identical INQUIRY identity
and has no radio at all. A node only counts as the Wi-Fi device once it answers
`0x1C`/`0x04` with a well-formed reply.

Two limits are structural to the protocol: network names and passwords are
capped at 63 characters, and a scan returns at most 10 networks. A join reports
only that the *request* was accepted — the firmware hands the credentials to the
radio and answers immediately — so both front ends wait a few seconds and then
ask the device what it is actually joined to.

In the GUI this is the **Wi-Fi** menu and a third choice in the lower pane. A
scan blocks for several seconds of real radio time, so switching the pane to
Wi-Fi does *not* scan by itself; press **Refresh** (or Wi-Fi ▸ Scan For
Networks) when you want one.

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
| `irixscsitb-*-53.tardist` | **Software Manager package (o32)** — built *and packaged* on IRIX 5.3, readable by every inst 5.3–6.5 | Download onto IRIX, open with swmgr — or `inst -f irixscsitb-*-53.tardist`. |
| `irixscsitb-*-65.tardist` | **Software Manager package (n32)** — built and packaged on IRIX 6.5, 6.x only | Same, on 6.x. |
| `irixscsitb-*.iso.gz` | IRIX EFS CD-ROM image (volume `SCSITB`), gzipped | `gunzip`, then attach as a CD in IRIS (`cdrom = true`) or burn it. mediad mounts it at `/CDROM`; then **`inst -f /CDROM/dist53`** (or `dist65` on 6.x). |
| `irixscsitb-*.hda.gz` | SGI EFS hard-disk image (dvh + EFS root), gzipped | `gunzip`, attach as a SCSI disk in IRIS (`cdrom = false`), mount, `inst -f` from the mounted `dist53`/`dist65`. |
| `irixscsitb-*.tar.gz` | the media tree + raw binaries (`bin53/`, `bin65/`, executable bits set) | Easiest with the IRIS emulator's built-in NFS server: drop the extracted tree in a `[nfs] shared_dir` folder, then inside IRIX `mount 192.168.0.1:/ /mnt && cp /mnt/*/bin53/irixscsitb /usr/sbin/`. |

The packages install the binaries into `/usr/sbin` **and the Toolchest
entry** ("SCSI Toolbox", visible after the window manager restarts; it hides
itself if the GUI is removed). Every medium carries **`/dist53`** and
**`/dist65`** — each a complete
Software Manager distribution **packaged by the OS that built it** (the 5.3
guest's own `gendist` packages the o32 build in the 5.3 product format that
every inst through 6.5 reads; the 6.5 guest packages its n32 build) — plus a
`README-dist.txt` saying exactly that. The images ship gzipped because they
are mostly empty space; the raw `.iso`/`.hda` also come out of a local build
for direct IRIS attachment.

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
/dev/scsi/sc0d4l0      Proc     Dayna SCSI/Link 2.0f  [WIFI]

5 device(s) answered, 1 toolbox-capable, 1 with Wi-Fi.
Pass one of the [TOOLBOX] paths to the other options, e.g. -i <device>
The [WIFI] path answers -w / -W / -j; those options find it themselves,
so you can leave the device path off for Wi-Fi.
```

A device that advertises the toolbox but fails the `0xD9` confirmation is shown
as `[claims toolbox, no 0xD9 answer]` rather than being silently trusted.

Note the two markers land on **different rows**, and always will: the firmware
answers the toolbox commands on the disk it is emulating and the Wi-Fi commands
on its emulated DaynaPort network target — a separate SCSI ID with its own
device node. See **Wi-Fi** below.

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
        -f      : force a CD switch even if the volume is still mounted (risky)
        -V      : show build revision/date and exit (also -version)

Wi-Fi (needs NO device path - the network target is found automatically):
        -w      : scan for Wi-Fi networks and list them
        -W      : show the Wi-Fi network currently joined
        -j ssid : join the named Wi-Fi network
        -k key  : password for -j (omit for an open network)
        -n num  : channel for -j (default 0 = let the device choose)
```

`irixscsitb -version` (or `-V`, or Help > About in the GUI) reports the git
revision the binary was built from, whether that tree was dirty, when it was
stamped and compiled, the ABI, and which system's libraries it was linked
against — so a binary found on a disk years later can still be traced back.

Run as root — the generic SCSI nodes are root-owned. `irixscsitb` checks `geteuid()`
and only warns about this when you are *not* root, so the reminder appears when
it is actually relevant.
