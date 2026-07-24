# irixscsitb — toolbox for emulated SCSI devices on SGI IRIX and Linux

`irixscsitb` is the host-side companion for **BlueSCSI** and **ZuluSCSI** emulated SCSI
devices. It speaks the Toolbox API (SCSI vendor commands `0xD0`–`0xDA`) to list
and transfer files in the device's shared directory, list and swap CD images,
enumerate the emulated targets, and toggle firmware debug logging.

It is the IRIX/Linux counterpart to
[escsitoolbox](https://github.com/nielsmh/escsitoolbox) (the DOS/Windows tool for
the same firmware), and started life as a fork of
[SonnyJim/bstoolbox](https://github.com/SonnyJim/bstoolbox).

Download the files and run `make`; it produces an `irixscsitb` binary. Targets:

| Host | Status |
|------|--------|
| IRIX 5.3 – 6.5 | o32 build (`make irix-o32`) runs across the whole range |
| IRIX 6.5 | n32 build (`make irix-n32`) is faster, 6.x only |
| Linux (Mint, NixOS, …) | `/dev/sgN` via `SG_IO` |

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
| `irixscsitb-*.tar.gz` | binaries (`o32` + `n32`) + README | Easiest with the IRIS emulator's built-in NFS server: point `[nfs] shared_dir` at a folder, drop the extracted files in, then inside IRIX `mount 192.168.0.1:/ /mnt && cp /mnt/irixscsitb-o32 /usr/local/bin/irixscsitb`. Also works over ftp/rcp. Carries the executable bit. |
| `irixscsitb-*.iso` | IRIX EFS CD-ROM image | Attach as a CD in IRIS (`cdrom = true`) or burn it. On IRIX: `mount -t efs -o ro /dev/dsk/dks0d<N>s7 /CDROM`, then copy `irixscsitb` off and `chmod +x` it. |
| `irixscsitb-*.hda` | SGI EFS hard-disk image (dvh + EFS root) | Attach as a SCSI disk in IRIS (`cdrom = false`). Mount the EFS root, copy `irixscsitb` off, `chmod +x`. |

Use the **`o32`** binary for anything from IRIX 5.3 through 6.5; the **`n32`**
binary is a faster 6.x-only build. The `.iso`/`.hda` store the binary mode 0644,
so `chmod +x irixscsitb` after copying it off; the `.tar.gz` is already executable.

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
```

Run as root — the generic SCSI nodes are root-owned. `irixscsitb` checks `geteuid()`
and only warns about this when you are *not* root, so the reminder appears when
it is actually relevant.
