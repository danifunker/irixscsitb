/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Speaks the BlueSCSI/ZuluSCSI Toolbox API (vendor commands 0xD0-0xDA): list
 * and transfer files in the device's shared directory, list and swap CD images,
 * enumerate emulated targets and toggle firmware debug logging.
 *
 * Firmware-neutral by design: devices are recognised by the toolbox API they
 * implement, not by which product they claim to be. See toolbox_firmware_ids[]
 * and toolbox_confirm() for how a target is accepted.
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
#include "irixscsitb.h"

/* Forward declaration: the definition lives further down, but toolbox_listcds
 * (which precedes it in this file) now needs it. */
static long int size_to_long(const unsigned char size[5]);

/*
 * Are we effectively root? The generic SCSI device nodes are normally mode 0600
 * root-owned, so anything short of uid 0 will fail at open(). We ask the system
 * (geteuid) rather than telling every user to "make sure you run as root" -
 * that advice is only worth printing when it actually applies.
 */
static int running_as_root(void)
{
	return geteuid() == 0;
}

/*
 * Sending Files
 * Sending files from the Host to the SD card is a three-step process:
 *
 * TOOLBOX_SEND_FILE_PREP 0xD3 to prepare a file on the SD card for receiving.
 * TOOLBOX_SEND_FILE_10 0xD4 to send the actual data of the file.
 * TOOLBOX_SEND_FILE_END 0xD5 to close the file.
 */

/*
 * TOOLBOX_SEND_FILE_PREP 0xD3
 * Prepares a file on the SD card in the ToolBoxSharedDir (Default /shared) for receiving.
 *
 * The file name is 33 char name sent in the SCSI data, null terminated. The name should only contain valid characters for file names on FAT32/ExFAT.
 *
 * If the file is not able to be created a CHECK_CONDITION ILLEGAL_REQUEST is set as the sense.
 *
 * TOOLBOX_SEND_FILE_10 0xD4
 * Receive data from the host in blocks of 512 bytes.
 *
 * CDB[1..2] - Number of bytes sent in this request. Big endian. Minimum 1, maximum 512.
 *
 * CDB[3..5] - Block number in the file for these bytes. Big endian.
 *
 * If the file has a write error sense will be set as CHECK_CONDITION ILLEGAL_REQUEST. You may try to resend the block or fail and call TOOLBOX_SEND_FILE_END
 *
 * NOTE: The number of bytes sent should be 512 in all but the final block of the file.
 *
 * TOOLBOX_SEND_FILE_END 0xD5
 * Once the file is completely sent this command will close the file.
 */
static int toolbox_sendfile(int dev, char *path)
{
	char cmd[10] = { TOOLBOX_SEND_FILE_PREP, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	char filename[NAME_BUF_SIZE];
	char *base_name;
	char send_buf[SEND_BUF_SIZE];
	long int bytes_read = 0;
	long int actual_read = 0;
	int chunk;
	int buf_idx = 0;
	int ret;
	FILE *fd;
	long int filesize;
	struct stat st;

	if (verbose)
		fprintf(stdout, "sendfile: %s\n", path);

	memset(send_buf, 0, sizeof(send_buf));
	/* Extract the base filename ourselves rather than via basename(3): POSIX
	 * basename() may modify its argument and lives in <libgen.h>, whose
	 * availability varies across the IRIX 5.3-6.5 range we target. */
	base_name = strrchr(path, '/');
	if (base_name == NULL) {
		base_name = path;
	} else {
		base_name++; /* skip the slash */
	}

	if (strlen(base_name) >= NAME_BUF_SIZE) {
		fprintf(stderr, "Error: sendfile Filename too long: %s\n", base_name);
		return -1;
	}

	memset(filename, 0, NAME_BUF_SIZE);
	strncpy(filename, base_name, NAME_BUF_SIZE - 1);

	/* Open file */
	fd = fopen(path, "rb");
	if (fd == NULL) {
		fprintf(stderr, "Error: sendfile couldn't open %s\n", path);
		return 1;
	}

	if (stat(path, &st) == 0) {
		if (verbose)
			printf("File size of %s is %ld bytes\n", filename, (long)st.st_size);
	} else {
		fprintf(stderr, "Error: sendfile couldn't stat %s\n", path);
		fclose(fd);
		return 1;
	}
	filesize = st.st_size;

	/* Send filename */
	if (scsi_send_commandw(dev, (unsigned char *)cmd, SCSI_CMD_LENGTH, (unsigned char *)filename, 33) != 0) {
		fprintf(stderr, "Error: sendfileprep failed - %s\n", strerror(errno));
		fclose(fd);
		return 1;
	}

	/* Prepare to send file data */
	cmd[0] = TOOLBOX_SEND_FILE_10;

	while (bytes_read < filesize) {
		chunk = (filesize - bytes_read) < SEND_BUF_SIZE ? (filesize - bytes_read) : SEND_BUF_SIZE;
		memset(send_buf, 0, SEND_BUF_SIZE);

		actual_read = fread(send_buf, 1, chunk, fd);
		if (actual_read <= 0) {
			fprintf(stderr, "Error: fread failed or returned 0 at offset %ld\n", bytes_read);
			fclose(fd);
			return 1;
		}

		cmd[1] = (actual_read & 0xFF00) >> 8;
		cmd[2] = (actual_read & 0xFF);

		cmd[3] = (buf_idx & 0xFF0000) >> 16;
		cmd[4] = (buf_idx & 0xFF00) >> 8;
		cmd[5] = (buf_idx & 0xFF);

		ret = scsi_send_commandw(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)send_buf, actual_read);
		if (ret != 0) {
			fprintf(stderr, "Error: sendfile10 failed - %s\n", strerror(errno));
			fclose(fd);
			return 1;
		}

		bytes_read += actual_read;
		buf_idx++;
	}

	/* Send file end command */
	memset(cmd, 0, sizeof(cmd));
	cmd[0] = TOOLBOX_SEND_FILE_END;

	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), NULL, 0) != 0) {
		fprintf(stderr, "Error: sendfileend failed - %s\n", strerror(errno));
		fclose(fd);
		return 1;
	}

	fclose(fd);
	return 0;
}


/*
 * TOOLBOX_TOGGLE_DEBUG 0xD6
 * Enable or disable Debug logs. Also allows you to get the current status.
 *
 * If CDB[1] is set to 0 it is the subcommand Set Debug. The value of CDB[2] is used as the boolean value for the debug flag.
 *
 * If CDB[1] is set to 1 it is the subcommand Get Debug. The boolean value is sent as a 1 byte value.
 *
 * NOTE: Debug logs significantly decrease performance while enabled. When your app enables debug you MUST notify them of the decreased performance.
 */

static int toolbox_getdebug (int dev)
{
	int ret;
	char cmd[10] = {TOOLBOX_TOGGLE_DEBUG, 0, 0, 0, 0, 0, 0, 0, 0, 0};	
	char buf[1];
	cmd[1] = DEBUG_GET;/*Get debug flag */
	memset(buf, 0, sizeof(buf));
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, sizeof(buf)) != 0)
	{
		fprintf (stderr, "Error: getdebug failed - %s\n", strerror(errno));
		return -1;
	}
	ret = buf[0];
	return ret;
}

static int toolbox_setdebug (int dev, int value)
{
	char cmd[10] = {TOOLBOX_TOGGLE_DEBUG, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	
	if (value > 1)
		value = 1;
	else if (value < 0)
		value = 0;
	cmd[1] = DEBUG_SET;
	cmd[2] = value;
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)NULL, 0) != 0)
	{
		fprintf (stderr, "Error: setdebug failed - %s\n", strerror(errno));
		return -1;
	}

	if (verbose)
		fprintf (stdout, "Debug mode set to: %i\n", toolbox_getdebug (dev));
	return 0;
}

/*
 * TOOLBOX_COUNT_FILES 0xD2
 * Counts the number of files in the ToolBoxSharedDir (default /shared). The purpose is to allow the host program to know how much data will be sent back by the List files function.
 *
 * This can be sent to any valid toolbox target.
 */

static int toolbox_countfiles(int dev)
{
	char cmd[10] = {TOOLBOX_COUNT_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};	
	char buf[1];
	int ret;
	memset(buf, 0, sizeof(buf));
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, sizeof(buf)) != 0)
	{
		fprintf (stderr, "Error: countfiles failed - %s\n", strerror(errno));
		return -1;
	}
	ret = buf[0]; /*Maximum of 100 files  */
	return ret;
}

/** TOOLBOX_COUNT_CDS (read, length 10)
 * Input:
 *  CDB 00 = command byte
 * Output:
 *  Single byte indicating number of CD images available. (Max 100.)
 */
static int toolbox_countcds(int dev)
{
	char cmd[10] = {TOOLBOX_COUNT_CDS, 0, 0, 0, 0, 0, 0, 0, 0, 0};	
	char buf[1];
	int ret;
	memset(buf, 0, sizeof(buf));
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, sizeof(buf)) != 0)
	{
		fprintf (stderr, "Error: countcds failed - %s\n", strerror(errno));
		return -1;
	}
	ret = buf[0]; /*Maximum of 100 files */
	if (ret < 0 || ret > MAX_FILES)
	{
		fprintf (stderr,"Error: countcds invalid count %i\n", ret);
		return -1;
	}
	return ret;
}

/** TOOLBOX_SET_NEXT_CD (read, length 10)
 * Input:
 *  CDB 00 = command byte
 *  CDB 01 = image file index byte to change to
 * Output:
 *  None.
 */
static int toolbox_setnextcd(int dev, int num)
{
	int max_cds;
	char cmd[10];
	memset(cmd, 0, sizeof(cmd));	
	max_cds = toolbox_countcds(dev);
	cmd[0] = TOOLBOX_SET_NEXT_CD;

	if (num < 0 || num > max_cds)
	{
		fprintf (stderr, "setnextcd: %i is out of range of max %i\n", num, max_cds);
		return 1;
	}

	cmd[1] = num;
	if (verbose)
		fprintf (stdout, "%i set as next CD\n", cmd[1]);	
	if (scsi_send_command(dev, (unsigned char *)cmd, 10, (unsigned char *)NULL, 0) != 0)
	{
		fprintf (stderr, "Error: setnextcd failed - %s\n", strerror(errno));
		return -1;
	}
	return 0;
}
/*
 * TOOLBOX_LIST_CDS 0xD7
 * Lists all the files for the current directory for the selected SCSI ID target. Eg: When selecting SCSI ID 3 it will look for a CD3 folder and list files from there. The structure is ToolboxFileEntry.
 *
 * NOTE: Since there is no universal name for a CD image there is no filtering done on the lists of files.
 */
static int toolbox_listcds(int dev)
{
	char cmd[10] = {TOOLBOX_LIST_CDS, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	char *buf;
	int i;
	int buf_size;
	int num_cds;
	ToolboxFileEntry entry;

	num_cds = toolbox_countcds (dev);
	if (num_cds < 0 || num_cds > MAX_FILES)
	{
		fprintf (stderr, "Error:  CD number requested invalid: %i\n", num_cds);
		return -1;
	}
	fprintf (stdout, "Found %i CDs\n", num_cds);
	if (num_cds == 0)
		return 0;

	buf_size = sizeof(ToolboxFileEntry) * num_cds;
	buf = (char *)malloc(buf_size);
	if (buf == NULL)
	{
		fprintf (stderr, "Error: failed to malloc %i bytes: - %s\n", buf_size, strerror(errno));
		return -1;
	}

	memset(buf, 0, buf_size);
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, buf_size) != 0)
	{
		fprintf (stderr, "Error: listcds failed - %s\n", strerror(errno));
		free(buf);
		return -1;
	}

	/* Parse the packed ToolboxFileEntry array the same way listfiles does, so
	 * CD listings show index, name and size instead of a raw byte dump. */
	for (i = 0; i < num_cds; i++)
	{
		memcpy(&entry, buf + i * sizeof(ToolboxFileEntry), sizeof(ToolboxFileEntry));
		entry.name[sizeof(entry.name) - 1] = '\0';
		fprintf (stdout, "#%i %s%s %li bytes\n", entry.index, entry.name,
			entry.type == 1 ? "/" : "", size_to_long(entry.size));
	}
	free(buf);
	return 0;
}

/*Helper function to convert 40bit size into a long */
static long int size_to_long(const unsigned char size[5])
{
    int i;
    long int result = 0;
    for (i = 0; i < 5; i++)
    {
        result = (result << 8) | size[i];
    }
    return result;
}

/*
 * Receiving Files
 * The Host machine can read files directly off the SD card and write them to the Host.
 *
 * To receive files
 *
 * Count and list the files with TOOLBOX_COUNT_FILES 0xD2 and TOOLBOX_LIST_FILES 0xD0.
 * Use the file index and byte offset to transfer the desired file with TOOLBOX_GET_FILE 0xD1.
 *
 */


/*
 * TOOLBOX_LIST_FILES 0xD0
 * Returns a list of files in the ToolBoxSharedDir (Default /shared) in a ToolboxFileEntry struct
 * NOTE: File names are truncated to 32 chars - but can still be transferred. 
 * NOTE: You may need to convert characters that are valid file names on FAT32/ExFat to support the hosts native character encoding and file name limitations. 
 * NOTE: Currently the response is limited to 100 entries, Will return 
 * NOTE: BlueSCSI only transfers as many entries as are actually present. You should request the file count first, then size your receive buffer to match that number of entries.
 */
static int toolbox_listfiles(int dev, int print)
{
	char cmd[10] = {TOOLBOX_LIST_FILES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	char *buf;
	int i;
	int buf_size;
	int num_files;
	
	if (verbose)
		fprintf (stdout, "Listing files on %i\n", dev);

	num_files = toolbox_countfiles (dev);
	if (num_files < 0 || num_files > MAX_FILES)
	{
		fprintf (stderr, "Error: listfiles num_files invalid: %i", num_files);
		return -1;
	}
	files_count = num_files;
	if (verbose)
		fprintf (stdout, "Found %i files\n", num_files);
	buf_size = sizeof(ToolboxFileEntry) * num_files;
	
	buf = (char *)malloc(buf_size);
	if (buf == NULL)
	{
		fprintf (stderr, "Error: failed to malloc %i bytes: - %s\n", buf_size, strerror(errno));
		return -1;
	}

	memset(buf, 0, buf_size);
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, buf_size) != 0)
	{
		fprintf (stderr, "Error: listfiles failed - %s\n", strerror(errno));
		return -1;
	}
	/*Copy SCSI data to global files var	 */
	for (i = 0; i < num_files; i++) {
		memcpy(&files[i], buf + i * sizeof(ToolboxFileEntry), sizeof(ToolboxFileEntry));
		files[i].name[sizeof(files[i].name) - 1] = '\0';
	}
	if (verbose || print)
	{	
		for (i = 0;i < num_files;i++)
			fprintf (stdout, "#%i %s %li bytes\n", files[i].index, files[i].name, size_to_long(files[i].size));
	}
	return 0;
}
/*
 * TOOLBOX_GET_FILE 0xD1
 * Transfers a file from the ToolBoxSharedDir (Default /shared) to the Host computer.
 *
 * CDB[1] contains the index of the file to transfer.
 *
 * CDB[2..5] contains the offset in the file in 4096 byte blocks. Big endian.
 */

static int toolbox_getfile(int dev, int idx, char *outdir)
{
	char cmd[10];
	char buf[MAX_DATA_LEN];
	FILE *fd;
	char *filename;
	size_t bytes_written;
	size_t bytes_left;
	long int filesize;
	long int max_blocks;
	int blk_idx = 0;


	memset(cmd, 0, sizeof(cmd));
	cmd[0] = TOOLBOX_GET_FILE;
	cmd[1] = idx;
	cmd[2] = 0; /*Index offset in MAX_DATA_LEN blocks */

	if (strlen (outdir) < 2)/*Default to current directory */
		strcpy (outdir, "./");
	/*We need to populate the files struct first before doing anything else */
	if (toolbox_listfiles (dev, 0) != 0)
	{
		fprintf (stderr, "Error: getfile couldn't listfiles\n");
		return -1;
	}
	
	if (verbose)
		fprintf (stdout, "getfile :#%i %s %li bytes\n", files[idx].index, files[idx].name, size_to_long(files[idx].size));
	filename = malloc (strlen(outdir) + strlen(files[idx].name));
	if (filename == NULL)
	{
		fprintf (stderr, "Error mallocing filename\n");
		return -1;
	}

	fprintf (stdout, "Fetching %s\n", files[idx].name);
	strcpy (filename, outdir);
	strcat (filename, files[idx].name);
	if (verbose)
		fprintf (stdout, "Writing to file %s\n", filename);
	fd = fopen(filename, "wb");
	if (fd == NULL)
	{
		fprintf (stderr, "Error: getfile couldn't open %s\n", filename);
		return -1;
	}
	memset(buf, 0, sizeof(buf));
	bytes_written = 0;
	bytes_left = 0;

	/* Bound the loop so a wrong/garbage size can't spin forever: a file of
	 * filesize bytes needs at most ceil(filesize / MAX_DATA_LEN) blocks; allow
	 * one extra as slack. */
	filesize = size_to_long (files[idx].size);
	max_blocks = (filesize / MAX_DATA_LEN) + 2;

	/*Read the data from the SCSI bus and store to disk */
	while (1)
	{
		if ((long int)blk_idx > max_blocks)
		{
			fprintf (stderr, "Error: getfile transfer exceeded expected size (%ld bytes), aborting\n", filesize);
			fclose (fd);
			return -1;
		}
		if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, MAX_DATA_LEN) != 0)
		{
			fprintf (stderr, "Error: getfile failed during transfer - %s\n", strerror(errno));
			fclose (fd);
			return -1;
		}

		bytes_left = filesize - bytes_written;
		if (verbose)
			fprintf (stdout, "Bytes left: %li\n", bytes_left);
		if (bytes_left <= 0)
		{
			if (verbose)
				fprintf (stdout, "Transfer of %s complete\n", filename);
			break;
		}

		/*Check to see if we are on the last chunk */
		if (bytes_left < MAX_DATA_LEN)
		{
			bytes_written += fwrite (buf, sizeof(unsigned char), bytes_left, fd);
			break;
		}
		/*Otherwise write the chunk and move onto the next one */
		bytes_written += fwrite (buf, sizeof(unsigned char), MAX_DATA_LEN, fd);

		/*increment the offset */
		blk_idx++;
		cmd[2] = (unsigned char)(blk_idx >> 24) & 0xFF;
		cmd[3] = (unsigned char)(blk_idx >> 16) & 0xFF;
		cmd[4] = (unsigned char)(blk_idx >>  8) & 0xFF;
		cmd[5] = (unsigned char)(blk_idx      ) & 0xFF;
	}
	fclose (fd);
	return 0;
}


/** TOOLBOX_MODE_DEVICES (read, length 10)
 * Input:
 *  CDB 00 = command byte
 * Output:
 *  8 bytes, each indicating the device type of the emulated SCSI devices
 *           or 0xFF for not-enabled targets
 */
static int toolbox_listdevices(int dev, char **outbuf)
{
	char cmd[10] = {TOOLBOX_LIST_DEVICES, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	char buf[8];
	*outbuf = NULL;

	memset(buf, 0, sizeof(buf));
	if (scsi_send_command(dev, (unsigned char *)cmd, sizeof(cmd), (unsigned char *)buf, sizeof(buf)) != 0)
	{
		fprintf (stderr, "Error: listdevices failed - %s\n", strerror(errno));
		return -1;
	}
	*outbuf = (char *)calloc(sizeof(buf), sizeof(char));
	if (*outbuf) {
		memcpy(*outbuf, buf, sizeof(buf));
		return 0;
	}
	else
		return -1;
}

/* Human-readable name for a device-type byte from the 0xD9 map. Masked to a
 * byte so a sign-extended 0xFF (TYPE_NONE) still matches on signed-char hosts. */
static const char *dev_type_name(int t)
{
	switch (t & 0xFF)
	{
	case TYPE_HDD:        return "HDD";
	case TYPE_REMOVABLE:  return "Removable";
	case TYPE_CD:         return "CD";
	case TYPE_FLOPPY:     return "Floppy";
	case TYPE_MO:         return "Magneto-optical";
	case TYPE_SEQUENTIAL: return "Tape";
	case TYPE_NETWORK:    return "Network";
	case TYPE_ZIP100:     return "Zip100";
	case TYPE_NONE:       return "not enabled";
	default:              return "unknown";
	}
}

/*
 * Print the 8-byte device-type map (0xD9) as a readable table of the emulated
 * SCSI targets, one line per ID 0-7. Works on any accepted toolbox target and
 * does not require the target to be a CD.
 */
static int toolbox_printdevices(int dev)
{
	char *dev_flags;
	int i;

	if (toolbox_listdevices(dev, &dev_flags) != 0)
	{
		fprintf (stderr, "Error: couldn't fetch device-type map (0xD9): %s\n", strerror(errno));
		free(dev_flags);
		return -1;
	}

	fprintf (stdout, "Emulated SCSI targets:\n");
	for (i = 0; i < 8; i++)
		fprintf (stdout, "  ID %i: %s\n", i, dev_type_name((unsigned char)dev_flags[i]));

	free(dev_flags);
	return 0;
}

/*
 * FIRMWARE names we recognise as advertising the toolbox API. Both BlueSCSI and
 * ZuluSCSI stamp their name into the vendor-specific area of INQUIRY starting at
 * byte 36 (which our product_rev field spans), and BlueSCSI also stamps it into
 * the MODE SENSE page 0x31 vendor page.
 *
 * These are deliberately FIRMWARE identifiers, never emulated-device model
 * names: a model name like "IRIS EMUL DISK" says what a device pretends to be,
 * not whether it implements commands 0xD0-0xDA. Self-identification alone is
 * never enough here either - toolbox_confirm() below has to get a real answer
 * out of the device before we treat it as toolbox-capable.
 */
static const char *toolbox_firmware_ids[] = { "BlueSCSI", "ZuluSCSI" };
#define N_ACCEPT_IDS (int)(sizeof(toolbox_firmware_ids) / sizeof(toolbox_firmware_ids[0]))

/* Substring search that tolerates embedded NULs (MODE SENSE data and the
 * vendor page both contain 0x00 bytes, so strstr() would stop early). */
static int buf_contains(const unsigned char *hay, int haylen, const char *needle)
{
	int n = (int)strlen(needle);
	int i;

	if (n == 0)
		return 1;
	for (i = 0; i + n <= haylen; i++)
		if (memcmp(hay + i, needle, n) == 0)
			return 1;
	return 0;
}

/*
 * Authoritative toolbox-capability check: MODE SENSE(6) for the BlueSCSI
 * vendor page 0x31 and look for an accepted magic string. This is how the IRIS
 * emulator (whose INQUIRY is a plain SGI disk) advertises toolbox support, and
 * how a real BlueSCSI advertises it via the "BlueSCSIVendorPage".
 * With probe non-zero the command is sent quietly and without retries (bus
 * scanning, where most targets legitimately reject this page).
 * Returns 0 if an accepted id is present in the page, -1 otherwise.
 */
static int toolbox_modesense_page31(int dev, int probe)
{
	unsigned char cmd[6];
	unsigned char buf[96];
	int ret;
	int i;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_MODE_SENSE_6;
	cmd[1] = 0x08;                  /* DBD: suppress block descriptors */
	cmd[2] = TOOLBOX_VENDOR_PAGE;  /* PC=current, page code 0x31 */
	cmd[4] = sizeof(buf);           /* allocation length */

	memset(buf, 0, sizeof(buf));
	if (probe)
		ret = scsi_send_command_probe(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	else
		ret = scsi_send_command(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	if (ret != 0)
		return -1;              /* page unsupported or command failed */

	for (i = 0; i < N_ACCEPT_IDS; i++)
		if (buf_contains(buf, sizeof(buf), toolbox_firmware_ids[i]))
			return 0;
	return -1;
}

/*
 * Send INQUIRY (0x12) and parse the reply into inq. If raw is non-NULL the
 * unparsed reply is copied there too (the toolbox API-version byte is read from
 * it). With probe non-zero the command is sent quietly and without retries.
 * Prints nothing itself. Returns 0 on success, -1 on failure.
 */
static int scsi_read_inquiry(int dev, scsi_inquiry *inq, unsigned char *raw, int rawlen, int probe)
{
	unsigned char cmd[6];
	unsigned char buf[sizeof(scsi_inquiry)];
	int ret;
	int n;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_INQUIRY;
	cmd[4] = sizeof(scsi_inquiry);  /* allocation length (66) */

	memset(buf, 0, sizeof(buf));
	if (probe)
		ret = scsi_send_command_probe(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	else
		ret = scsi_send_command(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	if (ret != 0)
		return -1;

	memset(inq, 0, sizeof(scsi_inquiry));
	inq->dev_type   = buf[0];
	inq->version    = buf[2];
	inq->add_length = buf[4];
	memcpy(inq->vendor_id, &buf[8], sizeof(inq->vendor_id) - 1);
	inq->vendor_id[sizeof(inq->vendor_id) - 1] = '\0';
	memcpy(inq->product_id, &buf[16], sizeof(inq->product_id) - 1);
	inq->product_id[sizeof(inq->product_id) - 1] = '\0';
	memcpy(inq->product_rev, &buf[32], sizeof(inq->product_rev) - 1);
	inq->product_rev[sizeof(inq->product_rev) - 1] = '\0';

	if (raw != NULL && rawlen > 0) {
		n = rawlen < (int)sizeof(buf) ? rawlen : (int)sizeof(buf);
		memcpy(raw, buf, n);
	}
	return 0;
}

/*
 * Append src to out with src's trailing spaces dropped. INQUIRY fields are
 * space-padded to their full width, which would otherwise leave ragged gaps in
 * the middle of the identity string.
 */
static void append_trimmed(char *out, int outlen, const char *src)
{
	int len = (int)strlen(src);
	int used = (int)strlen(out);
	int i;

	while (len > 0 && src[len - 1] == ' ')
		len--;
	for (i = 0; i < len && used < outlen - 1; i++)
		out[used++] = src[i];
	out[used] = '\0';
}

/* Build the "<vendor> <product> <rev>" string matched against the accept list. */
static void inquiry_identity(const scsi_inquiry *inq, char *out, int outlen)
{
	if (outlen <= 0)
		return;
	out[0] = '\0';
	append_trimmed(out, outlen, inq->vendor_id);
	if ((int)strlen(out) < outlen - 1)
		strcat(out, " ");
	append_trimmed(out, outlen, inq->product_id);
	if ((int)strlen(out) < outlen - 1)
		strcat(out, " ");
	append_trimmed(out, outlen, inq->product_rev);
}

/*
 * Does this INQUIRY identity carry a known toolbox FIRMWARE name (byte 36+)?
 * Returns the matching entry from toolbox_firmware_ids[], or NULL.
 */
static const char *identity_accepted(const char *identity)
{
	int i;

	for (i = 0; i < N_ACCEPT_IDS; i++)
		if (strstr(identity, toolbox_firmware_ids[i]) != NULL)
			return toolbox_firmware_ids[i];
	return NULL;
}

/*
 * Functional proof that a device really implements the toolbox: issue
 * TOOLBOX_LIST_DEVICES (0xD9) and require a plausible 8-byte device-type map
 * back. Anything that merely *claims* toolbox support in its INQUIRY string
 * still has to answer this - which is what stops a device that only emulates a
 * disk (e.g. an emulator presenting a plain SGI drive) from being reported as
 * toolbox-capable when it does not implement 0xD0-0xDA at all.
 *
 * A valid map holds only real device-type codes (0x00-0x07) or 0xFF for
 * "target not enabled", and must enable at least one target - random data or a
 * stream of zeros from a device that ignored the opcode fails that test.
 *
 * On success the map is copied into device_list[] (used to gate CD operations).
 * With probe non-zero the command is sent quietly and without retries.
 * Returns 0 if confirmed, -1 otherwise.
 */
static int toolbox_confirm(int dev, int probe)
{
	unsigned char cmd[10];
	unsigned char buf[8];
	int ret;
	int i;
	int enabled = 0;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = TOOLBOX_LIST_DEVICES;

	memset(buf, 0, sizeof(buf));
	if (probe)
		ret = scsi_send_command_probe(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	else
		ret = scsi_send_command(dev, cmd, sizeof(cmd), buf, sizeof(buf));
	if (ret != 0)
		return -1;

	for (i = 0; i < 8; i++) {
		if (buf[i] == TYPE_NONE)
			continue;
		if (buf[i] > TOOLBOX_DEVTYPE_MAX)
			return -1;      /* not a device-type map at all */
		enabled++;
	}
	if (enabled == 0)
		return -1;              /* nothing enabled: not a real answer */

	for (i = 0; i < 8; i++)
		device_list[i] = buf[i];
	return 0;
}

/* SCSI peripheral device type, INQUIRY byte 0 bits 0-4 (not the 0xD9 map). */
static const char *inquiry_pdt_name(unsigned char b0)
{
	switch (b0 & 0x1F)
	{
	case 0x00: return "Disk";
	case 0x01: return "Tape";
	case 0x02: return "Printer";
	case 0x03: return "Proc";
	case 0x04: return "WORM";
	case 0x05: return "CD-ROM";
	case 0x06: return "Scanner";
	case 0x07: return "Optical";
	case 0x08: return "Changer";
	case 0x09: return "Comms";
	case 0x1F: return "Unknown";
	default:   return "Other";
	}
}

/* Interrogate the device and find out its capabilities */
static int toolbox_inquiry(int dev, int print)
{
	unsigned char buf[sizeof(scsi_inquiry)];
	char identity[64];
	const char *match;
	int accepted = 0;
	scsi_inquiry inq;
	int i;
	int additional_len;
	int total_len;
	int toolbox_api_version;

	if (scsi_read_inquiry(dev, &inq, buf, sizeof(buf), 0) != 0)
	{
		fprintf (stderr, "Error: inquiry command failed - %s\n", strerror(errno));
		return 1;
	}

	if (verbose || print)
	{
		fprintf (stdout, "SCSI version: %i\n", inq.version);
		fprintf (stdout, "vendor_id: %s \nproduct_id: %s\n", inq.vendor_id, inq.product_id);
		fprintf (stdout, "product_rev: %s\n", inq.product_rev);
		fprintf (stdout, "debug mode: %i\n", toolbox_getdebug(dev));
	}
	/* Print the Toolbox API version if the extended data is present */
	additional_len = buf[4]; /*offset 4 contains how much extra data is in the packet */
	total_len = additional_len + 5;

	if (total_len <= (int)sizeof(buf)) {
		toolbox_api_version = buf[total_len - 1];
		if (verbose)
			fprintf(stdout, "Toolbox API version: %u\n", toolbox_api_version);

		if (toolbox_api_version < TOOLBOX_API_VER) {
			fprintf(stdout, "Toolbox API version %u too old, expecting: %u\n", toolbox_api_version, TOOLBOX_API_VER);
			/*return -1; */
		}

	} else {
		fprintf(stdout, "Toolbox API version: not available (length mismatch)\n");
	}

	/*
	 * Decide whether this is a toolbox-capable target. Two stages:
	 *
	 * 1. SELF-IDENTIFICATION - the device claims toolbox support, either by
	 *    carrying a known firmware name ("BlueSCSI"/"ZuluSCSI", stamped from
	 *    INQUIRY byte 36 which our product_rev field spans) or by returning the
	 *    MODE SENSE page 0x31 vendor page. The INQUIRY check runs first so a
	 *    recognised device never has to issue the page 0x31 probe.
	 *    -F skips this stage for firmware we don't know by name yet.
	 *
	 * 2. CONFIRMATION - the device actually ANSWERS a toolbox command (0xD9)
	 *    with a valid device-type map. A claim alone is not enough: this is what
	 *    keeps a device that only emulates a disk from being driven as a
	 *    toolbox target when it does not implement 0xD0-0xDA.
	 */
	inquiry_identity(&inq, identity, sizeof(identity));

	if (force_toolbox) {
		accepted = 1;
		if (verbose)
			fprintf(stdout, "Skipping identity check (-F): testing 0xD9 directly\n");
	} else {
		match = identity_accepted(identity);
		if (match != NULL) {
			accepted = 1;
			if (verbose)
				fprintf(stdout, "Claims toolbox via INQUIRY firmware id '%s'\n", match);
		}

		if (!accepted && toolbox_modesense_page31(dev, 0) == 0) {
			accepted = 1;
			if (verbose)
				fprintf(stdout, "Claims toolbox via MODE SENSE page 0x31 magic\n");
		}
	}

	if (!accepted) {
		fprintf(stderr, "Error: '%s' does not advertise the toolbox API.\n", identity);
		fprintf(stderr, "Known toolbox firmware: BlueSCSI, ZuluSCSI (toolbox must be enabled).\n");
		fprintf(stderr, "If you believe this device supports it, retry with -F to test it directly.\n");
		return 1;
	}

	/*
	 * Confirm the claim by getting a real answer out of the device. This also
	 * populates device_list[], used to gate CD operations.
	 */
	if (toolbox_confirm(dev, 0) != 0) {
		fprintf(stderr, "Error: '%s' did not answer TOOLBOX_LIST_DEVICES (0xD9).\n", identity);
		if (force_toolbox)
			fprintf(stderr, "Tested directly because of -F; this device does not implement the toolbox.\n");
		else
			fprintf(stderr, "It advertises the toolbox but does not implement it - refusing to continue.\n");
		return 1;
	}

	if (verbose) {
		fprintf (stdout, "Confirmed by 0xD9. Device flags: ");
		for (i = 0; i < 8; i++)
			fprintf (stdout, "%02x ", (unsigned char)device_list[i]);
		fprintf (stdout, "\n");
	}

	return 0;
}

/*
 * Scan every generic SCSI device node this host exposes, send INQUIRY to each,
 * and print whatever answers - flagging the toolbox-capable ones. Unlike every
 * other operation this needs NO device path: it is how you FIND the path to
 * pass to the others.
 *
 * Probing is deliberately quiet and retry-free (scsi_send_command_probe): most
 * nodes on a real bus will not answer, and the normal retry-with-warnings path
 * would make a scan slow and noisy. INQUIRY and MODE SENSE are both read-only,
 * so scanning cannot disturb an attached disk.
 *
 * Returns the number of toolbox-capable targets found, or -1 on error.
 */
static int toolbox_scanbus(void)
{
	char paths[MAX_SCAN_DEVICES][SCSI_PATH_MAX];
	scsi_inquiry inq;
	char identity[64];
	int count, i, dev;
	int toolbox;
	int claims;
	int answered = 0;
	int found = 0;
	int unconfirmed = 0;

	count = scsi_enum_devices(paths, MAX_SCAN_DEVICES);
	if (count < 0)
	{
		fprintf (stderr, "Error: could not enumerate SCSI devices\n");
		return -1;
	}
	if (count == 0)
	{
		fprintf (stderr, "No generic SCSI device nodes found.\n");
#if defined(OS_IRIX)
		fprintf (stderr, "Expected nodes like /dev/scsi/sc0d1l0 - check 'ls /dev/scsi'.\n");
#elif defined(OS_LINUX)
		fprintf (stderr, "Expected nodes like /dev/sg0 - is the sg module loaded?\n");
#endif
		return 0;
	}

	fprintf (stdout, "Scanning %i SCSI device node(s)...\n\n", count);
	fprintf (stdout, "%-22s %-8s %s\n", "DEVICE", "TYPE", "IDENTITY");

	for (i = 0; i < count; i++)
	{
		/* Read-only open: INQUIRY/MODE SENSE never modify the target. */
		dev = scsi_open(paths[i], 1);
		if (dev < 0)
			continue;

		if (scsi_read_inquiry(dev, &inq, NULL, 0, 1) == 0)
		{
			answered++;
			inquiry_identity(&inq, identity, sizeof(identity));

			/* Claim first (INQUIRY firmware name, else page 0x31), then
			 * prove it by getting a real 0xD9 answer. A vendor opcode is
			 * only ever sent to a device that already claimed support -
			 * or to everything, if the user asked for -F. */
			claims = force_toolbox ||
				 (identity_accepted(identity) != NULL) ||
				 (toolbox_modesense_page31(dev, 1) == 0);
			toolbox = claims && (toolbox_confirm(dev, 1) == 0);

			if (toolbox)
				found++;
			else if (claims)
				unconfirmed++;

			fprintf (stdout, "%-22s %-8s %s%s\n", paths[i],
				inquiry_pdt_name(inq.dev_type), identity,
				toolbox ? "  [TOOLBOX]" :
					(claims ? "  [claims toolbox, no 0xD9 answer]" : ""));
		}
		scsi_close(dev);
	}

	fprintf (stdout, "\n%i device(s) answered, %i toolbox-capable", answered, found);
	if (unconfirmed > 0)
		fprintf (stdout, " (%i claimed toolbox but failed 0xD9)", unconfirmed);
	fprintf (stdout, ".\n");

	if (found > 0)
		fprintf (stdout, "Pass one of the [TOOLBOX] paths to the other options, e.g. -i <device>\n");
	else if (answered > 0 && !force_toolbox)
		fprintf (stdout, "No toolbox target found. Check toolbox mode is enabled on the device,\n"
				 "or re-scan with -F to test every device by issuing 0xD9 directly.\n");
	return found;
}

static void do_drive(char *path, int list, int verbose, int cd_img, int file, char *outdir)
{
	int dev;
	int dev_scsi_id; /* SCSI ID pulled from path */
	int readonly; /* Needed to determine if it's a CDROM and only able to be opened READONLY */
	readonly = 0;
	
	/* CD targets are emulated read-only, so list-CDs and change-CD must open
	 * read-only; everything else (shared dir, inquiry, debug) opens read-write
	 * and falls back to read-only below if the open is refused. */
	if (list == MODE_CD || cd_img != NOT_ACTIVE)
	       readonly = 1;

	dev = scsi_open(path, readonly);
	if (dev < 0) {
		if (!readonly)
		{
			fprintf (stderr, "Error opening device for read/write, trying to open readonly\n");
			dev = scsi_open(path, 1); /*Try to open the device as read only */
			
		}
		if (dev < 0) {
			fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
			/* Only suggest root if that is plausibly the problem. */
			if (!running_as_root())
				fprintf(stderr, "You are not root - that is almost certainly why. Re-run as root.\n");
			exit(1);
		}
	}

	/*Do inquiry to check we are working with a BlueSCSI */
	/*device_type = toolbox_inquiry (dev, PRINT_OFF); */
	if (toolbox_inquiry (dev, PRINT_OFF) != 0)
	{
		fprintf (stderr, "No usable toolbox device at %s\n", path);
		scsi_close (dev);
		exit(1);
	}
	
	/* Only the CD operations (-l / -c) need the SCSI target id parsed out of the
	 * path to index device_list[]. Everything else (-i, -t, -D, -s, -g, -p) talks
	 * to the already-open device directly, so a path that opened fine but doesn't
	 * match the strict /dev/scsi/scNdNlN form must NOT block them. */
	dev_scsi_id = path_to_devnum(path);

	if (list == MODE_CD)
	{
		if (dev_scsi_id < 0)
		{
			fprintf (stderr, "Cannot list CDs: couldn't read the SCSI target id from '%s'\n", path);
			scsi_close(dev);
			exit(1);
		}
		if (device_list[dev_scsi_id] != TYPE_CD)
		{
			fprintf (stderr, "Tried to list CDs, but an emulated CD drive wasn't detected\n");
			scsi_close(dev);
			exit(1);
		}
		toolbox_listcds(dev);
	}
	else if (list == MODE_INQUIRY)
		toolbox_inquiry(dev, PRINT_ON);
	else if (list == MODE_DEVICES)
		toolbox_printdevices(dev);
	else if (list == MODE_DEBUG)
		toolbox_setdebug(dev, file);
	else if (list == MODE_DEBUG_GET)
	{
		int dbg = toolbox_getdebug(dev);
		if (dbg >= 0)
			fprintf (stdout, "Debug mode: %s\n", dbg ? "on" : "off");
	}
	else if (list == MODE_SHARED)
		toolbox_listfiles(dev, PRINT_ON);
	else if (list == MODE_PUT)
		toolbox_sendfile (dev, outdir);
	else if (file != NOT_ACTIVE)
		toolbox_getfile (dev, file, outdir);
	else if (cd_img != NOT_ACTIVE)
	{
		if (dev_scsi_id < 0)
			fprintf (stderr, "Cannot switch CD: couldn't read the SCSI target id from '%s'\n", path);
		else if (device_list[dev_scsi_id] != TYPE_CD)
			fprintf (stderr, "Device doesn't seem to be a CD drive? Detected type %i on SCSI ID %i\n", device_list[dev_scsi_id], dev_scsi_id);
		else
			toolbox_setnextcd(dev, cd_img);
	}
	else
		fprintf (stderr, "No operation requested for %s. Try -i, -t, -s, or -h for help.\n", path);

	scsi_close(dev);
}

static void usage(void)
{
	fprintf(stderr, "\nUsage:   irixscsitb [options] [device]\n\n");
#if defined(OS_IRIX)
	fprintf(stderr, "example: irixscsitb -s /dev/scsi/sc0d1l0\n\n");
#elif defined(OS_LINUX)
	fprintf(stderr, "example: irixscsitb -s /dev/sg2\n\n");
#endif
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t-h      : display this help message and exit\n");
	fprintf(stderr, "\t-v      : be verbose\n");
	fprintf(stderr, "\t-b      : scan the SCSI bus and list every device (NO device path needed)\n");
	fprintf(stderr, "\t-i      : interrogate the device and report firmware/toolbox info\n");
	fprintf(stderr, "\t-t      : list emulated SCSI targets (device map)\n");
	fprintf(stderr, "\t-l      : list available CDs\n");
	fprintf(stderr, "\t-s      : List /shared directory\n");
	fprintf(stderr, "\t-c num  : change to CD number (1, 2, etc)\n");
	fprintf(stderr, "\t-g num  : get file from shared directory (1, 2, etc)\n");
	fprintf(stderr, "\t-p file : put file to shared directory\n");
	fprintf(stderr, "\t-o dir  : set output directory, defaults to current\n");
	fprintf(stderr, "\t-d num  : set debug mode (0 = off, 1 = on)\n");
	fprintf(stderr, "\t-D      : show current debug mode\n");
	fprintf(stderr, "\t-F      : skip the identity check; test the device with a real toolbox command\n");
	/* Only nag about root when we actually aren't root. */
	if (!running_as_root())
		fprintf(stderr, "\nNOTE: you are not running as root - opening the SCSI device will most\n"
				"likely fail. Re-run this as root (or via su).\n");
}

int main(int argc, char *argv[])
{
	int c, cdimg = NOT_ACTIVE, list = 0, file = NOT_ACTIVE;
	char outdir[1024];

	while ((c = getopt(argc, argv, "hvlsitbDFc:d:g:o:p:")) != -1) switch (c) {
		case 'c':
			cdimg = atoi(optarg);
			break;
		case 'g':
			file = atoi(optarg);
			break;
		case 'o':
			strncpy(outdir, optarg, sizeof(outdir) - 1);
			break;
		case 'p':
			strncpy(outdir, optarg, sizeof(outdir) - 1);
			list = MODE_PUT;
			break;
		case 'l':
			list = MODE_CD;
			break;
		case 's':
			list = MODE_SHARED;
			break;
		case 'i':
			list = MODE_INQUIRY;
			break;
		case 't':
			list = MODE_DEVICES;
			break;
		case 'b':
			list = MODE_SCAN;
			break;
		case 'F':
			force_toolbox = 1;
			break;
		case 'D':
			list = MODE_DEBUG_GET;
			break;
		case 'd':
			list = MODE_DEBUG;
			file = atoi(optarg);
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
		default:
			usage();
			return 1;
	}
	
	argc -= optind;
	argv += optind;

	/* -b scans the bus to FIND devices, so it is the one mode that takes no
	 * device path. Handle it before the path check below. */
	if (list == MODE_SCAN)
		return toolbox_scanbus() < 0 ? 1 : 0;

	/*Stop any removable media managers running on the host system before changing CDs */
	if (cdimg != -1)
		mediad_stop ();

	if (argc < 1) {
		fprintf (stderr, "Error: no device path given.\n");
		fprintf (stderr, "A target device path is required for every operation except -b.\n");
		fprintf (stderr, "Run 'irixscsitb -b' to scan the bus and list the available device paths.\n");
#if defined(OS_IRIX)
		fprintf (stderr, "(They look like /dev/scsi/sc0d1l0; 'ls /dev/scsi' shows them too.)\n");
#elif defined(OS_LINUX)
		fprintf (stderr, "(They look like /dev/sg2; 'lsscsi -g' shows them too.)\n");
#endif
		usage();
		return 1;
	} else if (argc > 1) {
		fprintf(stderr, "WARNING: extra arguments after '%s' ignored - put options BEFORE the device path.\n", argv[0]);
	}
	/*strcpy (device_path, argv[0]); //Copy the path for later */
	do_drive(argv[0], list, verbose, cdimg, file, outdir);
	
	if (cdimg != -1)
		mediad_start ();
	
	return 0;
}
