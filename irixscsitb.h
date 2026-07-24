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

int device_list[8];

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

int verbose;

/*
 * -F: skip the INQUIRY/page-0x31 identity check and test the device by issuing
 * a real toolbox command instead. For firmware whose name we don't know yet.
 */
int force_toolbox;

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

ToolboxFileEntry files[MAX_FILES];
int files_count;
/*char device_path[256]; */
