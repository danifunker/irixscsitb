# BlueSCSI toolbox for SGI IRIX and Linux
Download files and run 'make', it should spit out a bstoolbox binary.  Currently tested on the following platforms:

Linux Mint

NixOS

IRIX 6.5

This fork also targets IRIX 5.3 through 6.5 and supports the **IRIS emulator** in
addition to real BlueSCSI hardware. Toolbox-capable targets are detected by their
INQUIRY identity (`BlueSCSI`, or IRIS's `IRIS EMUL DISK`) or by the MODE SENSE
page 0x31 vendor page. The IRIX host write path that made `-p` (put-to-shared)
appear to hang has been reworked (pending end-to-end verification).

For one binary that runs across IRIX 5.3–6.5, build with the o32 ABI:
`make irix-o32`.

**Treat this software as ALPHA, back up any important data before using it!**

## Usage
```
Please specify device ("/dev/scsi/..."

Usage:   bstoolbox [options] [device]

Example: bstoolbox -s /dev/scsi/sc0d1l0

Options:
        -h      : display this help message and exit
        -v      : be verbose
        -i      : interrogate BlueSCSI and return version
        -l      : list available CDs
        -s      : List /shared directory
        -c num  : change to CD number (1, 2, etc)
        -g num  : get file from shared directory (1, 2, etc)
        -p file : put file to shared directory
        -o dir  : set output directory, defaults to current
        -d num  : set debug mode (0 = off, 1 - on)


Please make sure you run the program as root.
```
