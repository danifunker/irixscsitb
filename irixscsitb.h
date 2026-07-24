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
	MODE_SCAN
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
} ToolboxScanEntry;

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
