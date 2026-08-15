/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Toolbox protocol constants and shared types.
 *
 * Copyright (C) 2024-2025 SonnyJim
 * Copyright (C) 2026 Dani Sarfati
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "os.h"

#define SCSI_INQUIRY    0x12
#define SCSI_MODE_SENSE_6 0x1A
/* BlueSCSI advertises toolbox capability via MODE SENSE vendor page 0x31
 * (the "BlueSCSIVendorPage"). This is the authoritative signal used to detect
 * toolbox-capable targets - such as the IRIS emulator - that present a native
 * (non-BlueSCSI) INQUIRY identity. */
#define TOOLBOX_VENDOR_PAGE 0x31
#define TOOLBOX_COUNT_FILES    0xD2
#define TOOLBOX_LIST_FILES     0xD0
#define TOOLBOX_GET_FILE       0xD1
#define TOOLBOX_SEND_FILE_PREP 0xD3
#define TOOLBOX_SEND_FILE_10   0xD4
#define TOOLBOX_SEND_FILE_END  0xD5
#define TOOLBOX_TOGGLE_DEBUG   0xD6
#define TOOLBOX_LIST_CDS       0xD7
#define TOOLBOX_SET_NEXT_CD    0xD8
#define TOOLBOX_LIST_DEVICES   0xD9
#define TOOLBOX_COUNT_CDS      0xDA
#define OPEN_RETRO_SCSI_TOO_MANY_FILES 0x0001

/*
 * Toolbox Wi-Fi control, a SECOND command family living on a SECOND target.
 *
 * Everything above (0xD0-0xDA) is answered by the emulated disk/CD. The Wi-Fi
 * commands are not: they are answered only by the firmware's emulated NETWORK
 * target - the DaynaPort SCSI/Link - which is a different SCSI ID with its own
 * device node. Sending 0x1C to the disk gets you nothing. See wifi.c.
 *
 * Two things about the CDB are easy to get wrong, and both are worth stating
 * plainly because the upstream firmware documentation has them wrong:
 *
 *   - the CDB is SIX bytes, not the ten every other toolbox command uses;
 *   - the subcommand is in CDB[1] and the transfer length is a big-endian
 *     16-bit value in CDB[3..4].
 *
 * The firmware's own source is the authority here: network.c dispatches on
 * `scsiDev.cdb[1]` and computes `size = (scsiDev.cdb[3] << 8) + scsiDev.cdb[4]`,
 * even though the comment directly above that switch says cdb[2]. Both working
 * host implementations (jcs's Macintosh wifi_da and SonnyJim's bswifi) agree
 * with the code rather than the comment.
 *
 * And one thing about the opcode itself: 0x1C is NOT a free vendor code the
 * way 0xD0-0xDA are. In standard SCSI it is RECEIVE DIAGNOSTIC RESULTS, an
 * opcode a real disk or tape is entitled to implement; the firmware overloads
 * it on the network target only. It is read-only, so a stray probe cannot
 * corrupt anything, but it must still never be aimed at a device that is
 * plainly not a network target: toolbox_wifi_probe()'s INQUIRY-identity check
 * and peripheral-device-type floor are that SAFETY boundary, not a speed
 * optimisation - do not remove or weaken them. The floor holds even under -F.
 */
#define SCSI_NETWORK_WIFI_CMD  0x1C
#define WIFI_CMD_SCAN          0x01  /* start a scan            -> 1 byte in  */
#define WIFI_CMD_COMPLETE      0x02  /* has the scan finished?  -> 1 byte in  */
#define WIFI_CMD_SCAN_RESULTS  0x03  /* fetch the network list  -> N bytes in */
#define WIFI_CMD_INFO          0x04  /* the current network     -> 76 bytes in*/
#define WIFI_CMD_JOIN          0x05  /* associate               -> 130 out    */

/* Six, not SCSI_CMD_LENGTH. Getting this wrong is the classic failure here. */
#define WIFI_CDB_LENGTH 6

/*
 * Wire sizes. These are the firmware's packed structs (network.h):
 *
 *   wifi_network_entry {ssid[64]; bssid[6]; int8 rssi; u8 channel; u8 flags;
 *                       u8 _padding;}                              = 74 bytes
 *   wifi_join_request  {ssid[64]; key[64]; u8 channel; u8 _padding;} = 130 bytes
 *
 * They are decoded and encoded byte by byte rather than memcpy'd into a C
 * struct: __attribute__((packed)) is a GCC extension MIPSpro does not have, so
 * a struct declared here would be free to carry padding the wire format has no
 * room for.
 */
#define WIFI_ENTRY_SIZE      74
#define WIFI_JOIN_SIZE      130
#define WIFI_SSID_MAX        64      /* field width; 63 chars + NUL usable */
#define WIFI_KEY_MAX         64
#define WIFI_BSSID_LEN        6

/* Field offsets inside one wire entry. */
#define WIFI_OFF_SSID         0
#define WIFI_OFF_BSSID       64
#define WIFI_OFF_RSSI        70
#define WIFI_OFF_CHANNEL     71
#define WIFI_OFF_FLAGS       72

/* flags byte: the firmware defines exactly one bit. */
#define WIFI_FLAG_AUTH     0x01

/*
 * The firmware keeps ten scan slots (WIFI_NETWORK_LIST_ENTRY_COUNT) and both
 * SCAN_RESULTS and INFO prefix their reply with a big-endian 16-bit byte count
 * that does NOT include the two count bytes themselves.
 */
#define WIFI_MAX_NETWORKS    10
#define WIFI_LEN_PREFIX       2
/* What we ask SCAN_RESULTS for - the same 2048 the Macintosh DA requests. */
#define WIFI_SCAN_BUF_SIZE 2048

/*
 * A scan takes a few seconds of real radio time. Poll COMPLETE (0x02) once a
 * second up to this many times; SCAN_RESULTS returns CHECK CONDITION if it is
 * asked before the scan finishes, so waiting is not optional.
 */
#define WIFI_SCAN_TIMEOUT_SEC 20

#define TOOLBOX_API_VER 1

#define MAX_FILES 100
/* GET_FILE (0xD1) transfers the file in fixed blocks; the CDB offset field is
 * counted in MAX_DATA_LEN-byte blocks and each reply returns up to this many
 * bytes. Must match the firmware's toolbox block size (4096). */
#define MAX_DATA_LEN 4096
#define SEND_BUF_SIZE 512
#define NAME_BUF_SIZE 33
#define NOT_ACTIVE -1
#define SCSI_CMD_LENGTH 10 /*Almost all of the SCSI commands we send are 10 big */
/*Copied from scsi2sd.h */
/*
 * Device-type codes returned by TOOLBOX_LIST_DEVICES (0xD9), one per SCSI ID.
 * Kept in step with the BlueSCSI/ZuluSCSI firmware list (see
 * escsitoolbox include/toolbox.h ToolboxDeviceType) - NETWORK and ZIP100 are
 * emitted by current firmware and must not be reported as "unknown".
 */
typedef enum
{
	TYPE_NONE = 0xFF,
	TYPE_HDD = 0x00,
	TYPE_REMOVABLE = 0x01,
	TYPE_CD = 0x02,
	TYPE_FLOPPY = 0x03,
	TYPE_MO = 0x04,
	TYPE_SEQUENTIAL = 0x05,
	TYPE_NETWORK = 0x06,
	TYPE_ZIP100 = 0x07
} dev_type;

/* Highest valid device-type code; anything above it (bar TYPE_NONE) is junk. */
#define TOOLBOX_DEVTYPE_MAX 0x07

/*
 * Device-type map from the last successful TOOLBOX_LIST_DEVICES (0xD9), used to
 * gate CD operations. Defined in toolbox.c.
 */
extern int device_list[8];

/* Upper bound on device nodes reported by a bus scan (-b). */
#define MAX_SCAN_DEVICES 64

enum {
	MODE_NONE,
	MODE_CD,
	MODE_SHARED,
	MODE_PUT,
	MODE_INQUIRY,
	MODE_DEBUG,
	MODE_DEBUG_GET,
	MODE_DEVICES,
	MODE_SCAN,
	MODE_WIFI_SCAN,
	MODE_WIFI_INFO,
	MODE_WIFI_JOIN
};

enum {
	PRINT_OFF,
	PRINT_ON
};



enum {
	DEBUG_SET,
	DEBUG_GET
};

/* Defined in toolbox.c; set from the CLI (-v) or the GUI. */
extern int verbose;

/*
 * -F: skip the INQUIRY/page-0x31 identity check and test the device by issuing
 * a real toolbox command instead. For firmware whose name we don't know yet.
 * Defined in toolbox.c.
 */
extern int force_toolbox;

typedef struct {
	unsigned char dev_type; /* Peripheral device type (bits 4-7), Peripheral qualifier (bits 0-3) */
	unsigned char dev_type_mod;    /*RMB (bit 7), Device-type modifier (bits 0-6) */
	unsigned char version; /*SCSI version ID */
	unsigned char add_length; /*Additional length in bytes */
	char reserved[3]; 
	char vendor_id[9];
	char product_id[17];
	char product_rev[33];
	
} scsi_inquiry;

typedef struct {
    unsigned char index;   /* byte 00: file index in directory */
    unsigned char type;    /* byte 01: type 0 = file, 1 = directory */
    char name[NAME_BUF_SIZE];         /* byte 02-34: filename (32 byte max) + space for NUL terminator */
    unsigned char size[5]; /* byte 35-39: file size (40 bit big endian unsigned) */
} ToolboxFileEntry;

/* Longest "<vendor> <product> <rev>" identity string we build from INQUIRY. */
#define TOOLBOX_IDENTITY_MAX 64

/* Shown by the CLI's -version and the GUI's Help > About. */
#define PROJECT_URL "https://github.com/danifunker/irixscsitb"

/*
 * Build identification (version.c). The generated version.h is included only by
 * version.c, never here - putting it in this header would rebuild the whole
 * tree every time the revision or timestamp changed.
 */
const char *build_revision(void);   /* git short rev, "-dirty" if modified */
const char *build_stamp(void);      /* when the source was stamped (host) */
const char *build_compiled(void);   /* when this object was compiled */
const char *build_abi(void);        /* "o32 (mips2) - runs on IRIX 5.3 ..." */
const char *build_libs(void);       /* OS + release the binary was linked on */

/*
 * Everything the two-stage detection learned about one target. Filled by
 * toolbox_detect(); the CLI turns it into stdout text and the GUI turns it into
 * widget state, so the detection itself never prints a result.
 */
typedef struct {
	scsi_inquiry inq;
	char identity[TOOLBOX_IDENTITY_MAX]; /* trimmed "<vendor> <product> <rev>" */
	const char *claim_id;       /* firmware name that matched, or NULL */
	int claimed_via_page31;     /* claim came from MODE SENSE page 0x31 */
	int claims;                 /* stage 1 passed: device advertises toolbox */
	int confirmed;              /* stage 2 passed: real 0xD9 answer */
	int api_version;            /* Toolbox API version, or -1 if unavailable */
	unsigned char devmap[8];    /* 0xD9 device-type map; valid when confirmed */
} ToolboxDetect;

/* One row of a bus scan (-b): what a single device node reported. */
typedef struct {
	char path[SCSI_PATH_MAX];
	char identity[TOOLBOX_IDENTITY_MAX];
	const char *type_name;      /* INQUIRY peripheral device type, printable */
	int claims;
	int confirmed;
	int wifi;                   /* answered the Wi-Fi INFO command (0x1C/0x04) */
} ToolboxScanEntry;

/*
 * Room for the markers a scan row can carry. Current firmware answers the
 * toolbox and the Wi-Fi commands on separate targets, so in practice a row gets
 * at most one - but the front ends render whatever a device actually claims
 * rather than relying on that, so this is sized for all of them at once.
 */
#define SCAN_TAG_MAX 64

/*
 * One Wi-Fi network, decoded from the 74-byte wire entry. Held in host types
 * rather than as the raw struct so front ends never have to know the layout:
 * rssi is signed on the wire and must not arrive here as 200-odd.
 */
typedef struct {
	char ssid[WIFI_SSID_MAX + 1];       /* always NUL-terminated */
	unsigned char bssid[WIFI_BSSID_LEN];
	int rssi;                           /* dBm, negative */
	int channel;
	unsigned char flags;                /* WIFI_FLAG_AUTH */
} ToolboxWifiNetwork;

/*
 * Core toolbox API (toolbox.c). Everything here returns data or a status and
 * prints only errors/verbose diagnostics to stderr - never a result - so the
 * CLI and the Motif GUI can share it. Presentation lives in the front ends.
 */
long int size_to_long(const unsigned char size[5]);
const char *dev_type_name(int t);
const char *inquiry_pdt_name(unsigned char b0);

int toolbox_getdebug(int dev);
int toolbox_setdebug(int dev, int value);
int toolbox_countfiles(int dev);
int toolbox_countcds(int dev);
int toolbox_setnextcd(int dev, int num);
int toolbox_sendfile(int dev, char *path);
int toolbox_getfile(int dev, int idx, char *outdir);

/*
 * Fill entries[] with at most max file/CD records from the target. Return the
 * number written, or -1 on failure.
 */
int toolbox_listfiles(int dev, ToolboxFileEntry *entries, int max);
int toolbox_listcds(int dev, ToolboxFileEntry *entries, int max);

/*
 * Outcomes of toolbox_prepare_cd_swap(). The first two mean "go ahead"; the
 * third means the swap must NOT happen unless the operator overrides.
 */
#define CDSWAP_NOT_MOUNTED  0   /* nothing was mounted */
#define CDSWAP_UNMOUNTED    1   /* it was mounted; we unmounted it */
#define CDSWAP_BUSY         2   /* still mounted and in use - do not swap */

/*
 * Get the host out of the way before a CD image is switched: find whether the
 * target's volume is mounted and, if so, unmount it. mnt receives the mount
 * point (required, non-NULL); why receives the reason on CDSWAP_BUSY.
 */
int toolbox_prepare_cd_swap(const char *path, char *mnt, int mntlen,
			    char *why, int whylen);

/* Raw 8-byte device-type map (0xD9) into map[]. Returns 0 on success. */
int toolbox_listdevices(int dev, unsigned char map[8]);

/*
 * toolbox_detect() outcomes. Distinguishing them is what lets each front end
 * explain a rejection instead of just failing: "no answer at all" and "answered
 * INQUIRY but never implemented 0xD9" need very different advice.
 */
#define TOOLBOX_OK             0
#define TOOLBOX_ERR_INQUIRY   (-1)  /* INQUIRY itself failed */
#define TOOLBOX_ERR_NO_CLAIM  (-2)  /* device does not advertise the toolbox */
#define TOOLBOX_ERR_NO_ANSWER (-3)  /* claimed toolbox, but no valid 0xD9 reply */

/*
 * Two-stage detection, available whole or in halves.
 *
 * toolbox_identify() does the cheap half: INQUIRY, the trimmed identity string
 * and the Toolbox API version. toolbox_qualify() does the rest - the firmware
 * claim (INQUIRY name or MODE SENSE page 0x31) and the functional 0xD9
 * confirmation - and is the only one that sends vendor opcodes. Splitting them
 * lets a front end show who the device says it is before paying for the slower
 * qualification; toolbox_detect() just runs both.
 *
 * All three fill *out regardless of outcome, so a caller can report *why* a
 * device was rejected, and return TOOLBOX_OK only when out->confirmed is set.
 * On success device_list[] is updated too.
 */
int toolbox_identify(int dev, ToolboxDetect *out);
int toolbox_qualify(int dev, ToolboxDetect *out);
int toolbox_detect(int dev, ToolboxDetect *out);

/*
 * Probe one generic SCSI node (open read-only, INQUIRY, claim, confirm) and
 * fill *out. Returns 0 if it answered, -1 if it could not be opened or stayed
 * silent. A bus scan is scsi_enum_devices() plus this in a loop - kept split so
 * a front end can show each result as it arrives instead of only at the end.
 */
int toolbox_probe(const char *path, ToolboxScanEntry *out);

/*
 * Wi-Fi (wifi.c). Same rules as the rest of the core: these return data or a
 * status and never print a result, so the CLI and the GUI can share them.
 *
 * Every one of these must be given a device node for the emulated NETWORK
 * target, NOT for the disk the same BlueSCSI/ZuluSCSI is emulating. The disk
 * does not implement 0x1C. toolbox_wifi_find() below is how you get that path
 * without making the operator work it out.
 */

/*
 * Is this open node the Wi-Fi target? Two stages, exactly like the toolbox:
 * the device has to claim it (INQUIRY product "SCSI/Link", covering both the
 * "Dayna" DaynaPort and "AmigaNET" AmigaWIFI personalities) and then answer
 * WIFI_CMD_INFO with a well-formed 76-byte reply. -F (force_toolbox) skips the
 * claim and tests every node directly, for firmware we don't know by name.
 *
 * SAFETY: 0x1C is standard RECEIVE DIAGNOSTIC RESULTS (see the note at
 * SCSI_NETWORK_WIFI_CMD), so nothing reaches the confirm step without passing
 * the INQUIRY gates here. Besides the identity claim there is a peripheral-
 * device-type floor that refuses clearly-non-network types (disk, tape,
 * CD-ROM, ...) - and that floor, unlike the identity check, is NOT waived by
 * -F: forcing exists for an unrecognised BlueSCSI, not for aiming a
 * diagnostic command at a disk. A forced probe of a device that passes the
 * floor but is not positively identified warns on stderr before it sends.
 *
 * identity is the trimmed INQUIRY string if the caller already has one (a bus
 * scan does), or NULL to have this fetch it; pdt is the INQUIRY peripheral-
 * device-type byte (raw byte 0 is fine, it is masked), or -1 to have this
 * fetch it - passing what you have saves the node a second INQUIRY. With probe
 * non-zero the commands are sent quietly and without retries, which is what a
 * bus scan wants. Returns 1 for yes, 0 for no.
 */
int toolbox_wifi_probe(int dev, const char *identity, int pdt, int probe);

/*
 * Walk the host's generic SCSI nodes and return the first one that answers as
 * a Wi-Fi target. Fills path (required) and returns 0, or returns -1 if the bus
 * has none. This is what makes the Wi-Fi options usable without a device path:
 * the network target is a different SCSI ID from the disk, and expecting the
 * operator to know which is asking them to read the firmware's config file.
 */
int toolbox_wifi_find(char *path, int pathlen);

/*
 * WIFI_CMD_SCAN (0x01): ask the radio to start scanning. Returns 0 if the scan
 * was accepted, -1 if the command failed or the firmware refused.
 */
int toolbox_wifi_scan_start(int dev);

/*
 * WIFI_CMD_COMPLETE (0x02): 1 if the scan has finished, 0 if it is still
 * running, -1 if the command failed.
 */
int toolbox_wifi_scan_done(int dev);

/*
 * Start a scan and block until it finishes (or timeout_sec elapses). Asking for
 * results early earns a CHECK CONDITION, so this is the sequence every caller
 * actually wants. Returns 0 when the scan completed, -1 on failure or timeout.
 */
int toolbox_wifi_scan(int dev, int timeout_sec);

/*
 * WIFI_CMD_SCAN_RESULTS (0x03): fill nets[] with at most max networks from the
 * last completed scan. Returns how many were written, or -1 on failure.
 */
int toolbox_wifi_results(int dev, ToolboxWifiNetwork *nets, int max);

/*
 * WIFI_CMD_INFO (0x04): the network currently joined. Returns 0 on success and
 * fills *out; an out->ssid of "" means the radio is not associated.
 */
int toolbox_wifi_info(int dev, ToolboxWifiNetwork *out);

/*
 * WIFI_CMD_JOIN (0x05): associate with ssid using key (pass "" or NULL for an
 * open network). channel 0 means "let the firmware pick". This is the one
 * Wi-Fi command that writes, so dev must have been opened read/write.
 * Returns 0 if the request was accepted by the firmware.
 *
 * NOTE the firmware only ACCEPTS the request here - it does not report whether
 * the association succeeded. Confirm with toolbox_wifi_info() afterwards.
 */
int toolbox_wifi_join(int dev, const char *ssid, const char *key, int channel);

/* "WPA/WEP" or "open", from the flags byte. */
const char *wifi_auth_name(unsigned char flags);

/* Format a BSSID as aa:bb:cc:dd:ee:ff. out must hold at least 18 bytes. */
void wifi_bssid_str(const unsigned char *bssid, char *out, int outlen);

/*
 * Signal strength as a 0-4 bar count, for a front end that wants to show one.
 * The thresholds are the usual dBm breakpoints (-55/-65/-75/-85).
 */
int wifi_signal_bars(int rssi);
