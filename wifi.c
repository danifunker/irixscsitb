/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * Toolbox Wi-Fi core: the 0x1C command builders, the two-stage Wi-Fi target
 * detection and the bus search that finds the network node for you.
 *
 * Split out of toolbox.c because this is a genuinely different thing on two
 * counts. It is a different command family - one opcode with subcommands in a
 * SIX-byte CDB, where every 0xD0-0xDA command is a ten-byte CDB - and it is
 * answered by a different TARGET. The Wi-Fi commands live on the firmware's
 * emulated DaynaPort SCSI/Link network device, not on the disk or CD the same
 * BlueSCSI is emulating, so the device node that works for -s and -l is the
 * wrong one here. That is the single most common way to get this wrong, and it
 * is why toolbox_wifi_find() exists.
 *
 * Front-end agnostic like the rest of the core: everything here returns data or
 * a status and prints only errors and verbose diagnostics, never a result.
 *
 * The protocol is documented in docs/toolbox-protocol.md. It was reconstructed
 * from the BlueSCSI firmware itself (lib/SCSI2SD/src/firmware/network.c) and
 * cross-checked against the two working host implementations: joshua stein's
 * Macintosh desk accessory wifi_da and SonnyJim's bswifi. The firmware's own
 * documentation is wrong about the CDB layout; the code is not.
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
#include "irixscsitb.h"

/*
 * FIRMWARE names we recognise as advertising a Wi-Fi-capable network target.
 *
 * Matched the same way toolbox_firmware_ids[] is - as a substring of the
 * trimmed "<vendor> <product> <rev>" INQUIRY identity - and, like that list,
 * these are deliberately the names the FIRMWARE stamps rather than a model we
 * decided to bless. BlueSCSI's network personalities are "Dayna SCSI/Link 2.0f"
 * and "AmigaNET SCSI/Link 1.0f" (BlueSCSI_config.h DRIVEINFO_NETWORK /
 * DRIVEINFO_AMIGAWIFI), so the product string they share is the useful key:
 * one entry covers both, and a future personality that keeps the SCSI/Link
 * product name is covered without a code change.
 *
 * A claim is never sufficient by itself. A real vintage Dayna SCSI/Link would
 * match this too and has no radio at all - it is toolbox_wifi_confirm() below,
 * which has to get a well-formed answer out of the device, that decides.
 */
static const char *wifi_firmware_ids[] = { "SCSI/Link" };
#define N_WIFI_IDS (int)(sizeof(wifi_firmware_ids) / sizeof(wifi_firmware_ids[0]))

/*
 * Build the six-byte Wi-Fi CDB.
 *
 *   CDB[0]    0x1C, SCSI_NETWORK_WIFI_CMD
 *   CDB[1]    subcommand
 *   CDB[2]    unused
 *   CDB[3..4] transfer length, big endian
 *   CDB[5]    control
 *
 * 0x1C is not a free vendor opcode: in standard SCSI it is RECEIVE DIAGNOSTIC
 * RESULTS, which any device on the bus may legitimately implement. The
 * firmware overloads it on its network target only, so from the point of view
 * of every OTHER device a CDB built here is a real (read-only) diagnostic
 * command. That is why every path that sends one runs behind the identity and
 * device-type gates in toolbox_wifi_probe() - keep it that way.
 *
 * The length field is what the firmware calls `size`, and for JOIN it is not
 * advisory: scsiNetworkWifiJoin() rejects the command outright unless it says
 * exactly sizeof(struct wifi_join_request).
 */
static void wifi_cdb(unsigned char *cdb, unsigned char subcmd, int length)
{
	memset(cdb, 0, WIFI_CDB_LENGTH);
	cdb[0] = SCSI_NETWORK_WIFI_CMD;
	cdb[1] = subcmd;
	cdb[3] = (unsigned char)((length >> 8) & 0xFF);
	cdb[4] = (unsigned char)(length & 0xFF);
}

/*
 * Issue a Wi-Fi data-in command. With probe non-zero it goes out quietly and
 * without retries - what detection wants, since most nodes on the bus will
 * (rightly) reject 0x1C. Returns 0 on success.
 */
static int wifi_read(int dev, unsigned char subcmd, unsigned char *buf, int len, int probe)
{
	unsigned char cdb[WIFI_CDB_LENGTH];

	wifi_cdb(cdb, subcmd, len);
	if (buf != NULL && len > 0)
		memset(buf, 0, len);

	if (probe)
		return scsi_send_command_probe(dev, cdb, WIFI_CDB_LENGTH, buf, len);
	return scsi_send_command(dev, cdb, WIFI_CDB_LENGTH, buf, len);
}

/* The big-endian 16-bit byte count that prefixes a SCAN_RESULTS/INFO reply. */
static int wifi_prefix_len(const unsigned char *buf)
{
	return ((int)buf[0] << 8) | (int)buf[1];
}

/*
 * Decode one 74-byte wire entry into host types.
 *
 * Done field by field rather than as a struct copy on purpose: the firmware
 * declares wifi_network_entry with __attribute__((packed)), which MIPSpro has
 * no equivalent of, so an identically-written C struct here would be free to
 * gain padding and silently misread every field after the SSID.
 *
 * The SSID is copied through a bounded loop and always NUL-terminated: the wire
 * field is a fixed 64 bytes and a firmware that filled all 64 would leave no
 * terminator behind.
 */
static void wifi_decode(const unsigned char *raw, ToolboxWifiNetwork *net)
{
	int i;

	memset(net, 0, sizeof(*net));

	for (i = 0; i < WIFI_SSID_MAX; i++) {
		net->ssid[i] = (char)raw[WIFI_OFF_SSID + i];
		if (net->ssid[i] == '\0')
			break;
	}
	net->ssid[WIFI_SSID_MAX] = '\0';

	memcpy(net->bssid, raw + WIFI_OFF_BSSID, WIFI_BSSID_LEN);

	/*
	 * rssi is an int8_t on the wire. Sign-extend it explicitly: plain char
	 * is unsigned on some of the platforms this builds for, which would turn
	 * a perfectly ordinary -67 dBm into 189.
	 */
	net->rssi = (int)raw[WIFI_OFF_RSSI];
	if (net->rssi > 127)
		net->rssi -= 256;

	net->channel = (int)raw[WIFI_OFF_CHANNEL];
	net->flags   = raw[WIFI_OFF_FLAGS];
}

const char *wifi_auth_name(unsigned char flags)
{
	return (flags & WIFI_FLAG_AUTH) ? "secured" : "open";
}

void wifi_bssid_str(const unsigned char *bssid, char *out, int outlen)
{
	if (out == NULL || outlen < 18)
		return;
	sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x",
		bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

int wifi_signal_bars(int rssi)
{
	if (rssi >= -55)
		return 4;
	if (rssi >= -65)
		return 3;
	if (rssi >= -75)
		return 2;
	if (rssi >= -85)
		return 1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * detection
 * ------------------------------------------------------------------ */

/*
 * Functional proof that a target really implements the Wi-Fi commands: ask it
 * for WIFI_CMD_INFO and require the reply to be shaped the way the firmware
 * shapes it.
 *
 * scsiNetworkWifiInfo() always answers with a two-byte big-endian length
 * followed by that many bytes, and the length is unconditionally
 * sizeof(struct wifi_network_entry) - it does not vary with whether the radio
 * has joined anything. So a reply whose prefix reads exactly 74 is a strong
 * signal, and one that doesn't is not this command.
 *
 * That check matters more than it looks. 0x1C is RECEIVE DIAGNOSTIC RESULTS in
 * standard SCSI, so a plain disk may well answer the opcode rather than reject
 * it - it just answers with a diagnostic page, which will not carry 0x00 0x4A
 * in its first two bytes. (It is also why probing is safe: RECEIVE DIAGNOSTIC
 * RESULTS reads, it does not write.)
 *
 * Returns 1 if confirmed, 0 otherwise.
 */
static int toolbox_wifi_confirm(int dev, int probe)
{
	unsigned char buf[WIFI_ENTRY_SIZE + WIFI_LEN_PREFIX];

	if (wifi_read(dev, WIFI_CMD_INFO, buf, sizeof(buf), probe) != 0)
		return 0;

	if (wifi_prefix_len(buf) != WIFI_ENTRY_SIZE) {
		if (verbose)
			fprintf(stdout, "Wi-Fi INFO answered but the length prefix is %d, "
					"expected %d - not a Wi-Fi target\n",
				wifi_prefix_len(buf), WIFI_ENTRY_SIZE);
		return 0;
	}
	return 1;
}

/* Does this INQUIRY identity carry a known Wi-Fi firmware product name? */
static const char *wifi_identity_accepted(const char *identity)
{
	int i;

	for (i = 0; i < N_WIFI_IDS; i++)
		if (strstr(identity, wifi_firmware_ids[i]) != NULL)
			return wifi_firmware_ids[i];
	return NULL;
}

/*
 * The peripheral-device-type floor under every 0x1C we send.
 *
 * 0x1C is RECEIVE DIAGNOSTIC RESULTS in standard SCSI (see wifi_cdb() above),
 * so to anything that is not the firmware's network target our probe is a
 * real, if read-only, diagnostic command. The identity check above is the
 * primary gate; this is the belt to its braces, and unlike the identity check
 * it holds even under -F, because -F exists for "an unrecognised BlueSCSI",
 * never for "send RECEIVE DIAGNOSTIC RESULTS to my disk".
 *
 * The floor refuses the INQUIRY types that plainly cannot be the radio and
 * admits processor (0x03 - what the DaynaPort presents), communications
 * (0x09 - what a network device properly is) and the reserved/vendor/unknown
 * codes, so a future firmware personality with an odd type still gets through
 * under -F.
 */
static int wifi_pdt_allowed(int pdt)
{
	switch (pdt & 0x1F) {
	case 0x00:              /* disk */
	case 0x01:              /* tape */
	case 0x02:              /* printer */
	case 0x04:              /* WORM */
	case 0x05:              /* CD-ROM */
	case 0x06:              /* scanner */
	case 0x07:              /* optical */
	case 0x08:              /* medium changer */
	case 0x0C:              /* RAID controller */
	case 0x0D:              /* enclosure services */
	case 0x0E:              /* simplified disk */
		return 0;
	default:
		return 1;
	}
}

/*
 * The gating here is a SAFETY boundary, not a speed optimisation: it is the
 * only thing standing between wifi_cdb()'s standard-opcode 0x1C and every
 * ordinary device on the bus. See the contract in irixscsitb.h.
 */
int toolbox_wifi_probe(int dev, const char *identity, int pdt, int probe)
{
	ToolboxDetect det;

	/*
	 * Fetch whatever the caller could not supply. This runs even under -F:
	 * the device-type floor below needs the INQUIRY byte, and a target
	 * that will not even answer INQUIRY has no business receiving 0x1C.
	 */
	if (identity == NULL || pdt < 0) {
		if (toolbox_identify(dev, &det) != TOOLBOX_OK)
			return 0;
		if (identity == NULL)
			identity = det.identity;
		if (pdt < 0)
			pdt = det.inq.dev_type;
	}

	/*
	 * The device-type floor - deliberately ahead of, and independent of,
	 * the -F escape hatch: a disk stays a disk no matter how hard the
	 * operator forces. Loud when the operator forced a named device and
	 * was refused anyway; quiet during a bus scan, where refusing storage
	 * targets is simply the normal case.
	 */
	if (!wifi_pdt_allowed(pdt)) {
		if (force_toolbox && !probe)
			fprintf(stderr, "Refusing Wi-Fi probe: 0x1C is RECEIVE DIAGNOSTIC "
					"RESULTS and '%s' is a %s-type device, not a "
					"network target. -F does not override this.\n",
				identity, inquiry_pdt_name((unsigned char)pdt));
		else if (verbose)
			fprintf(stdout, "Wi-Fi probe withheld from '%s': a %s-type "
					"device cannot be the radio\n",
				identity, inquiry_pdt_name((unsigned char)pdt));
		return 0;
	}

	if (!force_toolbox) {
		if (wifi_identity_accepted(identity) == NULL)
			return 0;
		if (verbose)
			fprintf(stdout, "Claims a Wi-Fi radio via INQUIRY identity '%s'\n",
				identity);
	} else if (wifi_identity_accepted(identity) == NULL) {
		/*
		 * -F means "I know what I am doing, test it directly", exactly
		 * as it does for the toolbox - but say so out loud: the 0x1C
		 * about to go out is a standard opcode aimed at a device we
		 * could not positively identify.
		 */
		fprintf(stderr, "Warning: -F: sending Wi-Fi probe (0x1C, RECEIVE "
				"DIAGNOSTIC RESULTS) to unrecognised %s-type device '%s'\n",
			inquiry_pdt_name((unsigned char)pdt), identity);
	}

	return toolbox_wifi_confirm(dev, probe);
}

int toolbox_wifi_find(char *path, int pathlen)
{
	char paths[MAX_SCAN_DEVICES][SCSI_PATH_MAX];
	int count, i, dev;

	if (path == NULL || pathlen <= 0)
		return -1;
	path[0] = '\0';

	count = scsi_enum_devices(paths, MAX_SCAN_DEVICES);
	if (count <= 0)
		return -1;

	for (i = 0; i < count; i++) {
		/*
		 * Read-only: INQUIRY and WIFI_CMD_INFO both only read, so the
		 * search cannot disturb a disk it happens to walk past. A caller
		 * that goes on to join a network reopens read/write.
		 */
		dev = scsi_open(paths[i], 1);
		if (dev < 0)
			continue;

		if (toolbox_wifi_probe(dev, NULL, -1, 1)) {
			scsi_close(dev);
			strncpy(path, paths[i], pathlen - 1);
			path[pathlen - 1] = '\0';
			return 0;
		}
		scsi_close(dev);
	}
	return -1;
}

/* ------------------------------------------------------------------ *
 * commands
 * ------------------------------------------------------------------ */

/*
 * WIFI_CMD_SCAN 0x01
 *
 * One byte back. The firmware returns 1 when the scan started and the negative
 * error from platform_network_wifi_start_scan() when it did not - which arrives
 * here as a large unsigned byte, so anything other than 1 is a refusal.
 */
int toolbox_wifi_scan_start(int dev)
{
	unsigned char buf[1];

	if (wifi_read(dev, WIFI_CMD_SCAN, buf, sizeof(buf), 0) != 0) {
		fprintf(stderr, "Error: Wi-Fi scan request failed - %s\n", strerror(errno));
		return -1;
	}

	if (verbose)
		fprintf(stdout, "Wi-Fi SCAN returned 0x%02x\n", buf[0]);

	if (buf[0] != 1) {
		fprintf(stderr, "Error: the device refused to start a Wi-Fi scan (status 0x%02x).\n",
			buf[0]);
		return -1;
	}
	return 0;
}

/*
 * WIFI_CMD_COMPLETE 0x02
 *
 * One byte: 1 once the scan has finished, 0 while it is still running.
 */
int toolbox_wifi_scan_done(int dev)
{
	unsigned char buf[1];

	if (wifi_read(dev, WIFI_CMD_COMPLETE, buf, sizeof(buf), 0) != 0) {
		fprintf(stderr, "Error: Wi-Fi scan-completion check failed - %s\n", strerror(errno));
		return -1;
	}
	return buf[0] == 1 ? 1 : 0;
}

/*
 * Start a scan and wait it out.
 *
 * The wait is not optional and not cosmetic: scsiNetworkWifiScanResults()
 * answers CHECK CONDITION (ILLEGAL_REQUEST / INVALID_FIELD_IN_CDB) if it is
 * asked while a scan is still running, so a caller that fetched results
 * immediately would get an error rather than an empty list.
 *
 * sleep(3) rather than a sub-second timer: IRIX 5.3 has no usleep(3) - it
 * compiles and then fails to LINK - and a radio scan takes seconds, so one-
 * second granularity costs nothing. sleep() links everywhere from 5.3 to 6.5.
 */
int toolbox_wifi_scan(int dev, int timeout_sec)
{
	int waited;
	int done;

	if (toolbox_wifi_scan_start(dev) != 0)
		return -1;

	for (waited = 0; waited < timeout_sec; waited++) {
		sleep(1);

		done = toolbox_wifi_scan_done(dev);
		if (done < 0)
			return -1;
		if (done) {
			if (verbose)
				fprintf(stdout, "Wi-Fi scan finished after %d second(s)\n", waited + 1);
			return 0;
		}
	}

	fprintf(stderr, "Error: the Wi-Fi scan did not finish within %d seconds.\n", timeout_sec);
	return -1;
}

/*
 * WIFI_CMD_SCAN_RESULTS 0x03
 *
 * Reply: a two-byte big-endian byte count, then that many bytes of packed
 * 74-byte entries. The count excludes itself, and the firmware truncates it to
 * a whole number of entries, so dividing is safe.
 *
 * A count of zero is a real answer, not a failure: it means the scan found
 * nothing. The distinction matters to the caller, so it comes back as 0 rather
 * than -1.
 */
int toolbox_wifi_results(int dev, ToolboxWifiNetwork *nets, int max)
{
	unsigned char buf[WIFI_SCAN_BUF_SIZE];
	int size, count, i;

	if (nets == NULL || max <= 0)
		return -1;

	if (wifi_read(dev, WIFI_CMD_SCAN_RESULTS, buf, sizeof(buf), 0) != 0) {
		fprintf(stderr, "Error: Wi-Fi scan results failed - %s\n", strerror(errno));
		fprintf(stderr, "(the device rejects this until a scan has completed)\n");
		return -1;
	}

	size = wifi_prefix_len(buf);
	if (verbose)
		fprintf(stdout, "Wi-Fi SCAN_RESULTS reports %d bytes of entries\n", size);

	if (size < 0 || size > (int)sizeof(buf) - WIFI_LEN_PREFIX) {
		fprintf(stderr, "Error: Wi-Fi scan results reported an implausible size (%d bytes)\n", size);
		return -1;
	}

	count = size / WIFI_ENTRY_SIZE;
	if (count > WIFI_MAX_NETWORKS)
		count = WIFI_MAX_NETWORKS;   /* the firmware keeps ten slots */
	if (count > max)
		count = max;

	for (i = 0; i < count; i++)
		wifi_decode(buf + WIFI_LEN_PREFIX + (i * WIFI_ENTRY_SIZE), &nets[i]);

	return count;
}

/*
 * WIFI_CMD_INFO 0x04
 *
 * Reply: the same two-byte count - always 74 here - followed by one entry
 * describing the network currently joined. An all-zero entry (empty SSID) is
 * how "not associated" comes back; that is not an error.
 */
int toolbox_wifi_info(int dev, ToolboxWifiNetwork *out)
{
	unsigned char buf[WIFI_ENTRY_SIZE + WIFI_LEN_PREFIX];
	int size;

	if (out == NULL)
		return -1;

	if (wifi_read(dev, WIFI_CMD_INFO, buf, sizeof(buf), 0) != 0) {
		fprintf(stderr, "Error: Wi-Fi info failed - %s\n", strerror(errno));
		return -1;
	}

	size = wifi_prefix_len(buf);
	if (size != WIFI_ENTRY_SIZE) {
		fprintf(stderr, "Error: Wi-Fi info returned %d bytes, expected %d\n",
			size, WIFI_ENTRY_SIZE);
		return -1;
	}

	wifi_decode(buf + WIFI_LEN_PREFIX, out);
	return 0;
}

/*
 * WIFI_CMD_JOIN 0x05 - the only Wi-Fi command that writes.
 *
 * Payload is the 130-byte wifi_join_request: ssid[64], key[64], channel, pad.
 * The buffer is zeroed first, which both NUL-terminates the two strings and
 * fills the padding byte the firmware expects.
 *
 * Both fields are 64 bytes INCLUDING the terminator, so 63 usable characters -
 * the firmware strlcpy()s them into equally-sized buffers on the far side.
 */
int toolbox_wifi_join(int dev, const char *ssid, const char *key, int channel)
{
	unsigned char cdb[WIFI_CDB_LENGTH];
	unsigned char req[WIFI_JOIN_SIZE];
	int ssid_len, key_len;

	if (ssid == NULL)
		ssid = "";
	if (key == NULL)
		key = "";

	ssid_len = (int)strlen(ssid);
	key_len  = (int)strlen(key);

	if (ssid_len == 0) {
		fprintf(stderr, "Error: no network name (SSID) given\n");
		return -1;
	}
	if (ssid_len >= WIFI_SSID_MAX) {
		fprintf(stderr, "Error: network name too long (%d characters, maximum %d)\n",
			ssid_len, WIFI_SSID_MAX - 1);
		return -1;
	}
	if (key_len >= WIFI_KEY_MAX) {
		fprintf(stderr, "Error: password too long (%d characters, maximum %d)\n",
			key_len, WIFI_KEY_MAX - 1);
		return -1;
	}
	if (channel < 0 || channel > 255) {
		fprintf(stderr, "Error: channel %d out of range (0-255, 0 = auto)\n", channel);
		return -1;
	}

	memset(req, 0, sizeof(req));
	memcpy(req, ssid, ssid_len);
	memcpy(req + WIFI_SSID_MAX, key, key_len);
	req[WIFI_SSID_MAX + WIFI_KEY_MAX] = (unsigned char)channel;

	/*
	 * The length in the CDB has to be exactly 130: scsiNetworkWifiJoin()
	 * compares it against sizeof(struct wifi_join_request) and answers
	 * CHECK CONDITION on any other value rather than trying to cope.
	 */
	wifi_cdb(cdb, WIFI_CMD_JOIN, WIFI_JOIN_SIZE);

	if (scsi_send_commandw(dev, cdb, WIFI_CDB_LENGTH, req, (int)sizeof(req)) != 0) {
		fprintf(stderr, "Error: Wi-Fi join failed - %s\n", strerror(errno));
		return -1;
	}

	if (verbose)
		fprintf(stdout, "Wi-Fi join request for '%s' accepted by the device\n", ssid);
	return 0;
}
