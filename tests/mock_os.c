/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Mock OS backend: a fake SCSI bus that lets irixscsitb.c's protocol and
 * detection logic be exercised on a development machine that has neither IRIX
 * (<sys/dsreq.h>) nor Linux (<scsi/sg.h>) SCSI headers - e.g. macOS.
 *
 * It implements the os.h contract with a synthetic 10-device bus covering the
 * cases that matter for detection:
 *
 *   d0  plain SGI disk        - no toolbox
 *   d1  IRIS EMUL DISK        - emulated disk, implements NO toolbox commands
 *   d2  Sony CD-ROM           - no toolbox
 *   d3  BlueSCSI              - real toolbox (name at INQUIRY byte 36)
 *   d4  dead node             - never answers
 *   d5  ZuluSCSI              - real toolbox (name at INQUIRY byte 36)
 *   d6  liar                  - serves page 0x31 but does NOT implement 0xD9
 *   d7  Dayna SCSI/Link       - the emulated network target: implements the
 *                               Wi-Fi commands (0x1C) but NOT the toolbox
 *   d8  real Dayna SCSI/Link  - the vintage card: same INQUIRY identity, no
 *                               radio, so it claims Wi-Fi and fails to answer
 *   d9  ZuluSCSI, toolbox OFF - a stock ZuluSCSI: zuluscsi.ini defaults to
 *                               EnableToolbox = 0, yet INQUIRY still carries
 *                               the name, so it claims and then fails 0xD9
 *
 * Expected result of `make test`: d3 and d5 are marked [TOOLBOX]; d1 is NOT
 * (it must never be accepted on product name alone); d6 and d9 are reported
 * as "claims toolbox, no 0xD9 answer", and d9 makes the scan print the
 * EnableToolbox advice; d4 is skipped; d7 is marked [WIFI] and d8 is not -
 * the Wi-Fi check has to be functional too, exactly like the toolbox one,
 * because the emulated and the genuine SCSI/Link are indistinguishable by
 * name.
 *
 * The mock also enforces the host's 0x1C safety contract: 0x1C is standard
 * RECEIVE DIAGNOSTIC RESULTS, and the host must never emit it to a
 * storage-type target, -F included. Any 0x1C arriving at a node whose INQUIRY
 * peripheral device type is not 0x03 aborts the test (see
 * mock_wifi_floor_check).
 *
 * Listings and transfers: the toolbox nodes serve a /shared directory of two
 * files, one directory and one 2 GiB file (see mock_shared[]) and a CD list whose second
 * image is 4,433,547,264 bytes - a size that needs the top byte of the 40-bit
 * field, so a host that folds it into a 32-bit long prints it negative. Entry
 * byte 1 is written the way the firmware writes it, 0x01 for a file and 0x00
 * for a directory. GET_FILE serves a byte pattern (offset & 0xFF) and, like
 * the Wi-Fi floor, treats a request for a block past the end of the file as
 * fatal: the host knows the exact size, so it has no business reading there.
 * The ZuluSCSI's CD target (d5) reports a mounted, busy /CDROM, which is what
 * lets `make test` check that a refused -c exits non-zero and -f goes through.
 *
 * Test scaffolding only - never built into the shipped tool.
 *
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
#include "os.h"

extern int verbose;

#define MOCK_N 10
static const char *mock_paths[MOCK_N] = {
	"/dev/mock/sc0d0l0",   /* plain SGI disk    - no toolbox at all      */
	"/dev/mock/sc0d1l0",   /* IRIS EMUL DISK    - emulated disk, NO tbox */
	"/dev/mock/sc0d2l0",   /* Sony CD-ROM       - no toolbox             */
	"/dev/mock/sc0d3l0",   /* BlueSCSI          - real toolbox           */
	"/dev/mock/sc0d4l0",   /* dead node         - never answers          */
	"/dev/mock/sc0d5l0",   /* ZuluSCSI          - real toolbox           */
	"/dev/mock/sc0d6l0",   /* liar: page 0x31 but no 0xD9 implementation */
	"/dev/mock/sc0d7l0",   /* emulated DaynaPort: Wi-Fi, no toolbox      */
	"/dev/mock/sc0d8l0",   /* genuine DaynaPort: same name, no radio     */
	"/dev/mock/sc0d9l0"    /* ZuluSCSI with EnableToolbox = 0 (default)  */
};

/* Does the device serve MODE SENSE page 0x31 with the toolbox magic? */
static int mock_page31[MOCK_N] = { 0, 0, 0, 1, 0, 0, 1, 0, 0, 0 };
/* Does the device actually IMPLEMENT 0xD9 LIST_DEVICES? */
static int mock_d9[MOCK_N]     = { 0, 0, 0, 1, 0, 1, 0, 0, 0, 0 };
/* Does the device actually IMPLEMENT the 0x1C Wi-Fi commands? */
static int mock_wifi[MOCK_N]   = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 };

/*
 * INQUIRY peripheral device type per node, mirroring fill_inq() below. The
 * 0x1C branches assert against it: 0x1C is standard RECEIVE DIAGNOSTIC
 * RESULTS, and the host's safety contract (toolbox_wifi_probe's device-type
 * floor, which holds even under -F) is that it never reaches a storage-type
 * target. A mock disk or CD seeing 0x1C is therefore a regression in the
 * host, not a rejection by the device, and the mock dies loudly so `make
 * test` fails.
 */
static const unsigned char mock_pdt[MOCK_N] = {
	0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00
};

static void mock_wifi_floor_check(int idx)
{
	if (mock_pdt[idx] != 0x03) {
		fprintf(stderr, "MOCK: FATAL: 0x1C sent to %s (PDT 0x%02x) - "
				"the Wi-Fi device-type floor is gone\n",
			mock_paths[idx], mock_pdt[idx]);
		exit(1);
	}
}

int mediad_start(void) { return 0; }
int mediad_stop(void)  { return 0; }

int scsi_open(char *path, int readonly)
{
	int i;
	(void)readonly;
	for (i = 0; i < MOCK_N; i++)
		if (strcmp(path, mock_paths[i]) == 0)
			return 100 + i;
	return -1;
}

int scsi_close(int dev) { (void)dev; return 0; }

int scsi_enum_devices(char paths[][SCSI_PATH_MAX], int max)
{
	int i, n = 0;
	for (i = 0; i < MOCK_N && n < max; i++) {
		strcpy(paths[n], mock_paths[i]);
		n++;
	}
	return n;
}

int path_to_devnum(const char *path)
{
	int t;
	if (sscanf(path, "/dev/mock/sc%*dd%dl%*d", &t) != 1)
		return -1;
	return t;
}

static void fill_inq(unsigned char *buf, int len, unsigned char pdt,
		     const char *v, const char *p, const char *r, const char *tail)
{
	memset(buf, ' ', len);
	buf[0] = pdt; buf[1] = 0; buf[2] = 2; buf[3] = 2;
	buf[4] = 0x1f; buf[5] = 0; buf[6] = 0; buf[7] = 0x18;
	memcpy(buf + 8,  v, strlen(v));
	memcpy(buf + 16, p, strlen(p));
	memcpy(buf + 32, r, strlen(r));
	if (tail != NULL && len > 36 + (int)strlen(tail)) {
		memcpy(buf + 36, tail, strlen(tail));
		buf[4] = (unsigned char)(0x1f + strlen(tail) + 1);
	}
}

/*
 * What the toolbox nodes list. Byte 1 of a wire entry is 0x01 for a FILE and
 * 0x00 for a DIRECTORY - the firmware computes it as
 * "isDirectory() ? 0x00 : 0x01" - and LIST_CDS never returns a directory.
 * Sizes are the 40-bit big-endian field exactly as the firmware packs it.
 */
typedef struct {
	unsigned char type;
	const char *name;
	unsigned char size[5];
} mock_entry;

static const mock_entry mock_shared[] = {
	{ 0x01, "Doom.iso",  { 0x00, 0x00, 0x00, 0x10, 0x00 } },  /* 4096: exactly one block */
	{ 0x01, "notes.txt", { 0x00, 0x00, 0x00, 0x13, 0x88 } },  /* 5000: one block + 904   */
	{ 0x00, "games",     { 0x00, 0x00, 0x00, 0x00, 0x00 } },  /* a directory             */
	{ 0x01, "big.img",   { 0x00, 0x80, 0x00, 0x00, 0x00 } }   /* 2^31: the first size a
								 * 32-bit off_t cannot
								 * hold - must be refused
								 * up front, never read */
};
static const mock_entry mock_cds[] = {
	{ 0x01, "MIPSpro_dev7_4_full.iso", { 0x00, 0x2B, 0xC0, 0x00, 0x00 } }, /*   734,003,200 */
	{ 0x01, "irix-6-5-22-full.iso",    { 0x01, 0x08, 0x42, 0x90, 0x00 } }  /* 4,433,547,264 */
};
#define MOCK_N_SHARED (int)(sizeof(mock_shared) / sizeof(mock_shared[0]))
#define MOCK_N_CDS    (int)(sizeof(mock_cds) / sizeof(mock_cds[0]))

/* Pack entries the way onListFiles() does: 40 bytes each, back to back. */
static void mock_fill_entries(unsigned char *buf, int buf_len, const mock_entry *e, int n)
{
	int i;

	memset(buf, 0, buf_len);
	for (i = 0; i < n && (i + 1) * 40 <= buf_len; i++) {
		unsigned char *p = buf + i * 40;

		p[0] = (unsigned char)i;
		p[1] = e[i].type;
		strncpy((char *)p + 2, e[i].name, 32);
		memcpy(p + 35, e[i].size, 5);
	}
}

/* Low 32 bits of a size: enough for the /shared files, which are small. */
static unsigned long mock_size32(const unsigned char size[5])
{
	return ((unsigned long)size[1] << 24) | ((unsigned long)size[2] << 16) |
	       ((unsigned long)size[3] << 8) | (unsigned long)size[4];
}

static int mock_command(int dev, unsigned char *cmd, int cmd_len,
			unsigned char *buf, int buf_len)
{
	int idx = dev - 100;
	(void)cmd_len;

	if (idx < 0 || idx >= MOCK_N)
		return 1;
	if (idx == 4)
		return 1;                      /* dead target: never answers */

	if (cmd[0] == 0x12) {                  /* INQUIRY */
		if (buf_len < 66) return 1;
		switch (idx) {
		case 0: fill_inq(buf, buf_len, 0x00, "SGI     ", "IBM DORS-32160  ", "1.0 ", NULL); break;
		case 1: fill_inq(buf, buf_len, 0x00, "SGI     ", "IRIS EMUL DISK  ", "1.0 ", NULL); break;
		case 2: fill_inq(buf, buf_len, 0x05, "SONY    ", "CD-ROM CDU-76S  ", "1.0 ", NULL); break;
		case 3: fill_inq(buf, buf_len, 0x00, "QUANTUM ", "BlueSCSI        ", "1.0 ", "BlueSCSI Picov2026.04.28"); break;
		case 5: fill_inq(buf, buf_len, 0x00, "QUANTUM ", "ZuluSCSI        ", "1.0 ", "ZuluSCSI v2024.05.17"); break;
		case 6: fill_inq(buf, buf_len, 0x00, "ACME    ", "MYSTERY BOX     ", "1.0 ", NULL); break;
		/* Both SCSI/Link nodes are processor devices (PDT 0x03) with
		 * byte-identical INQUIRY data - which is the point: only the
		 * 0x1C answer below tells them apart. */
		case 7: fill_inq(buf, buf_len, 0x03, "Dayna   ", "SCSI/Link       ", "2.0f", NULL); break;
		case 8: fill_inq(buf, buf_len, 0x03, "Dayna   ", "SCSI/Link       ", "2.0f", NULL); break;
		/* Byte-identical to d5: the firmware appends its name to INQUIRY
		 * whether or not zuluscsi.ini enables the toolbox, so only the
		 * 0xD9 answer (refused below via mock_d9) tells the two apart. */
		case 9: fill_inq(buf, buf_len, 0x00, "QUANTUM ", "ZuluSCSI        ", "1.0 ", "ZuluSCSI v2024.05.17"); break;
		}
		return 0;
	}

	if (cmd[0] == 0x1A && cmd[2] == 0x31) { /* MODE SENSE page 0x31 */
		if (!mock_page31[idx])
			return 1;
		memset(buf, 0, buf_len);
		buf[0] = 0x31; buf[1] = 42;
		strcpy((char *)buf + 2, "BlueSCSI is the BEST STOLEN FROM BLUESCSI");
		return 0;
	}

	if (cmd[0] == 0xD9) {                   /* LIST_DEVICES map */
		if (!mock_d9[idx]) return 1;    /* device does not implement it */
		if (buf_len < 8) return 1;
		memset(buf, 0xFF, 8);
		buf[0] = 0x00;  /* HDD */
		buf[1] = 0x02;  /* CD */
		buf[2] = 0x02;  /* CD  */
		buf[3] = 0x00;  /* HDD: the BlueSCSI node itself (d3) */
		buf[5] = 0x02;  /* CD:  the ZuluSCSI node itself (d5) - so -l / -c
				 * addressed to it pass the CD gate */
		return 0;
	}

	/*
	 * Toolbox Wi-Fi, 0x1C with the subcommand in cmd[1] and a big-endian
	 * length in cmd[3..4] - a SIX-byte CDB, which is exactly the detail this
	 * mock exists to keep honest. Only d7 answers; d8 has the same INQUIRY
	 * identity and no radio, so it must fall through to the failure return
	 * at the bottom and be rejected.
	 */
	if (cmd[0] == 0x1C) {
		int want = ((int)cmd[3] << 8) | (int)cmd[4];
		int i;

		mock_wifi_floor_check(idx);
		if (!mock_wifi[idx])
			return 1;
		if (cmd_len != 6)
			return 1;              /* the ten-byte CDB mistake */
		if (buf_len < want)
			want = buf_len;
		memset(buf, 0, buf_len);

		switch (cmd[1]) {
		case 0x01:                     /* SCAN: started */
		case 0x02:                     /* COMPLETE: instantly, for the test */
			if (want < 1) return 1;
			buf[0] = 1;
			return 0;

		case 0x03: {                   /* SCAN_RESULTS: three networks */
			static const char *ssids[3] = { "Indigo Magic", "4Dwm", "open-guest" };
			static const int rssi[3]    = { -42, -71, -88 };
			static const int chan[3]    = { 6, 11, 1 };
			static const int auth[3]    = { 1, 1, 0 };
			int n = 3;
			int size = n * 74;

			if (want < 2 + size) return 1;
			buf[0] = (unsigned char)((size >> 8) & 0xFF);
			buf[1] = (unsigned char)(size & 0xFF);
			for (i = 0; i < n; i++) {
				unsigned char *e = buf + 2 + (i * 74);
				strcpy((char *)e, ssids[i]);
				e[64] = 0xDE; e[65] = 0xAD; e[66] = 0xBE;
				e[67] = 0xEF; e[68] = 0x00; e[69] = (unsigned char)i;
				e[70] = (unsigned char)(rssi[i] & 0xFF);  /* int8 */
				e[71] = (unsigned char)chan[i];
				e[72] = (unsigned char)auth[i];
			}
			return 0;
		}

		case 0x04:                     /* INFO: joined to the first one */
			if (want < 2 + 74) return 1;
			buf[0] = 0x00;
			buf[1] = 74;           /* always sizeof(wifi_network_entry) */
			strcpy((char *)buf + 2, "Indigo Magic");
			buf[2 + 64] = 0xDE; buf[2 + 65] = 0xAD; buf[2 + 66] = 0xBE;
			buf[2 + 67] = 0xEF; buf[2 + 68] = 0x00; buf[2 + 69] = 0x00;
			buf[2 + 70] = (unsigned char)(-42 & 0xFF);
			buf[2 + 71] = 6;
			buf[2 + 72] = 0x01;
			return 0;

		default:
			return 1;
		}
	}

	if (cmd[0] == 0xD6) {                   /* TOGGLE_DEBUG */
		if (cmd[1] == 1 && buf_len >= 1) buf[0] = 1;
		return 0;
	}

	if (cmd[0] == 0xDA) {                   /* COUNT_CDS */
		if (buf_len >= 1) buf[0] = (unsigned char)MOCK_N_CDS;
		return 0;
	}
	if (cmd[0] == 0xD2) {                   /* COUNT_FILES */
		if (buf_len >= 1) buf[0] = (unsigned char)MOCK_N_SHARED;
		return 0;
	}
	if (cmd[0] == 0xD7) {                   /* LIST_CDS: files only */
		mock_fill_entries(buf, buf_len, mock_cds, MOCK_N_CDS);
		return 0;
	}
	if (cmd[0] == 0xD0) {                   /* LIST_FILES */
		mock_fill_entries(buf, buf_len, mock_shared, MOCK_N_SHARED);
		return 0;
	}

	if (cmd[0] == 0xD8)                     /* SET_NEXT_CD, no data phase */
		return cmd[1] < MOCK_N_CDS ? 0 : 1;

	if (cmd[0] == 0xD1) {                   /* GET_FILE: one 4096-byte window */
		int fidx = cmd[1];
		unsigned long blk = ((unsigned long)cmd[2] << 24) | ((unsigned long)cmd[3] << 16) |
				    ((unsigned long)cmd[4] << 8) | (unsigned long)cmd[5];
		unsigned long fsize;
		unsigned long off;
		int i;

		if (!mock_d9[idx])
			return 1;
		if (fidx < 0 || fidx >= MOCK_N_SHARED || mock_shared[fidx].type == 0x00)
			return 1;                  /* no such file, or a directory */
		fsize = mock_size32(mock_shared[fidx].size);
		off = blk * 4096UL;
		if (off >= fsize) {
			fprintf(stderr, "MOCK: FATAL: GET_FILE asked for block %lu of %s, "
					"which is past its end (%lu bytes) - the host "
					"read beyond the size it was given\n",
				blk, mock_shared[fidx].name, fsize);
			exit(1);
		}
		memset(buf, 0, buf_len);
		for (i = 0; i < buf_len && off + (unsigned long)i < fsize; i++)
			buf[i] = (unsigned char)((off + (unsigned long)i) & 0xFF);
		return 0;
	}

	return 1;
}

int scsi_send_command(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	return mock_command(dev, cmd, cmd_len, buf, buf_len);
}

int scsi_send_command_probe(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	return mock_command(dev, cmd, cmd_len, buf, buf_len);
}

/*
 * Data-out. The toolbox send-file sequence is accepted unconditionally (there
 * is no SD card here to write to), but the Wi-Fi JOIN is checked: the firmware
 * refuses it outright unless the CDB is six bytes and the length field says
 * exactly sizeof(struct wifi_join_request), so the mock refuses it too rather
 * than letting a malformed request look like a success.
 */
int scsi_send_commandw(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	int idx = dev - 100;

	(void)buf;

	if (cmd[0] == 0x1C) {
		if (idx < 0 || idx >= MOCK_N)
			return 1;
		mock_wifi_floor_check(idx);
		if (!mock_wifi[idx])
			return 1;
		if (cmd_len != 6)
			return 1;
		if ((((int)cmd[3] << 8) | (int)cmd[4]) != 130 || buf_len != 130)
			return 1;
	}
	return 0;
}

/*
 * Mount handling. The mock bus has no filesystems, but the CD-swap guard needs
 * both of its outcomes exercised: every node reports nothing mounted (a clear
 * swap) except the ZuluSCSI CD target d5, whose /CDROM is mounted and cannot be
 * unmounted because something is sitting in it - the case a refused -c must
 * turn into a non-zero exit status, and -f must override.
 */
static void mock_copy(char *out, int outlen, const char *src)
{
	if (out == NULL || outlen <= 0)
		return;
	strncpy(out, src, outlen - 1);
	out[outlen - 1] = '\0';
}

int media_find_mount(const char *path, char *mnt, int mntlen, char *dev, int devlen)
{
	int busy = strstr(path, "sc0d5l0") != NULL;

	mock_copy(mnt, mntlen, busy ? "/CDROM" : "");
	mock_copy(dev, devlen, busy ? "dks0d5s7" : "");
	return busy ? 1 : 0;
}

int media_unmount(const char *mnt, char *why, int whylen)
{
	if (strcmp(mnt, "/CDROM") == 0) {
		mock_copy(why, whylen, "  /CDROM:   4242c(csh)\n");
		return -1;
	}
	mock_copy(why, whylen, "");
	return 0;
}
