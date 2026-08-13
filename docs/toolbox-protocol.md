# BlueSCSI detection & toolbox protocol (host ⇄ device)

This documents exactly what `irixscsitb` (the host tool in this repo) sends and
what it expects back, so the **IRIS emulator** can present a SCSI target that
`irixscsitb` recognises as a BlueSCSI and drives correctly.

All multi-byte SCSI fields are **big-endian**. The host's view of the protocol
lives in `irixscsitb.c`; opcodes in `irixscsitb.h`. Cross-checked against BlueSCSI
firmware `src/BlueSCSI_Toolbox.cpp` and `lib/SCSI2SD/src/firmware/inquiry.c`.

## 1. How the host decides "this is a toolbox target"

`do_drive()` calls `bluescsi_inquiry()` before *any* operation. If it returns
non-zero, the tool prints an error and exits. A device is **accepted if EITHER**
of the following matches (`irixscsitb.c`, `toolbox_accept_ids[] = {"BlueSCSI",
"IRIS EMUL DISK"}`):

1. **INQUIRY identity** (§1a) contains an accepted id, **or**
2. **MODE SENSE page 0x31** (§1b) carries the toolbox magic.

There is also a soft API-version check (§1c). The `0xD9` device-type map is
fetched after acceptance and is **non-fatal** (§1d).

### 1a. INQUIRY identity match

Host sends a 6-byte `INQUIRY` (`12 00 00 00 42 00`, allocation length 66) and
parses: `vendor_id = buf[8..15]`, `product_id = buf[16..31]`,
`product_rev = buf[32..63]`. It builds the string `"<vendor> <product> <rev>"`
and accepts if any `toolbox_accept_ids[]` entry is a substring:

- **Real BlueSCSI** appends `"BlueSCSI<ver>"` into `product_rev` (bytes 36+), so
  `"BlueSCSI"` matches.
- **IRIS emulator** presents a native SGI hard-disk identity — **vendor `SGI`,
  product `IRIS EMUL DISK` (bytes 16–31), revision `1.0`** — so `"IRIS EMUL
  DISK"` matches. (The IRIS CD-ROM masquerades as a Sony `CDU-76S`; CD identity
  is not used for acceptance, only the `0xD9` map gates CD ops.)

To add another accepted device, append its identifying substring to
`toolbox_accept_ids[]`.

### 1b. MODE SENSE page 0x31 (authoritative toolbox magic)

If the INQUIRY identity doesn't match, the host sends `MODE SENSE(6)` for the
BlueSCSI vendor page `0x31` and scans the whole reply (NUL-tolerant) for any
accepted id:

```
1A 08 31 00 60 00        # MODE SENSE(6), DBD=1 (no block descriptors),
                         # page 0x31, allocation length 0x60
```

The firmware emits this page **only when toolbox mode is enabled**
(`mode.c`: `scsiToolboxEnabled() && pageCode==0x31`). The page is:

```
0x31, 42,                                 # page code, page length (0x2A)
"BlueSCSI is the BEST STOLEN FROM BLUESCSI\0"   # 42 bytes of magic
```

So a device is accepted here if its page 0x31 content contains `"BlueSCSI"`.
**This is how IRIS signals toolbox capability** — emit page 0x31 with the magic
above (the host matches the `"BlueSCSI"` substring), in addition to (or instead
of) the INQUIRY `IRIS EMUL DISK` identity.

### 1c. Toolbox API version byte (soft — warns only)

```c
additional_len     = buf[4];
toolbox_api_version = buf[additional_len + 4];   /* last valid byte */
if (toolbox_api_version < BLUESCSI_TOOLBOX_API_VER /*=1*/) { warn; /* no fail */ }
```

Real firmware returns `0` (`< 1`), so the host warns and continues — never a
failure. For a plain SGI INQUIRY (IRIS) this byte is just whatever lands at that
offset; harmless.

### 1d. `0xD9` device-type map (non-fatal)

After acceptance the host calls `bluescsi_listdevices()` (`0xD9`) to populate
`device_list[]`, used to gate CD operations (§2.4). A failure here only warns —
shared-directory operations still work. (IRIS's `0xD9` already returns its 8
bytes, so this succeeds for IRIS.)

### Exact INQUIRY layout to emulate (BlueSCSI-style, optional for IRIS)

> IRIS does **not** need this appended-name form — its native `SGI` /
> `IRIS EMUL DISK` identity plus page 0x31 is enough. This describes how *real
> BlueSCSI* builds its INQUIRY, for reference.

Mirror what the firmware builds (`s2s_getStandardInquiry`). For a hard disk
target whose firmware name string is `NAME` (e.g. `"BlueSCSI Picov2026.04.28"`,
length `L`):

```
off  val          meaning
0    0x00         peripheral device type (0x00 = direct-access/HDD; 0x05 = CD-ROM)
1    0x00         RMB/modifier (set 0x80 for removable)
2    0x02         ANSI version = SCSI-2
3    0x01/0x02    response data format
4    0x1F+L+1     ADDITIONAL LENGTH  (so total length = 5 + this)
5    0x00
6    0x00
7    0x18         flags (sync + linked cmds)
8   ..15          vendor id, 8 bytes, space-padded   (any value, e.g. "QUANTUM ")
16  ..31          product id, 16 bytes, space-padded (any value)
32  ..35          product revision, 4 bytes          (any value, e.g. "1.0 ")
36  ..36+L-1      NAME string, must start with/contain "BlueSCSI"   <-- detection
36+L             TOOLBOX API VERSION byte (0 = like hardware)       <-- last byte
```

Total returned length = `36 + L + 1`. With `buf[4] = 0x1F + L + 1`, the host's
`buf[buf[4]+4]` lands exactly on the API byte. Simplest correct emulator: return
the standard 36 bytes, append `"BlueSCSI"` + your version string, append one API
byte, and set `buf[4]` accordingly.

Minimal valid example (`NAME = "BlueSCSI"`, L=8, API=0): 45-byte reply,
`buf[4] = 0x1F + 8 + 1 = 0x28`, `buf[36..43] = "BlueSCSI"`, `buf[44] = 0x00`.

## 2. Toolbox vendor commands the host uses

All are 10-byte CDBs. "Data-in" = device → host; "data-out" = host → device.

| Op | Name | CDB fields the host sets | Data the host expects |
|----|------|--------------------------|-----------------------|
| `0xD2` | COUNT_FILES | — | 1 byte: file count in `/shared` (≤100) |
| `0xD0` | LIST_FILES | — | `count` × 40-byte `ToolboxFileEntry` (data-in) |
| `0xD1` | GET_FILE | `[1]`=index, `[2..5]`=4096-byte block offset | up to 4096 bytes per call (data-in) |
| `0xD3` | SEND_FILE_PREP | — | host sends 33-byte filename (data-out) |
| `0xD4` | SEND_FILE_10 | `[1..2]`=byte count, `[3..5]`=block number | host sends `count` data bytes (data-out) |
| `0xD5` | SEND_FILE_END | — | none |
| `0xD6` | TOGGLE_DEBUG | `[1]`=0 set/`[2]`=val, `[1]`=1 get | get returns 1 byte (data-in) |
| `0xD7` | LIST_CDS | — | `count` × 40-byte `ToolboxFileEntry` (data-in) |
| `0xD8` | SET_NEXT_CD | `[1]`=image index | none |
| `0xD9` | LIST_DEVICES (metadata) | `[1]`=0 (subcmd) | 8 bytes of device types (data-in) |
| `0xDA` | COUNT_CDS | — | 1 byte: CD image count (≤100) |

### 2.1 `ToolboxFileEntry` (40 bytes, used by LIST_FILES / LIST_CDS)

```
byte 0      index       (file index in directory)
byte 1      type        (0 = file, 1 = directory)
byte 2..34  name        (32 bytes; host treats as NUL-terminated, truncates to 32)
byte 35..39 size        (40-bit big-endian unsigned byte count)
```

Host reads `count` from COUNT_FILES/COUNT_CDS first, then sizes its receive
buffer to `count * 40` for the LIST call. Return entries packed back-to-back.

### 2.2 GET_FILE (`0xD1`)

Host loops: `CDB[1]`=file index, `CDB[2..5]`=block offset counted in **4096-byte
blocks**, big-endian. Device returns up to 4096 bytes (data-in) per call. Host
stops when it has written `size` bytes (from the file's `ToolboxFileEntry`), so
the device just needs to serve the requested 4096-byte window.

### 2.3 SEND_FILE (PUT) — `0xD3` → `0xD4`… → `0xD5`

1. **PREP `0xD3`** — host sends a 33-byte (NUL-terminated, ≤32 chars) filename in
   data-out. Device creates/truncates that file in the shared dir. On failure,
   return CHECK CONDITION / ILLEGAL_REQUEST.
2. **SEND_FILE_10 `0xD4`** (repeated) — `CDB[1..2]` = number of bytes in this
   request (big-endian, 1..512 as the host sends it), `CDB[3..5]` = **block
   number** (0,1,2,…), big-endian. Host then sends that many data bytes
   (data-out). Place them at file offset `block_number * 512`.
   - **Emulator note:** seek **absolutely** to `block_number * 512` and write.
     Verified against local firmware `v2026.04.27-7-g61ddd31d`: `onSendFile10`
     does `gFile.seekCur(offset * 512)` (a *relative* seek), which corrupts
     multi-block writes — implement absolute seek to match the documented intent.
   - The host sends 512 bytes per block except the final (short) block.
3. **END `0xD5`** — close/flush the file.

### 2.4 LIST_DEVICES (`0xD9`) — required for detection

Host sends `D9 00 00 …` (`CDB[1]`=0 = LIST_DEVICES subcommand) and reads **8
bytes**, one per SCSI ID 0–7. Each byte is the device type:

```
0x00 HDD   0x01 removable   0x02 CD   0x03 floppy   0x04 MO   0x05 sequential
0xFF = target not enabled
```

The host stores these in `device_list[]` and uses them to gate CD operations
(`-l`, `-c`) — those require the target's byte to be `0x02` (CD). It is fetched
after acceptance and a failure is non-fatal, so it is no longer required for
detection, but you should still implement it for CD support.

## 2.5 Wi-Fi commands (`0x1C`) — a different CDB on a different target

Two things make this family unlike everything above, and getting either wrong is
the usual reason a first implementation sees nothing at all:

1. **The CDB is 6 bytes, not 10.** Every `0xD0`–`0xDA` command is a 10-byte CDB.
   These are not. Send a 10-byte CDB and the device does not answer.
2. **They are answered by the emulated NETWORK target**, not by the disk or CD.
   That is a different SCSI ID with its own device node. Worse, the firmware
   *deliberately* omits the toolbox INQUIRY tail for network targets
   (`inquiry.c`: the `INQUIRY_NAME` append is skipped for `S2S_CFG_NETWORK`), so
   the Wi-Fi node will never be detected as a toolbox device and the toolbox
   node will never answer `0x1C`. They are two separate devices that happen to
   live on the same board.

> **The upstream documentation is wrong about the subcommand byte.** It says the
> subcommand is in `CDB[2]`, and so does the comment directly above the dispatch
> in the firmware's own `network.c`. The **code** switches on `scsiDev.cdb[1]`
> and computes `size = (scsiDev.cdb[3] << 8) + scsiDev.cdb[4]`. Both working
> host implementations — joshua stein's Macintosh `wifi_da` and SonnyJim's
> `bswifi` — follow the code. So does this one.

### CDB layout (6 bytes)

```
byte 0    0x1C        SCSI_NETWORK_WIFI_CMD
byte 1    subcommand  0x01 SCAN, 0x02 COMPLETE, 0x03 SCAN_RESULTS,
                      0x04 INFO, 0x05 JOIN
byte 2    unused
byte 3..4 transfer length, big endian
byte 5    control
```

Note `0x1C` is **RECEIVE DIAGNOSTIC RESULTS** in standard SCSI — a read-only
command — so probing a non-Wi-Fi target with it is safe, but a plain disk may
well *answer* rather than reject it. Detection therefore has to check the shape
of the reply, not merely that one arrived (see below).

| Sub | Name | Length host sets | Data |
|-----|------|------------------|------|
| `0x01` | SCAN | 1 | 1 byte in: `1` = scan started, anything else = refused |
| `0x02` | COMPLETE | 1 | 1 byte in: `1` = finished, `0` = still scanning |
| `0x03` | SCAN_RESULTS | 2048 | 2-byte BE size, then that many bytes of entries (data-in) |
| `0x04` | INFO | 76 | 2-byte BE size (always 74), then one entry (data-in) |
| `0x05` | JOIN | 130 | host sends a 130-byte `wifi_join_request` (data-out) |

The 2-byte size prefix **excludes itself**. `SCAN_RESULTS` truncates to a whole
number of entries, so `size / 74` is always exact.

**`SCAN_RESULTS` before the scan finishes is an error, not an empty list.** The
firmware answers CHECK CONDITION (ILLEGAL_REQUEST / INVALID_FIELD_IN_CDB) if
asked early, so the host must poll `COMPLETE` first. A size of `0` from a
*finished* scan legitimately means "no networks found".

**`JOIN` validates the length field exactly.** `scsiNetworkWifiJoin()` compares
it against `sizeof(struct wifi_join_request)` and answers CHECK CONDITION on any
other value rather than coping — so `CDB[3..4]` must say exactly 130.

`JOIN` also reports only that the *request* was accepted; the firmware hands the
credentials to the radio and answers GOOD immediately. Whether the association
succeeded can only be learned by waiting and then issuing `INFO`.

### 2.5.1 `wifi_network_entry` (74 bytes, used by SCAN_RESULTS and INFO)

```
byte 0..63   ssid      (64 bytes, NUL-padded; may fill all 64 with no terminator)
byte 64..69  bssid     (6 raw bytes)
byte 70      rssi      (SIGNED 8-bit, dBm — sign-extend it)
byte 71      channel   (unsigned)
byte 72      flags     (bit 0 = WIFI_NETWORK_FLAG_AUTH, i.e. secured)
byte 73      padding
```

The firmware declares this `__attribute__((packed))`, which MIPSpro has no
equivalent of — so `wifi.c` decodes it byte by byte rather than through a C
struct that would be free to gain padding. `rssi` is the field that bites: read
as a plain `char` on a platform where that is unsigned, a normal −67 dBm arrives
as 189.

The firmware keeps ten slots (`WIFI_NETWORK_LIST_ENTRY_COUNT`), so a scan
returns at most 10 entries (740 bytes + the 2-byte prefix).

### 2.5.2 `wifi_join_request` (130 bytes, data-out for JOIN)

```
byte 0..63    ssid     (64 bytes including the terminator → 63 usable)
byte 64..127  key      (64 bytes including the terminator → 63 usable)
byte 128      channel  (0 = let the firmware choose)
byte 129      padding
```

### 2.5.3 How the host finds the Wi-Fi target

Same two-stage shape as toolbox detection, for the same reason — a claim alone
is never trusted:

1. **Claim** — the INQUIRY identity contains `SCSI/Link`. BlueSCSI's network
   personalities are `Dayna SCSI/Link 2.0f` and `AmigaNET SCSI/Link 1.0f`
   (`BlueSCSI_config.h`), so the shared product string covers both; both are
   INQUIRY peripheral device type `0x03` (processor).
2. **Confirm** — issue `INFO` (`0x1C`/`0x04`) and require the 2-byte prefix to
   read exactly **74**. The firmware always reports `sizeof(wifi_network_entry)`
   there regardless of whether the radio has joined anything, so it is a
   reliable signature — and one a genuine vintage Dayna SCSI/Link (which has the
   same INQUIRY identity and no radio at all) cannot produce.

`-F` skips stage 1 and tests every node directly, exactly as it does for the
toolbox. `irixscsitb -b` reports a confirmed Wi-Fi node as `[WIFI]`.

## 3. Minimum the emulator must implement to be useful

- **Acceptance:** present vendor `SGI` / product `IRIS EMUL DISK` in INQUIRY
  (§1a) **and/or** emit MODE SENSE page 0x31 with the `"BlueSCSI"` magic (§1b).
  Either one is sufficient; doing both is most robust.
- **`0xD9`** returning 8 device-type bytes (gates CD ops; non-fatal for detection).
- For `-s`/`-g` (read from shared): `0xD2` COUNT_FILES, `0xD0` LIST_FILES,
  `0xD1` GET_FILE.
- For `-p` (write to shared): `0xD3`/`0xD4`/`0xD5`.
- For `-l`/`-c` (CDs): `0xDA` COUNT_CDS, `0xD7` LIST_CDS, `0xD8` SET_NEXT_CD,
  and report `0x02` for that ID in `0xD9`.
- `0xD6` TOGGLE_DEBUG is only exercised with `-i -v` / `-d`; nice to have.

## 4. Transport notes (IRIX vs Linux)

The host issues these CDBs via `scsi_send_command` (data-in) and
`scsi_send_commandw` (data-out). On IRIX that's `DS_ENTER` ioctls on
`/dev/scsi/scNdMl0`; on Linux it's `SG_IO` on `/dev/sgN`. The emulator only
needs to honour the SCSI-level CDB/data semantics above — it does not care which
host transport is used. The host derives the target SCSI ID from the device path
(`path_to_devnum`) and indexes `device_list[]` with it, so present device types
on the IDs the emulator exposes.
