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
 *
 * Expected result of `make test`: d3 and d5 are marked [TOOLBOX]; d1 is NOT
 * (it must never be accepted on product name alone); d6 is reported as
 * "claims toolbox, no 0xD9 answer"; d4 is skipped.
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
#include <string.h>
#include "os.h"

extern int verbose;

#define MOCK_N 7
static const char *mock_paths[MOCK_N] = {
	"/dev/mock/sc0d0l0",   /* plain SGI disk    - no toolbox at all      */
	"/dev/mock/sc0d1l0",   /* IRIS EMUL DISK    - emulated disk, NO tbox */
	"/dev/mock/sc0d2l0",   /* Sony CD-ROM       - no toolbox             */
	"/dev/mock/sc0d3l0",   /* BlueSCSI          - real toolbox           */
	"/dev/mock/sc0d4l0",   /* dead node         - never answers          */
	"/dev/mock/sc0d5l0",   /* ZuluSCSI          - real toolbox           */
	"/dev/mock/sc0d6l0"    /* liar: page 0x31 but no 0xD9 implementation */
};

/* Does the device serve MODE SENSE page 0x31 with the toolbox magic? */
static int mock_page31[MOCK_N] = { 0, 0, 0, 1, 0, 0, 1 };
/* Does the device actually IMPLEMENT 0xD9 LIST_DEVICES? */
static int mock_d9[MOCK_N]     = { 0, 0, 0, 1, 0, 1, 0 };

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

int scsi_send_commandw(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	(void)dev; (void)cmd; (void)cmd_len; (void)buf; (void)buf_len;
	return 0;
}
