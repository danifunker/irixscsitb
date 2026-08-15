/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Mock OS backend: a fake SCSI bus that lets irixscsitb.c's protocol and
 * detection logic be exercised on a development machine that has neither IRIX
 * (<sys/dsreq.h>) nor Linux (<scsi/sg.h>) SCSI headers - e.g. macOS.
 *
 * It implements the os.h contract with a synthetic 7-device bus covering the
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
 *
 * Expected result of `make test`: d3 and d5 are marked [TOOLBOX]; d1 is NOT
 * (it must never be accepted on product name alone); d6 is reported as
 * "claims toolbox, no 0xD9 answer"; d4 is skipped; d7 is marked [WIFI] and d8
 * is not - the Wi-Fi check has to be functional too, exactly like the toolbox
 * one, because the emulated and the genuine SCSI/Link are indistinguishable
 * by name.
 *
 * The mock also enforces the host's 0x1C safety contract: 0x1C is standard
 * RECEIVE DIAGNOSTIC RESULTS, and the host must never emit it to a
 * storage-type target, -F included. Any 0x1C arriving at a node whose INQUIRY
 * peripheral device type is not 0x03 aborts the test (see
 * mock_wifi_floor_check).
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

#define MOCK_N 9
static const char *mock_paths[MOCK_N] = {
	"/dev/mock/sc0d0l0",   /* plain SGI disk    - no toolbox at all      */
	"/dev/mock/sc0d1l0",   /* IRIS EMUL DISK    - emulated disk, NO tbox */
	"/dev/mock/sc0d2l0",   /* Sony CD-ROM       - no toolbox             */
	"/dev/mock/sc0d3l0",   /* BlueSCSI          - real toolbox           */
	"/dev/mock/sc0d4l0",   /* dead node         - never answers          */
	"/dev/mock/sc0d5l0",   /* ZuluSCSI          - real toolbox           */
	"/dev/mock/sc0d6l0",   /* liar: page 0x31 but no 0xD9 implementation */
	"/dev/mock/sc0d7l0",   /* emulated DaynaPort: Wi-Fi, no toolbox      */
	"/dev/mock/sc0d8l0"    /* genuine DaynaPort: same name, no radio     */
};

/* Does the device serve MODE SENSE page 0x31 with the toolbox magic? */
static int mock_page31[MOCK_N] = { 0, 0, 0, 1, 0, 0, 1, 0, 0 };
/* Does the device actually IMPLEMENT 0xD9 LIST_DEVICES? */
static int mock_d9[MOCK_N]     = { 0, 0, 0, 1, 0, 1, 0, 0, 0 };
/* Does the device actually IMPLEMENT the 0x1C Wi-Fi commands? */
static int mock_wifi[MOCK_N]   = { 0, 0, 0, 0, 0, 0, 0, 1, 0 };

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
	0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03
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

	if (cmd[0] == 0xDA || cmd[0] == 0xD2) { /* COUNT_CDS / COUNT_FILES */
		if (buf_len >= 1) buf[0] = 2;
		return 0;
	}

	if (cmd[0] == 0xD7 || cmd[0] == 0xD0) { /* LIST_CDS / LIST_FILES */
		memset(buf, 0, buf_len);
		if (buf_len >= 80) {
			buf[0] = 0; buf[1] = 0;
			strcpy((char *)buf + 2, "Doom.iso");
			buf[35] = 0; buf[36] = 0; buf[37] = 0x0A; buf[38] = 0; buf[39] = 0;
			buf[40] = 1; buf[41] = 1;
			strcpy((char *)buf + 42, "games");
			buf[75] = 0; buf[76] = 0; buf[77] = 0; buf[78] = 0x10; buf[79] = 0;
		}
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
 * Mount handling: the mock bus has no filesystems, so nothing is ever mounted
 * and the CD-swap guard always sees a clear path. That is the case worth
 * exercising here - the busy path needs a real mount to be meaningful.
 */
int media_find_mount(const char *path, char *mnt, int mntlen, char *dev, int devlen)
{
	(void)path;
	if (mnt != NULL && mntlen > 0)
		mnt[0] = '\0';
	if (dev != NULL && devlen > 0)
		dev[0] = '\0';
	return 0;
}

int media_unmount(const char *mnt, char *why, int whylen)
{
	(void)mnt;
	if (why != NULL && whylen > 0)
		why[0] = '\0';
	return 0;
}
