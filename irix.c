/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * SGI IRIX SCSI backend (<sys/dsreq.h> DS_ENTER ioctls).
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/dsreq.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <invent.h>
#include <errno.h>

#include "os.h"

/*
 * Ranges walked by scsi_enum_devices(). SCSI-2 allows 8 targets (16 on wide);
 * LUN > 0 is rare on the vintage devices this tool talks to, so we check LUN 0
 * and 1 only. Widening these just costs a few more stat() calls.
 */
#define SCAN_MAX_CTRL   8
#define SCAN_MAX_TARGET 16
#define SCAN_MAX_LUN    2

extern int verbose;

/*
 * Sub-second sleep. IRIX 5.3's libc has no usleep(3) (it fails to link), so use
 * select() with a timeout, which is available and links across IRIX 5.3-6.5.
 */
static void ms_sleep(long ms)
{
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	select(0, NULL, NULL, NULL, &tv);
}

/*
 * Is a mediad daemon currently running?
 *
 * mediad enforces a single instance itself - start a second one and it prints
 * "Another mediad is running. Only one is allowed at a time." and exits. That
 * matters because /etc/init.d/mediad stop runs `mediad -k`, which only ASKS the
 * daemon to exit; it then has to unmount whatever it had mounted before it
 * actually goes. Restarting immediately races that shutdown, and the failure
 * mode is nastier than it looks: the new mediad refuses to start, the old one
 * then finishes exiting, and the system is left with NO mediad at all - so a
 * newly-switched CD never gets mounted.
 *
 * ps(1) is /sbin/ps with a symlink at /usr/bin/ps, so run it unqualified and
 * let PATH find it. `ps -e` prints the command NAME (not the full argv), so
 * this cannot match the shell running the pipeline; grep -v grep drops the
 * grep itself.
 */
static int mediad_running(void)
{
	return system("ps -e 2>/dev/null | grep mediad | grep -v grep >/dev/null 2>&1") == 0;
}

int mediad_start(void) {
    int status;

    if (mediad_running()) {
        if (verbose)
            fprintf (stdout, "mediad is already running\n");
        return 0;
    }

    /* Starting mediad service */
    if (verbose)
    	fprintf (stdout, "Starting mediad...\n");
    status = system("/etc/init.d/mediad start");
    if (status != 0) {
        fprintf(stderr, "Failed to start mediad service: %s\n", strerror(errno));
        return 1;
    }

    /*
     * The init script only starts mediad if `chkconfig mediad` is on, and says
     * nothing when it isn't. Without this check the CD silently never remounts
     * and there is no clue why.
     */
    if (!mediad_running()) {
        fprintf(stderr, "Warning: mediad did not start. If removable media should be\n");
        fprintf(stderr, "automounted, check 'chkconfig mediad' is on.\n");
        return 1;
    }
    return 0;
}

int mediad_stop(void) {
    int status;
    int i;

    if (!mediad_running()) {
        if (verbose)
            fprintf (stdout, "mediad is not running, nothing to stop\n");
        return 0;
    }

    /* Stop mediad service */
    if (verbose)
    	fprintf (stdout, "Stopping mediad...\n");
    status = system("/etc/init.d/mediad stop");
    if (status != 0)
        fprintf(stderr, "Warning: '/etc/init.d/mediad stop' returned an error\n");

    /*
     * Wait for it to actually be gone. `mediad -k` is asynchronous - it signals
     * the daemon, which then unmounts its media before exiting - so returning
     * here immediately is what caused "Another mediad is running" on the
     * restart, and with it a CD that never remounted. Ten seconds is far longer
     * than an unmount needs; we warn and carry on rather than hang forever.
     */
    for (i = 0; i < 100; i++) {
        if (!mediad_running()) {
            if (verbose)
                fprintf (stdout, "mediad stopped\n");
            return 0;
        }
        ms_sleep(100);
    }

    fprintf(stderr, "Warning: mediad still running 10s after being asked to stop.\n");
    fprintf(stderr, "Continuing anyway; the CD may not remount by itself.\n");
    return 1;
}

static int test_dsreq_flags(int dev_fd, uint flag)
{
   dsconf_t config;
   int ret;
   ret = ioctl(dev_fd, DS_CONF, &config);
   if (verbose) {
      /* dsc_iomax/dsc_biomax are ulong in <sys/dsreq.h>, not int. */
      fprintf (stdout, "dsc_iomax: %lu\n", (unsigned long)config.dsc_iomax);
      fprintf (stdout, "dsc_biomax: %lu\n", (unsigned long)config.dsc_biomax);
      fprintf (stdout, "SCSI Bus:%i Max Target:%i Max LUN:%i\n", config.dsc_bus, config.dsc_imax, config.dsc_lmax);
   }
   if (!ret) { /* no problem in ioctl */
      return (flag & config.dsc_flags);
   } else { /* ioctl failure */
      return 0; /* not supported, it seems */
   }
}


int scsi_open(char *path, int readonly)
{
	int ret;
	if (readonly)
		ret = open(path, O_RDONLY | O_SYNC);
	
	else
		ret = open(path, O_RDWR | O_SYNC);
	if (verbose && ret >= 0)
		fprintf (stdout, "test flags: %i\n", test_dsreq_flags(ret, DSRQ_BUF));
	return ret;
}



int scsi_close(int dev)
{
	return close(dev);
}

int scsi_send_command(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	int i;
	int try;
	dsreq_t r;
	memset(&r, 0, sizeof(dsreq_t));
	
	/* Assemble the request structure */
	r.ds_cmdbuf   = (caddr_t) cmd;
	r.ds_cmdlen   = cmd_len;
	r.ds_databuf  = (caddr_t) buf;
	r.ds_datalen  = buf_len;
	/*r.ids_sensebuf = (caddr_t) buf; */
	/*r.ds_senselen = buf_len; */
	r.ds_sensebuf = NULL;
	r.ds_senselen = 0;
	
	r.ds_time     = 5 * 1000;  /* 5 seconds should be enough */
	r.ds_flags    = DSRQ_READ;
	
	if (verbose){
		fprintf(stdout, "Sending SCSI command: ");
		for (i = 0; i < cmd_len; ++i) {
			fprintf(stdout, "%02x ", (unsigned char)r.ds_cmdbuf[i]);
		}
		fprintf(stdout, "\n");
	}	
	/* Issue the request. Retry a handful of times with a short delay; a
	 * failed command must report failure (returning 0 here previously made
	 * callers treat errors as success). */
	for (try = 0; try < 10; try++){
		if (ioctl(dev, DS_ENTER, &r) == 0 && r.ds_status == 0)
			return 0;
		fprintf(stderr, "WARNING: SCSI command failed (status %d); retrying...\n", r.ds_status);
		ms_sleep(100); /* 100ms between retries */
	}
	fprintf(stderr, "ERROR: SCSI command failed after retries (status %d)\n", r.ds_status);
	return 1;
}

/*
 * Quiet, single-attempt data-in command for the bus scanner. Same request shape
 * as scsi_send_command() but with a short timeout, no retry loop and no output,
 * so probing an empty or unresponsive target costs one failed ioctl instead of
 * ten retries, a second of sleeping and ten warning lines.
 */
int scsi_send_command_probe(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	dsreq_t r;
	memset(&r, 0, sizeof(dsreq_t));

	r.ds_cmdbuf   = (caddr_t) cmd;
	r.ds_cmdlen   = cmd_len;
	r.ds_databuf  = (caddr_t) buf;
	r.ds_datalen  = buf_len;
	r.ds_sensebuf = NULL;
	r.ds_senselen = 0;
	r.ds_time     = 2 * 1000;  /* 2s: a present target answers INQUIRY at once */
	r.ds_flags    = DSRQ_READ;

	if (ioctl(dev, DS_ENTER, &r) == 0 && r.ds_status == 0)
		return 0;
	return 1;
}

/*
 * Enumerate the generic SCSI (dsreq) character devices IRIX exposes, one per
 * attached target, named /dev/scsi/sc<controller>d<target>l<lun>. The nodes are
 * created for the devices found at boot, so we walk the plausible controller /
 * target / LUN ranges and keep whichever character devices actually exist.
 */
int scsi_enum_devices(char paths[][SCSI_PATH_MAX], int max)
{
	struct stat st;
	char path[SCSI_PATH_MAX];
	int ctrl, target, lun;
	int n = 0;

	for (ctrl = 0; ctrl < SCAN_MAX_CTRL; ctrl++) {
		for (target = 0; target < SCAN_MAX_TARGET; target++) {
			for (lun = 0; lun < SCAN_MAX_LUN; lun++) {
				if (n >= max)
					return n;
				sprintf(path, "/dev/scsi/sc%dd%dl%d", ctrl, target, lun);
				if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
					strcpy(paths[n], path);
					n++;
				}
			}
		}
	}
	return n;
}
#define MAX_READY_RETRIES 10
#define SENSE_BUF_LEN 64
#define STATUS_CHECKCOND 0x02

static int scsi_wait_until_ready(int dev)
{
    dsreq_t tur;
    unsigned char tur_cmd[6] = { 0x00, 0, 0, 0, 0, 0 };  /* TEST UNIT READY */
    unsigned char sense_data[SENSE_BUF_LEN];
    int i;
    for (i = 0; i < MAX_READY_RETRIES; ++i) {
        memset(&tur, 0, sizeof(dsreq_t));
        memset(sense_data, 0, sizeof(sense_data));

        tur.ds_cmdbuf = (caddr_t) tur_cmd;
        tur.ds_cmdlen = sizeof(tur_cmd);
        tur.ds_databuf = NULL;
        tur.ds_datalen = 0;
        tur.ds_sensebuf = (caddr_t) sense_data;
        tur.ds_senselen = sizeof(sense_data);
        tur.ds_time = 1000; /* 1 second timeout */
        tur.ds_flags = 0;

        if (ioctl(dev, DS_ENTER, &tur) == 0) {
            /* Command completed, check if status was good */
            if (tur.ds_status == 0) {
                return 0;  /* Device is ready */
            } else if (tur.ds_status == STATUS_CHECKCOND) {
                if (verbose) {
                    fprintf(stderr, "SCSI CHECK CONDITION on TUR, sense key: 0x%02x\n", sense_data[2] & 0x0F);
                }
            }
        } else {
            if (verbose) {
                perror("TEST UNIT READY ioctl failed");
            }
        }

        ms_sleep(100); /* wait 100ms before retrying */
    }

    return -1;  /* Device not ready after retries */
}
/*
 * Send a SCSI command that writes data TO the device (data-out), e.g. the
 * toolbox SEND_FILE sequence. Waits once for the target to be ready, issues the
 * command with sense collection enabled, and reports CHECK CONDITION sense
 * keys. Returns 0 on success, negative on error.
 */
int scsi_send_commandw(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len)
{
	int i;
	dsreq_t r;
	unsigned char sense_data[256];  /* buffer for sense data */

	/* Wait once for the device to be ready, then issue the write.
	 * Previously this routine did a second TEST UNIT READY loop and slept
	 * 100ms unconditionally after every command. Because a PUT sends the
	 * file in 512-byte blocks, those per-block stalls made multi-KB/MB
	 * transfers take minutes and appear to hang the system (issue #3).
	 * scsi_wait_until_ready() already returns immediately once the target
	 * reports good status, so a single check per command is enough. */
	if (scsi_wait_until_ready(dev) != 0) {
	    fprintf(stderr, "Device not ready, aborting transfer\n");
	    return -1;
	}

	memset(&r, 0, sizeof(dsreq_t));
	memset(sense_data, 0, sizeof(sense_data));

	/* Prepare sense buffer and command info */
	r.ds_cmdbuf   = (caddr_t) cmd;
	r.ds_cmdlen   = cmd_len;
	r.ds_databuf  = (caddr_t) buf;
	r.ds_datalen  = buf_len;
	r.ds_sensebuf = (caddr_t) sense_data;
	r.ds_senselen = (u_char) sizeof(sense_data);

	r.ds_time     = 30 * 1000;
	r.ds_flags    = DSRQ_WRITE | DSRQ_SENSE;

	if (verbose) {
		fprintf(stdout, "Sending SCSI command: ");
		for (i = 0; i < cmd_len; ++i)
			fprintf(stdout, "%02x ", (unsigned char)r.ds_cmdbuf[i]);
		fprintf(stdout, "\n");
	}

	/* Send the actual command */
	if (ioctl(dev, DS_ENTER, &r) != 0) {
		perror("ioctl failed");
		return -errno;
	}

	if (r.ds_status == STATUS_CHECKCOND) {  /* CHECK CONDITION is usually 0x02 */
		fprintf(stderr, "SCSI CHECK CONDITION\n");
		if (r.ds_senselen >= 14) {
			unsigned char key = sense_data[2] & 0x0F;
			unsigned char asc = sense_data[12];
			unsigned char ascq = sense_data[13];
			fprintf(stderr, "Sense Key: 0x%02x, ASC: 0x%02x, ASCQ: 0x%02x\n", key, asc, ascq);
		} else {
			fprintf(stderr, "Sense data too short (%d bytes)\n", r.ds_senselen);
		}
		return -EIO;
	}

	return 0;
}

/*
 * Pull the SCSI target id out of an IRIX device path of the form
 * /dev/scsi/sc<ctrl>d<target>l<lun> (e.g. /dev/scsi/sc0d1l0 -> 1). The 'd'
 * field is the SCSI target id, which indexes device_list[]. Returns the id
 * (0-7) or -1 if the path doesn't match or the id is out of range.
 */
/*
 * Pull BOTH the controller and the target id out of /dev/scsi/scNdNlN.
 * path_to_devnum() below discards the controller, but the block-device names
 * we have to match against (/dev/dsk/dks<c>d<id>s<n>) carry both.
 */
static int parse_scsi_path(const char *path, int *ctlr, int *id)
{
	if (strncmp(path, "/dev/scsi/sc", 12) != 0)
		return -1;
	if (sscanf(path, "/dev/scsi/sc%dd%dl%*d", ctlr, id) != 2)
		return -1;
	if (*id < 0 || *id > 7)
		return -1;
	return 0;
}

int media_find_mount(const char *path, char *mnt, int mntlen, char *dev, int devlen)
{
	FILE *fp;
	char line[512];
	char mdev[256];
	char mpt[256];
	char want[32];
	int ctlr, id;

	if (mnt != NULL && mntlen > 0)
		mnt[0] = '\0';
	if (dev != NULL && devlen > 0)
		dev[0] = '\0';

	if (parse_scsi_path(path, &ctlr, &id) != 0)
		return -1;

	/*
	 * Block devices for this target are /dev/dsk/dks<c>d<id>s<n> (raw ones
	 * /dev/rdsk/...). Match the stem so any partition of the same target
	 * counts - it is the physical disc being swapped, not one slice of it.
	 */
	sprintf(want, "dks%dd%d", ctlr, id);

	/* /etc/mtab: "device mountpoint fstype options freq passno". */
	fp = fopen("/etc/mtab", "r");
	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "%255s %255s", mdev, mpt) != 2)
			continue;
		if (strstr(mdev, want) == NULL)
			continue;

		if (mnt != NULL && mntlen > 0) {
			strncpy(mnt, mpt, mntlen - 1);
			mnt[mntlen - 1] = '\0';
		}
		if (dev != NULL && devlen > 0) {
			strncpy(dev, mdev, devlen - 1);
			dev[devlen - 1] = '\0';
		}
		fclose(fp);
		return 1;
	}
	fclose(fp);
	return 0;
}

int media_unmount(const char *mnt, char *why, int whylen)
{
	char cmd[640];
	char tmp[64];
	FILE *fp;
	int n = 0;
	int c;

	if (why != NULL && whylen > 0)
		why[0] = '\0';

	sprintf(cmd, "umount %s 2>/dev/null", mnt);
	if (system(cmd) == 0)
		return 0;

	/*
	 * Still mounted. fuser(1M) names the processes holding it, which is the
	 * whole difference between "try again in a second" and "you have a shell
	 * sitting in /CDROM". -c asks about the mounted filesystem rather than
	 * just the directory node. It writes the filename to stderr and the pids
	 * to stdout, so both are captured.
	 *
	 * Via a temp file rather than popen(3): IRIX 5.3's <stdio.h> declares
	 * popen only under _SVR4_SOURCE or _XOPEN_SOURCE, and nothing in the
	 * default MIPSpro environment defines either - so a popen() call here
	 * would compile on an implicit declaration and truncate its FILE* return
	 * to an int. system()/fopen() carry no such condition.
	 */
	if (why == NULL || whylen < 2)
		return -1;

	sprintf(tmp, "/tmp/.irixscsitb.%d", (int)getpid());
	sprintf(cmd, "fuser -c %s > %s 2>&1", mnt, tmp);
	(void)system(cmd);      /* fuser's exit status is not meaningful here */

	fp = fopen(tmp, "r");
	if (fp != NULL) {
		while (n < whylen - 1) {
			c = fgetc(fp);
			if (c == EOF)
				break;
			why[n++] = (char)c;
		}
		fclose(fp);
	}
	why[n] = '\0';
	unlink(tmp);
	return -1;
}

int path_to_devnum(const char *path) {
	int dev_path_num;

	if (strncmp(path, "/dev/scsi/sc", 12) != 0 ||
	    sscanf(path, "/dev/scsi/sc%*dd%dl%*d", &dev_path_num) != 1) {
		fprintf(stderr, "ERROR: Invalid path format (expected /dev/scsi/scNdNlN): %s\n", path);
		return -1;
	}
	if (dev_path_num < 0 || dev_path_num > 7) {
		fprintf(stderr, "ERROR: SCSI target id %d out of range in %s\n", dev_path_num, path);
		return -1;
	}

	return dev_path_num;
}
