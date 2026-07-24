/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Linux SCSI backend (<scsi/sg.h> SG_IO ioctls).
 *
 * Copyright (C) 2024-2025 SonnyJim
 * Copyright (C) 2025 Daniel Palmer
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

/*
 * This file contains the code for talking to SCSI devices on Linux
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "os.h"

/* How many /dev/sgN nodes scsi_enum_devices() looks for. */
#define SCAN_MAX_SG 64

extern int verbose;
/*
 * mediad is an IRIX-only removable-media daemon that must be paused while a CD
 * image is swapped. Linux has no direct equivalent (a desktop automounter would
 * be distro-specific), so these are intentional no-op stubs - swapping CDs from
 * Linux just doesn't coordinate with any host mounter.
 */
int mediad_start(void)
{
	if (verbose)
		fprintf(stderr, "mediad_start(): no equivalent on Linux, skipping\n");
	return 0;
}

int mediad_stop(void)
{
	if (verbose)
		fprintf(stderr, "mediad_stop(): no equivalent on Linux, skipping\n");
	return 0;
}

int scsi_open(char *path, int readonly)
{
	if (readonly)
		return open(path, O_RDONLY | O_SYNC);
	
	else
		return open(path, O_RDWR | O_SYNC);
}

int scsi_close(int dev)
{
	return close(dev);
}

int scsi_send_command(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	int i;
	struct sg_io_hdr io_hdr = { 0 };

	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = cmd_len;
	io_hdr.mx_sb_len = 0;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	/* 5000ms should be enough */
	io_hdr.timeout = 5000;

	if (verbose){
		fprintf(stdout, "Sending SCSI command: ");
		for (i = 0; i < cmd_len; ++i) {
			fprintf(stdout, "%02x ", (unsigned char) io_hdr.cmdp[i]);
		}
		fprintf(stdout, "\n");
	}

	if (ioctl(dev, SG_IO, &io_hdr) < 0)
		return 1;

	return 0;
}

/*
 * Quiet, single-attempt data-in command for the bus scanner. SG_IO already does
 * one attempt with no retries, so this is scsi_send_command() minus the verbose
 * CDB dump and with a shorter timeout - probing a node that nothing answers on
 * should be cheap and silent.
 */
int scsi_send_command_probe(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	struct sg_io_hdr io_hdr = { 0 };

	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = cmd_len;
	io_hdr.mx_sb_len = 0;
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	io_hdr.timeout = 2000;

	if (ioctl(dev, SG_IO, &io_hdr) < 0)
		return 1;
	if (io_hdr.status != 0)
		return 1;

	return 0;
}

/*
 * Enumerate the Linux generic SCSI character devices, /dev/sgN. These are the
 * pass-through nodes (the sg driver); the matching block devices (/dev/sdX) do
 * not accept arbitrary vendor CDBs.
 */
int scsi_enum_devices(char paths[][SCSI_PATH_MAX], int max)
{
	struct stat st;
	char path[SCSI_PATH_MAX];
	int i;
	int n = 0;

	for (i = 0; i < SCAN_MAX_SG; i++) {
		if (n >= max)
			return n;
		sprintf(path, "/dev/sg%d", i);
		if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
			strcpy(paths[n], path);
			n++;
		}
	}
	return n;
}

/*
 * Send a SCSI command that writes data TO the device (data-out, SG_DXFER_TO_DEV),
 * e.g. the toolbox SEND_FILE sequence. With verbose enabled it also hexdumps the
 * outgoing data buffer. Returns 0 on success, 1 on ioctl failure.
 */
int scsi_send_commandw(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	int i;
	struct sg_io_hdr io_hdr = { 0 };

	io_hdr.interface_id = 'S';
	io_hdr.cmdp = cmd;
	io_hdr.cmd_len = cmd_len;
	io_hdr.mx_sb_len = 0;
	io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
	io_hdr.dxferp = buf;
	io_hdr.dxfer_len = buf_len;
	/* 5000ms should be enough */
	io_hdr.timeout = 5000;

	if (verbose){
		fprintf(stdout, "Sending SCSI command: ");
		for (i = 0; i < cmd_len; ++i) {
			fprintf(stdout, "%02x ", (unsigned char) io_hdr.cmdp[i]);
		}
		fprintf(stdout, "\n");
		fprintf(stdout, "Data to device (%d bytes):\n", buf_len);
		for (i = 0; i < buf_len; i += 16) {
			int j;
			fprintf(stdout, "  %04x: ", i);

			/* Hex part */
			for (j = 0; j < 16; ++j) {
			    if (i + j < buf_len) {
				fprintf(stdout, "%02x ", buf[i + j]);
			    } else {
				fprintf(stdout, "   "); /* padding */
			    }
			}

			fprintf(stdout, " |");

			/* ASCII part */
			for (j = 0; j < 16; ++j) {
			    if (i + j < buf_len) {
				unsigned char c = buf[i + j];
				fprintf(stdout, "%c", (c >= 32 && c <= 126) ? c : '.');
			    }
			}

			fprintf(stdout, "|\n");
		}
	}
	if (ioctl(dev, SG_IO, &io_hdr) < 0)
		return 1;

	return 0;
}

/**
 * Extract the SCSI ID from a /dev/sgX device path.
 * Returns: SCSI ID on success, -1 on failure.
 */
int path_to_devnum(const char *path) {
    struct sg_scsi_id scsi_id;
    int fd;

    /* Validate the path format */
    if (strncmp(path, "/dev/sg", 7) != 0 || strlen(path) < 8) {
        fprintf(stderr, "ERROR: Unsupported device path format: %s\n", path);
        return -1;
    }

    /* Open the device */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Attempt to get the SCSI ID */
    if (ioctl(fd, SG_GET_SCSI_ID, &scsi_id) < 0) {
        fprintf(stderr, "ERROR: ioctl SG_GET_SCSI_ID failed on %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    if (verbose)
	    fprintf(stderr, "Found something at SCSI ID%i\n", scsi_id.scsi_id);
    return scsi_id.scsi_id;
}

/*
 * Mount handling around a CD swap - no-ops on Linux. There is no mediad, and
 * no convention about who owns a removable mount (that is a desktop
 * automounter's business and entirely distro-specific), so claiming "not
 * mounted" is the honest answer rather than guessing at /proc/mounts and
 * unmounting something the user asked for.
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
