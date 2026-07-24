/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * OS backend contract: the SCSI transport each platform implements.
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

/* Maximum length of a SCSI device node path handled by scsi_enum_devices(). */
#define SCSI_PATH_MAX 256

int scsi_open(char *path, int readonly);
int scsi_send_command(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len);
int scsi_send_commandw(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len);
int scsi_close(int dev);

/*
 * Single-shot data-in command: one attempt, no retries, no warning output.
 * scsi_send_command() retries ten times with a 100ms delay and prints a warning
 * each round, which is right for a command aimed at a known-good target but
 * wrong for the bus scanner, where most nodes are expected NOT to answer.
 * Returns 0 on success, non-zero on any failure.
 */
int scsi_send_command_probe(int dev, unsigned char *cmd, int cmd_len, unsigned char *buf, int buf_len);

/*
 * Enumerate the generic SCSI device nodes this host exposes (IRIX:
 * /dev/scsi/scNdNlN, Linux: /dev/sgN). Fills up to max entries of paths and
 * returns how many were written, or -1 on error. Existence of a node does not
 * guarantee a device answers - the caller still has to send INQUIRY.
 */
int scsi_enum_devices(char paths[][SCSI_PATH_MAX], int max);

int path_to_devnum(const char *path);

int mediad_start(void); /*Helper functions to start and stop the removable device damons */
int mediad_stop(void);

/*
 * Mount handling around a CD image swap.
 *
 * Swapping the image out from under a MOUNTED filesystem is how you corrupt
 * it: the host still holds cached metadata for the old disc. Verified on real
 * hardware - a BlueSCSI v2 on an Indigo would not pick up the new disc until
 * /CDROM was unmounted first. So the swap has to check, and refuse when the
 * volume is busy.
 *
 * media_find_mount(): is the emulated target behind `path` mounted right now?
 *   Fills mnt (mount point) and dev (block device); either may be NULL.
 *   Returns 1 if mounted, 0 if not, -1 if it could not be determined.
 *
 * media_unmount(): unmount that mount point. Returns 0 on success, -1 on
 *   failure - and on failure fills `why` with a human-readable reason,
 *   including which processes are holding it open where the OS can say.
 *
 * The Linux backend stubs both out: there is no mediad there and no
 * convention about who owns a removable mount.
 */
int media_find_mount(const char *path, char *mnt, int mntlen, char *dev, int devlen);
int media_unmount(const char *mnt, char *why, int whylen);
