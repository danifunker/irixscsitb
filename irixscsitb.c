/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Command-line front end: argument parsing, and turning the results that
 * toolbox.c returns into text on stdout. All protocol work - the 0xD0-0xDA
 * command builders, capability detection, the bus probe - lives in toolbox.c,
 * which the Motif GUI (gui_motif.c) links against too. Nothing in this file
 * speaks SCSI directly.
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

/* Print one ToolboxFileEntry array as "#N name size" lines. */
static void print_entries(const ToolboxFileEntry *entries, int n, int mark_dirs)
{
	int i;

	for (i = 0; i < n; i++)
		fprintf (stdout, "#%i %s%s %li bytes\n", entries[i].index, entries[i].name,
			 (mark_dirs && entries[i].type == 1) ? "/" : "",
			 size_to_long(entries[i].size));
}

/* -s: list the device's /shared directory. */
static int cli_listfiles(int dev)
{
	ToolboxFileEntry entries[MAX_FILES];
	int n = toolbox_listfiles(dev, entries, MAX_FILES);

	if (n < 0)
		return -1;
	print_entries(entries, n, 0);
	return 0;
}

/* -l: list the CD images available to the selected target. */
static int cli_listcds(int dev)
{
	ToolboxFileEntry entries[MAX_FILES];
	int n = toolbox_listcds(dev, entries, MAX_FILES);

	if (n < 0)
		return -1;
	fprintf (stdout, "Found %i CDs\n", n);
	print_entries(entries, n, 1);
	return 0;
}

/*
 * -t: print the 8-byte device-type map (0xD9) as a readable table of the
 * emulated SCSI targets, one line per ID 0-7. Works on any accepted toolbox
 * target and does not require the target to be a CD.
 */
static int cli_printdevices(int dev)
{
	unsigned char map[8];
	int i;

	if (toolbox_listdevices(dev, map) != 0)
	{
		fprintf (stderr, "Error: couldn't fetch device-type map (0xD9): %s\n", strerror(errno));
		return -1;
	}

	fprintf (stdout, "Emulated SCSI targets:\n");
	for (i = 0; i < 8; i++)
		fprintf (stdout, "  ID %i: %s\n", i, dev_type_name(map[i]));

	return 0;
}

/*
 * Explain a failed qualification. Kept next to the CLI rather than in the core
 * so the GUI can put the same distinctions in a dialog instead of on stderr.
 */
static void cli_report_rejection(int ret, const ToolboxDetect *det)
{
	if (ret == TOOLBOX_ERR_NO_CLAIM)
	{
		fprintf(stderr, "Error: '%s' does not advertise the toolbox API.\n", det->identity);
		fprintf(stderr, "Known toolbox firmware: BlueSCSI, ZuluSCSI (toolbox must be enabled).\n");
		fprintf(stderr, "If you believe this device supports it, retry with -F to test it directly.\n");
	}
	else if (ret == TOOLBOX_ERR_NO_ANSWER)
	{
		fprintf(stderr, "Error: '%s' did not answer TOOLBOX_LIST_DEVICES (0xD9).\n", det->identity);
		if (force_toolbox)
			fprintf(stderr, "Tested directly because of -F; this device does not implement the toolbox.\n");
		else
			fprintf(stderr, "It advertises the toolbox but does not implement it - refusing to continue.\n");
	}
}

/*
 * Interrogate the device and report what it is. With print set this is -i;
 * with it clear this is the capability gate every other operation runs first.
 */
static int cli_inquiry(int dev, int print)
{
	ToolboxDetect det;
	int ret;
	int i;

	if (toolbox_identify(dev, &det) != TOOLBOX_OK)
	{
		fprintf (stderr, "Error: inquiry command failed - %s\n", strerror(errno));
		return 1;
	}

	if (verbose || print)
	{
		fprintf (stdout, "SCSI version: %i\n", det.inq.version);
		fprintf (stdout, "vendor_id: %s \nproduct_id: %s\n", det.inq.vendor_id, det.inq.product_id);
		fprintf (stdout, "product_rev: %s\n", det.inq.product_rev);
		fprintf (stdout, "debug mode: %i\n", toolbox_getdebug(dev));
	}

	if (det.api_version >= 0)
	{
		if (verbose)
			fprintf(stdout, "Toolbox API version: %u\n", det.api_version);

		if (det.api_version < TOOLBOX_API_VER)
			fprintf(stdout, "Toolbox API version %u too old, expecting: %u\n",
				det.api_version, TOOLBOX_API_VER);
	}
	else
		fprintf(stdout, "Toolbox API version: not available (length mismatch)\n");

	ret = toolbox_qualify(dev, &det);
	if (ret != TOOLBOX_OK)
	{
		cli_report_rejection(ret, &det);
		return 1;
	}

	if (verbose) {
		fprintf (stdout, "Confirmed by 0xD9. Device flags: ");
		for (i = 0; i < 8; i++)
			fprintf (stdout, "%02x ", det.devmap[i]);
		fprintf (stdout, "\n");
	}

	return 0;
}

/*
 * -b: scan every generic SCSI device node this host exposes and print whatever
 * answers, flagging the toolbox-capable ones. Unlike every other operation this
 * needs NO device path: it is how you FIND the path to pass to the others.
 *
 * Returns the number of toolbox-capable targets found, or -1 on error.
 */
static int cli_scanbus(void)
{
	char paths[MAX_SCAN_DEVICES][SCSI_PATH_MAX];
	ToolboxScanEntry e;
	int count, i;
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
		if (toolbox_probe(paths[i], &e) != 0)
			continue;

		answered++;
		if (e.confirmed)
			found++;
		else if (e.claims)
			unconfirmed++;

		fprintf (stdout, "%-22s %-8s %s%s\n", e.path, e.type_name, e.identity,
			e.confirmed ? "  [TOOLBOX]" :
				(e.claims ? "  [claims toolbox, no 0xD9 answer]" : ""));
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

	/*Check we are talking to something that really implements the toolbox */
	if (cli_inquiry (dev, PRINT_OFF) != 0)
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
		cli_listcds(dev);
	}
	else if (list == MODE_INQUIRY)
		cli_inquiry(dev, PRINT_ON);
	else if (list == MODE_DEVICES)
		cli_printdevices(dev);
	else if (list == MODE_DEBUG)
		toolbox_setdebug(dev, file);
	else if (list == MODE_DEBUG_GET)
	{
		int dbg = toolbox_getdebug(dev);
		if (dbg >= 0)
			fprintf (stdout, "Debug mode: %s\n", dbg ? "on" : "off");
	}
	else if (list == MODE_SHARED)
		cli_listfiles(dev);
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

/*
 * Build identification. Printed by -version / -V, and mirrored in the GUI's
 * Help > About so a binary found on a disk somewhere can always be traced back
 * to the commit and machine that produced it.
 */
static void print_version(void)
{
	fprintf(stdout, "irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)\n\n");
	fprintf(stdout, "Revision:   %s\n", build_revision());
	fprintf(stdout, "Stamped:    %s\n", build_stamp());
	fprintf(stdout, "Compiled:   %s\n", build_compiled());
	fprintf(stdout, "Target:     %s\n", build_abi());
	fprintf(stdout, "Built on:   %s\n", build_libs());
	fprintf(stdout, "Project:    %s\n", PROJECT_URL);
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
	fprintf(stderr, "\t-V      : show build revision/date and exit (also -version)\n");
	/* Only nag about root when we actually aren't root. */
	if (!running_as_root())
		fprintf(stderr, "\nNOTE: you are not running as root - opening the SCSI device will most\n"
				"likely fail. Re-run this as root (or via su).\n");
}

int main(int argc, char *argv[])
{
	int c, cdimg = NOT_ACTIVE, list = 0, file = NOT_ACTIVE;
	char outdir[1024];

	/* Must start empty: without -o or -p nothing else writes to it, and
	 * toolbox_getfile() reads it to decide whether to default to "./". */
	outdir[0] = '\0';

	/*
	 * getopt() only understands single-character flags, so a literal
	 * "-version" would be shredded into -v -e -r -s -i -o -n (and fail on
	 * the -e). Catch the long spellings before getopt ever sees them; -V is
	 * the short form and goes through getopt normally.
	 */
	for (c = 1; c < argc; c++) {
		if (strcmp(argv[c], "-version") == 0 || strcmp(argv[c], "--version") == 0) {
			print_version();
			return 0;
		}
	}

	while ((c = getopt(argc, argv, "hvVlsitbDFc:d:g:o:p:")) != -1) switch (c) {
		case 'c':
			cdimg = atoi(optarg);
			break;
		case 'g':
			file = atoi(optarg);
			break;
		case 'o':
			strncpy(outdir, optarg, sizeof(outdir) - 1);
			outdir[sizeof(outdir) - 1] = '\0';
			break;
		case 'p':
			strncpy(outdir, optarg, sizeof(outdir) - 1);
			outdir[sizeof(outdir) - 1] = '\0';
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
		case 'V':
			print_version();
			return 0;
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
		return cli_scanbus() < 0 ? 1 : 0;

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
	do_drive(argv[0], list, verbose, cdimg, file, outdir);

	if (cdimg != -1)
		mediad_start ();

	return 0;
}
