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

/* ------------------------------------------------------------------ *
 * Wi-Fi (-w / -W / -j)
 * ------------------------------------------------------------------ */

/*
 * One network, as two lines. Signal is given both ways on purpose: the dBm is
 * the number you compare between rows, the bar count is the one you can read at
 * a glance without knowing that -30 beats -80.
 */
static void print_wifi_network(const ToolboxWifiNetwork *net, int number)
{
	char bssid[18];
	char bars[6];
	int n, i;

	wifi_bssid_str(net->bssid, bssid, sizeof(bssid));

	n = wifi_signal_bars(net->rssi);
	for (i = 0; i < 4; i++)
		bars[i] = (i < n) ? '#' : '.';
	bars[4] = '\0';

	if (number > 0)
		fprintf(stdout, "#%-2i %s\n", number, net->ssid);
	else
		fprintf(stdout, "    %s\n", net->ssid);

	fprintf(stdout, "    %s  %i dBm   channel %-3i  %-8s  %s\n",
		bars, net->rssi, net->channel, wifi_auth_name(net->flags), bssid);
}

/* -w: scan for networks and list what came back. */
static int cli_wifi_scan(int dev)
{
	ToolboxWifiNetwork nets[WIFI_MAX_NETWORKS];
	int n, i;

	fprintf(stdout, "Scanning for Wi-Fi networks (this takes a few seconds)...\n");
	if (toolbox_wifi_scan(dev, WIFI_SCAN_TIMEOUT_SEC) != 0)
		return -1;

	n = toolbox_wifi_results(dev, nets, WIFI_MAX_NETWORKS);
	if (n < 0)
		return -1;

	if (n == 0) {
		fprintf(stdout, "No Wi-Fi networks found.\n");
		return 0;
	}

	fprintf(stdout, "\nFound %i network(s):\n\n", n);
	for (i = 0; i < n; i++) {
		print_wifi_network(&nets[i], i + 1);
		fprintf(stdout, "\n");
	}
	fprintf(stdout, "Join one with:  irixscsitb -j '<name>' -k '<password>'\n");
	return 0;
}

/* -W: what the radio is joined to right now. */
static int cli_wifi_info(int dev)
{
	ToolboxWifiNetwork net;

	if (toolbox_wifi_info(dev, &net) != 0)
		return -1;

	if (net.ssid[0] == '\0') {
		fprintf(stdout, "Not joined to any Wi-Fi network.\n");
		fprintf(stdout, "Scan with -w, then join with -j '<name>' -k '<password>'.\n");
		return 0;
	}

	fprintf(stdout, "Current Wi-Fi network:\n\n");
	print_wifi_network(&net, 0);
	return 0;
}

/* -j: join a network, then report what actually happened. */
static int cli_wifi_join(int dev, const char *ssid, const char *key, int channel)
{
	ToolboxWifiNetwork net;

	if (toolbox_wifi_join(dev, ssid, key, channel) != 0)
		return -1;

	fprintf(stdout, "Join request for '%s' sent.\n", ssid);

	/*
	 * The firmware acknowledges the REQUEST, not the association - it hands
	 * the credentials to the radio and answers GOOD immediately. So the only
	 * way to tell the operator whether it worked is to associate-and-ask,
	 * which takes a few seconds on real hardware.
	 */
	fprintf(stdout, "Waiting for the radio to associate...\n");
	sleep(5);

	if (toolbox_wifi_info(dev, &net) != 0)
		return -1;

	if (net.ssid[0] == '\0') {
		fprintf(stdout, "Not associated yet. The device reports no network.\n");
		fprintf(stdout, "Check the password and re-run with -W in a few seconds.\n");
		return 1;
	}
	if (strcmp(net.ssid, ssid) != 0) {
		fprintf(stdout, "Still joined to '%s'. The new network was not taken.\n", net.ssid);
		return 1;
	}

	fprintf(stdout, "\nJoined:\n\n");
	print_wifi_network(&net, 0);
	return 0;
}

/*
 * Get an open file descriptor for the Wi-Fi target.
 *
 * The path argument is optional here, unlike every other operation, and that is
 * deliberate. Wi-Fi lives on the emulated NETWORK target - a different SCSI ID
 * from the disk, with its own device node - so the path that works for -s and
 * -l is the wrong one, and the right one is not something the operator can work
 * out from anything visible on the host. Given no path we find it; given one we
 * check it really is the radio and, if it isn't, say which node is.
 *
 * Returns the open fd (caller closes it) or -1.
 */
static int cli_wifi_open(const char *path, int for_write)
{
	char found[SCSI_PATH_MAX];
	int dev;
	int auto_found = 0;

	if (path == NULL) {
		if (toolbox_wifi_find(found, sizeof(found)) != 0) {
			fprintf(stderr, "Error: no Wi-Fi device found on the SCSI bus.\n");
			fprintf(stderr, "The Wi-Fi commands are answered by the emulated NETWORK target\n");
			fprintf(stderr, "(a DaynaPort SCSI/Link), not by the disk or CD. Check that a\n");
			fprintf(stderr, "network device is enabled in the firmware's config and that the\n");
			fprintf(stderr, "board has a radio; 'irixscsitb -t <device>' lists the emulated\n");
			fprintf(stderr, "targets and 'irixscsitb -b' scans the bus.\n");
			if (!running_as_root())
				fprintf(stderr, "You are also not root, which on its own would explain this.\n");
			return -1;
		}
		path = found;
		auto_found = 1;
	}

	dev = scsi_open((char *)path, for_write ? 0 : 1);
	if (dev < 0 && for_write)
		dev = scsi_open((char *)path, 1);
	if (dev < 0) {
		fprintf(stderr, "ERROR: Cannot open %s: %s\n", path, strerror(errno));
		if (!running_as_root())
			fprintf(stderr, "You are not root - that is almost certainly why. Re-run as root.\n");
		return -1;
	}

	if (!toolbox_wifi_probe(dev, NULL, 0)) {
		fprintf(stderr, "Error: %s is not the Wi-Fi device.\n", path);
		fprintf(stderr, "It did not answer the Wi-Fi info command (0x1C/0x04).\n");
		scsi_close(dev);

		/* Point at the right node rather than just refusing. */
		if (toolbox_wifi_find(found, sizeof(found)) == 0)
			fprintf(stderr, "The Wi-Fi device on this bus is %s - use that instead,\n"
					"or leave the device path off entirely and it will be found.\n", found);
		else
			fprintf(stderr, "No Wi-Fi device was found on this bus either. Remember the\n"
					"Wi-Fi commands go to the emulated NETWORK target, not the disk.\n");
		return -1;
	}

	/* Only now, once it has actually answered - saying "using X" and then
	 * failing on X reads like the tool picked the wrong node. */
	if (auto_found)
		fprintf(stdout, "Using Wi-Fi device %s\n", path);

	return dev;
}

/* Dispatch for the three Wi-Fi modes. Returns a process exit status. */
static int cli_wifi(int mode, const char *path, const char *ssid,
		    const char *key, int channel)
{
	int dev;
	int ret;

	dev = cli_wifi_open(path, mode == MODE_WIFI_JOIN);
	if (dev < 0)
		return 1;

	if (mode == MODE_WIFI_SCAN)
		ret = cli_wifi_scan(dev);
	else if (mode == MODE_WIFI_INFO)
		ret = cli_wifi_info(dev);
	else
		ret = cli_wifi_join(dev, ssid, key, channel);

	scsi_close(dev);
	return ret == 0 ? 0 : 1;
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
 * The markers on one scan row. out must hold SCAN_TAG_MAX bytes; the longest
 * combination is well under that.
 *
 * Written as an accumulation rather than a chain of ternaries because the two
 * questions are independent: "does it implement the toolbox" and "does it have
 * a radio" are answered by different commands, and today's firmware answering
 * them on different targets is a fact about the firmware, not a rule we should
 * bake into what we can display.
 */
static void scan_tags(const ToolboxScanEntry *e, char *out)
{
	out[0] = '\0';
	if (e->confirmed)
		strcpy(out, "  [TOOLBOX]");
	else if (e->claims)
		strcpy(out, "  [claims toolbox, no 0xD9 answer]");
	if (e->wifi)
		strcat(out, "  [WIFI]");
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
	char tags[SCAN_TAG_MAX];
	int count, i;
	int answered = 0;
	int found = 0;
	int unconfirmed = 0;
	int wifi = 0;

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
		if (e.wifi)
			wifi++;

		scan_tags(&e, tags);
		fprintf (stdout, "%-22s %-8s %s%s\n", e.path, e.type_name, e.identity, tags);
	}

	fprintf (stdout, "\n%i device(s) answered, %i toolbox-capable", answered, found);
	if (unconfirmed > 0)
		fprintf (stdout, " (%i claimed toolbox but failed 0xD9)", unconfirmed);
	if (wifi > 0)
		fprintf (stdout, ", %i with Wi-Fi", wifi);
	fprintf (stdout, ".\n");

	if (found > 0)
		fprintf (stdout, "Pass one of the [TOOLBOX] paths to the other options, e.g. -i <device>\n");
	else if (answered > 0 && !force_toolbox)
		fprintf (stdout, "No toolbox target found. Check toolbox mode is enabled on the device,\n"
				 "or re-scan with -F to test every device by issuing 0xD9 directly.\n");
	if (wifi > 0)
		fprintf (stdout, "The [WIFI] path answers -w / -W / -j; those options find it themselves,\n"
				 "so you can leave the device path off for Wi-Fi.\n");
	return found;
}

static void do_drive(char *path, int list, int verbose, int cd_img, int file, char *outdir, int force)
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
		{
			char mnt[SCSI_PATH_MAX];
			char why[512];
			int st;

			/* mediad has already been stopped by main(); clear any
			 * remaining mount before swapping the image out from
			 * under it. */
			st = toolbox_prepare_cd_swap(path, mnt, sizeof(mnt), why, sizeof(why));

			if (st == CDSWAP_BUSY && !force)
			{
				fprintf (stderr, "Refusing to switch: %s is still mounted and could not be unmounted.\n", mnt);
				if (why[0] != '\0')
					fprintf (stderr, "Still in use by:\n%s", why);
				fprintf (stderr, "Close whatever is using it (a shell sitting in the directory counts),\n");
				fprintf (stderr, "then retry - or pass -f to switch anyway, which risks corrupting the\n");
				fprintf (stderr, "mounted filesystem.\n");
			}
			else
			{
				if (st == CDSWAP_UNMOUNTED)
					fprintf (stdout, "Unmounted %s before switching.\n", mnt);
				else if (st == CDSWAP_BUSY)
					fprintf (stderr, "WARNING: %s is still mounted; switching anyway (-f).\n", mnt);
				toolbox_setnextcd(dev, cd_img);
			}
		}
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
	fprintf(stderr, "\t-f      : force a CD switch even if the volume is still mounted (risky)\n");
	fprintf(stderr, "\t-V      : show build revision/date and exit (also -version)\n");
	fprintf(stderr, "\nWi-Fi (needs NO device path - the network target is found automatically):\n");
	fprintf(stderr, "\t-w      : scan for Wi-Fi networks and list them\n");
	fprintf(stderr, "\t-W      : show the Wi-Fi network currently joined\n");
	fprintf(stderr, "\t-j ssid : join the named Wi-Fi network\n");
	fprintf(stderr, "\t-k key  : password for -j (omit for an open network)\n");
	fprintf(stderr, "\t-n num  : channel for -j (default 0 = let the device choose)\n");
	fprintf(stderr, "\nNOTE: Wi-Fi is answered by the emulated NETWORK target (DaynaPort SCSI/Link),\n");
	fprintf(stderr, "which is a DIFFERENT SCSI ID from the disk. If you do pass a device path to\n");
	fprintf(stderr, "the Wi-Fi options it must be that one; 'irixscsitb -b' marks it [WIFI].\n");
	/* Only nag about root when we actually aren't root. */
	if (!running_as_root())
		fprintf(stderr, "\nNOTE: you are not running as root - opening the SCSI device will most\n"
				"likely fail. Re-run this as root (or via su).\n");
}

int main(int argc, char *argv[])
{
	int c, cdimg = NOT_ACTIVE, list = 0, file = NOT_ACTIVE;
	int force = 0;
	char outdir[1024];
	/* One byte longer than the protocol allows, so an over-long value still
	 * arrives at toolbox_wifi_join() intact enough to be REJECTED with a
	 * message rather than silently truncated into a different network name. */
	char wifi_ssid[WIFI_SSID_MAX + 2];
	char wifi_key[WIFI_KEY_MAX + 2];
	int wifi_channel = 0;

	/* Must start empty: without -o or -p nothing else writes to it, and
	 * toolbox_getfile() reads it to decide whether to default to "./". */
	outdir[0] = '\0';
	wifi_ssid[0] = '\0';
	wifi_key[0] = '\0';

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

	while ((c = getopt(argc, argv, "hvVlsitbwWDFfc:d:g:o:p:j:k:n:")) != -1) switch (c) {
		case 'c':
			cdimg = atoi(optarg);
			break;
		case 'w':
			list = MODE_WIFI_SCAN;
			break;
		case 'W':
			list = MODE_WIFI_INFO;
			break;
		case 'j':
			strncpy(wifi_ssid, optarg, sizeof(wifi_ssid) - 1);
			wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
			list = MODE_WIFI_JOIN;
			break;
		case 'k':
			strncpy(wifi_key, optarg, sizeof(wifi_key) - 1);
			wifi_key[sizeof(wifi_key) - 1] = '\0';
			break;
		case 'n':
			wifi_channel = atoi(optarg);
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
		case 'f':
			force = 1;
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

	/*
	 * The Wi-Fi modes take no device path either, for a different reason:
	 * they talk to the emulated NETWORK target rather than the disk, and
	 * that node is found for us. A path is still accepted (and checked) if
	 * one is given.
	 *
	 * They also bypass do_drive() entirely, because the toolbox gate it
	 * applies would reject the network target correctly - the firmware does
	 * not implement 0xD0-0xDA there at all.
	 */
	if (list == MODE_WIFI_SCAN || list == MODE_WIFI_INFO || list == MODE_WIFI_JOIN) {
		if (argc > 1)
			fprintf(stderr, "WARNING: extra arguments after '%s' ignored - put options BEFORE the device path.\n", argv[0]);
		return cli_wifi(list, argc >= 1 ? argv[0] : NULL,
				wifi_ssid, wifi_key, wifi_channel);
	}

	if (wifi_key[0] != '\0' || wifi_channel != 0)
		fprintf(stderr, "WARNING: -k/-n only mean anything with -j; ignoring them.\n");

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
	do_drive(argv[0], list, verbose, cdimg, file, outdir, force);

	if (cdimg != -1)
		mediad_start ();

	return 0;
}
