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
