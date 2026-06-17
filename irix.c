#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/dsreq.h>
#include <invent.h>
#include <errno.h>

#include "os.h"

extern int verbose;

int mediad_start(void) {
    int status;

    // Starting mediad service
    if (verbose)
    	fprintf (stdout, "Starting mediad...\n");
    status = system("/etc/init.d/mediad start");
    if (status != 0) {
        fprintf(stderr, "Failed to start mediad service: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

int mediad_stop(void) {
    int status;

    // Stop mediad service
    if (verbose)
    	fprintf (stdout, "Stopping mediad...\n");
    status = system("/etc/init.d/mediad stop");
    if (status != 0) {
        fprintf(stderr, "Failed to stop mediad service: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int test_dsreq_flags(int dev_fd, uint flag)
{
   dsconf_t config;
   int ret;
   ret = ioctl(dev_fd, DS_CONF, &config);
   if (verbose) {
      fprintf (stdout, "dsc_iomax: %i\n", config.dsc_iomax);
      fprintf (stdout, "dsc_biomax: %i\n", config.dsc_biomax);
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
	//r.ids_sensebuf = (caddr_t) buf;
	//r.ds_senselen = buf_len;
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
		usleep(100000); /* 100ms between retries */
	}
	fprintf(stderr, "ERROR: SCSI command failed after retries (status %d)\n", r.ds_status);
	return 1;
}
#define MAX_READY_RETRIES 10
#define SENSE_BUF_LEN 64
#define STATUS_CHECKCOND 0x02

static int scsi_wait_until_ready(int dev)
{
    dsreq_t tur;
    unsigned char tur_cmd[6] = { 0x00, 0, 0, 0, 0, 0 };  // TEST UNIT READY
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
        tur.ds_time = 1000; // 1 second timeout
        tur.ds_flags = 0;

        if (ioctl(dev, DS_ENTER, &tur) == 0) {
            // Command completed, check if status was good
            if (tur.ds_status == 0) {
                return 0;  // Device is ready
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

        usleep(100000); // wait 100ms before retrying
    }

    return -1;  // Device not ready after retries
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
	unsigned char sense_data[256];  // buffer for sense data

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
