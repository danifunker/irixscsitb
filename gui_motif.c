/*
 * irixscsitb - toolbox for emulated SCSI devices (BlueSCSI / ZuluSCSI)
 * on SGI IRIX and Linux hosts.
 *
 * IRIS IM (OSF/Motif) front end, built as the binary "scsitbgui" alongside the
 * "irixscsitb" CLI. Covers everything the CLI does: scan the bus, interrogate a
 * target, list the emulated device map, browse and transfer the /shared
 * directory, list and switch CD images, and read or set the firmware debug flag.
 *
 * It calls exactly the same toolbox.c core the CLI does, so the two can never
 * drift: there is no protocol code in this file at all, only widgets and the
 * open/act/close sequence around each operation.
 *
 * Written against the Motif 1.2 API deliberately: 1.2 is what IRIX 5.3 ships
 * and 6.5 still carries, so one o32 binary covers 5.3 through 6.5. Do not
 * reach for Motif 2.x-only calls here.
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

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/List.h>
#include <Xm/PushB.h>
#include <Xm/Label.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/Separator.h>
#include <Xm/PanedW.h>
#include <Xm/ToggleB.h>
#include <Xm/MessageB.h>
#include <Xm/FileSB.h>
#include <Xm/SelectioB.h>
#include <X11/Shell.h>

#include "irixscsitb.h"

/*
 * XtVaAppInitialize() takes Cardinal* for argc up to X11R5 and int* from R6.
 * IRIX 5.3 reports XtSpecificationRelease 4, IRIX 6.5 is R6, and we build one
 * binary for both - so pick the type the headers in front of us actually want
 * rather than assuming either.
 */
#if defined(XtSpecificationRelease) && XtSpecificationRelease >= 6
typedef int XtArgcType;
#else
typedef Cardinal XtArgcType;
#endif

/*
 * Ask for the SGI look. These are the same resources every stock IRIX Motif
 * app sets (grep useSchemes in /usr/lib/X11/app-defaults): without them the
 * widgets render as plain OSF/Motif battleship grey. They are only fallbacks,
 * so a system that has no schemes installed - or a user with their own
 * app-defaults - simply ignores them and we still come up as ordinary Motif.
 *
 * The lists are deliberately a fixed font: rows are laid out in columns and a
 * proportional font would ragged them. It is also what lets the width
 * preflight below measure accurately.
 */
static String fallback_resources[] = {
	"*useSchemes: all",
	"*schemeFileList: SgiSpec",
	"*scheme: Base",
	"*sgiMode: true",
	/* Window title. The first component has to be the application NAME
	 * (argv[0], "scsitbgui") or its CLASS ("Scsitbgui" - the second argument
	 * to XtVaAppInitialize below); anything else silently never matches and
	 * the WM falls back to showing the bare binary name. */
	"Scsitbgui.title: scsitbgui",
	"*deviceList.fontList: fixed",
	"*contentList.fontList: fixed",
	"*deviceList.visibleItemCount: 6",
	"*contentList.visibleItemCount: 10",
	/* Widget labels live here rather than in the code so they can be
	 * overridden per-site (and eventually localised) without a rebuild. */
	"*file.labelString: File",
	"*rescan.labelString: Rescan Bus",
	"*quit.labelString: Quit",
	"*device.labelString: Device",
	"*interrogate.labelString: Interrogate...",
	"*targets.labelString: Emulated Targets...",
	"*debugShow.labelString: Show Debug State",
	"*debugOn.labelString: Turn Debug On",
	"*debugOff.labelString: Turn Debug Off",
	"*forceToggle.labelString: Force detection (-F)",
	"*help.labelString: Help",
	"*about.labelString: About...",
	"*busFrameLabel.labelString: SCSI bus",
	"*modeShared.labelString: Shared files",
	"*modeCds.labelString: CD images",
	"*refresh.labelString: Refresh",
	"*getFile.labelString: Get File...",
	"*putFile.labelString: Put File...",
	"*switchCd.labelString: Switch To CD",
	"*rescanButton.labelString: Rescan",
	"*status.labelString: Ready.",
	NULL
};

/*
 * Window-manager icon: a 32x32 one-bit disc.
 *
 * XBM rather than XPM because IRIX 5.3 ships libXpm.so WITHOUT its header -
 * /usr/include/X11/xpm.h is simply absent, exactly like ViewKit - so it cannot
 * be built against. XCreateBitmapFromData is core Xlib and always present, and
 * one bit deep is period-appropriate anyway.
 *
 * This is the WM icon, i.e. what 4Dwm shows when the window is iconified. A
 * proper Indigo Magic DESKTOP icon is a different and much larger job (FTR
 * rules plus a vector .icon file) and is deliberately not attempted.
 */
#define ICON_WIDTH  32
#define ICON_HEIGHT 32
static unsigned char icon_bits[] = {
   0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x1c, 0x00, 0x00, 0x03, 0xc0, 0x00,
   0x80, 0xf0, 0x0f, 0x01, 0x40, 0xfc, 0x3f, 0x02, 0x20, 0xff, 0xff, 0x04,
   0x90, 0xff, 0xff, 0x09, 0xc8, 0xff, 0xff, 0x13, 0xe4, 0xff, 0xff, 0x27,
   0xe4, 0xff, 0xff, 0x27, 0xf0, 0x1f, 0xf8, 0x0f, 0xf2, 0x8f, 0xf1, 0x4f,
   0xfa, 0xe7, 0xe7, 0x5f, 0xfa, 0x33, 0xcc, 0x5f, 0xf8, 0x13, 0xc8, 0x1f,
   0xf8, 0x1b, 0xd8, 0x1f, 0xf8, 0x1b, 0xd8, 0x1f, 0xf8, 0x13, 0xc8, 0x1f,
   0xfa, 0x33, 0xcc, 0x5f, 0xfa, 0xe7, 0xe7, 0x5f, 0xf2, 0x8f, 0xf1, 0x4f,
   0xf0, 0x1f, 0xf8, 0x0f, 0xe4, 0xff, 0xff, 0x27, 0xe4, 0xff, 0xff, 0x27,
   0xc8, 0xff, 0xff, 0x13, 0x90, 0xff, 0xff, 0x09, 0x20, 0xff, 0xff, 0x04,
   0x40, 0xfc, 0x3f, 0x02, 0x80, 0xf0, 0x0f, 0x01, 0x00, 0x03, 0xc0, 0x00,
   0x00, 0x38, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void set_window_icon(Widget shell)
{
	Display *dpy = XtDisplay(shell);
	Pixmap pm;

	if (dpy == NULL)
		return;
	pm = XCreateBitmapFromData(dpy, RootWindow(dpy, DefaultScreen(dpy)),
				   (char *)icon_bits, ICON_WIDTH, ICON_HEIGHT);
	if (pm == None)
		return;
	XtVaSetValues(shell, XtNiconPixmap, pm, XtNiconName, "scsitbgui", NULL);
}

/* Which listing the lower pane is showing. */
#define CONTENT_SHARED 0
#define CONTENT_CDS    1

static Widget toplevel;
static Widget device_list_w;    /* upper pane: devices on the bus */
static Widget content_list_w;   /* lower pane: /shared files or CD images */
static Widget status_w;
static Widget get_btn, put_btn, cd_btn, refresh_btn;
static Widget info_dialog, error_dialog, dir_dialog, file_dialog;

static ToolboxScanEntry scan[MAX_SCAN_DEVICES];
static int scan_n;
static int sel_dev = -1;        /* index into scan[], -1 = nothing selected */

static ToolboxFileEntry entries[MAX_FILES];
static int entries_n;
static int content_mode = CONTENT_SHARED;

/*
 * Set only while re-entering do_switch_cd() from the "Switch Anyway" button of
 * the busy-volume confirmation, so the second pass skips the guard.
 */
static int cd_swap_forced;
static Widget cd_force_dialog;

static void do_switch_cd(void);

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

/*
 * Bounded copy, used on every variable-length value before it reaches a
 * sprintf(). IRIX 5.3 has NO snprintf - it is absent from <stdio.h> and from
 * libc.so.1, so calling it fails to link exactly the way usleep(3) does - which
 * means the only way to keep a message buffer safe is to clamp what goes INTO
 * it. Device paths and INQUIRY identities are already bounded by the protocol,
 * but a filename coming back from a file-selection dialog is not.
 */
static void copy_clamped(char *dst, int dstlen, const char *src)
{
	int n = (int)strlen(src);

	if (dstlen <= 0)
		return;
	if (n > dstlen - 1)
		n = dstlen - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void set_status(const char *text)
{
	XmString xs = XmStringCreateLtoR((char *)text, XmSTRING_DEFAULT_CHARSET);

	XtVaSetValues(status_w, XmNlabelString, xs, NULL);
	XmStringFree(xs);
}

static void list_add(Widget list, const char *text)
{
	XmString xs = XmStringCreateLtoR((char *)text, XmSTRING_DEFAULT_CHARSET);

	XmListAddItemUnselected(list, xs, 0);
	XmStringFree(xs);
}

/*
 * Pop up a message. Two dialogs are created once and reused rather than made
 * fresh each time, which would leak a widget per call.
 */
static void show_msg(const char *title, const char *text, int is_error)
{
	Widget dlg;
	XmString xs, xt;

	if (is_error) {
		if (error_dialog == NULL) {
			error_dialog = XmCreateErrorDialog(toplevel, "errorDialog", NULL, 0);
			/* Message boxes come with Cancel and Help buttons we
			 * have no use for. Only worth doing once. */
			XtUnmanageChild(XmMessageBoxGetChild(error_dialog, XmDIALOG_CANCEL_BUTTON));
			XtUnmanageChild(XmMessageBoxGetChild(error_dialog, XmDIALOG_HELP_BUTTON));
		}
		dlg = error_dialog;
	} else {
		if (info_dialog == NULL) {
			info_dialog = XmCreateInformationDialog(toplevel, "infoDialog", NULL, 0);
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_CANCEL_BUTTON));
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_HELP_BUTTON));
		}
		dlg = info_dialog;
	}

	xs = XmStringCreateLtoR((char *)text, XmSTRING_DEFAULT_CHARSET);
	xt = XmStringCreateLtoR((char *)title, XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(dlg, XmNmessageString, xs, XmNdialogTitle, xt, NULL);
	XmStringFree(xs);
	XmStringFree(xt);

	XtManageChild(dlg);
}

/*
 * The XFontStruct behind a widget's fontList, so a string can be measured in
 * pixels. Returns NULL if the font list can't be read, in which case callers
 * simply skip resizing and let Motif pick a size.
 */
static XFontStruct *widget_font(Widget w)
{
	XmFontList fl = NULL;
	XmFontContext ctx;
	XmStringCharSet cs = NULL;
	XFontStruct *fs = NULL;

	XtVaGetValues(w, XmNfontList, &fl, NULL);
	if (fl == NULL)
		return NULL;
	if (!XmFontListInitFontContext(&ctx, fl))
		return NULL;
	if (!XmFontListGetNextFont(ctx, &cs, &fs))
		fs = NULL;
	if (cs != NULL)
		XtFree(cs);
	XmFontListFreeFontContext(ctx);
	return fs;
}

/*
 * Size a list to the widest row it actually holds - the "preflight". Rows are
 * built from real device paths and INQUIRY strings whose widths aren't known
 * until the bus has been scanned, so guessing a column count in advance either
 * wastes screen or clips the interesting part (the firmware name lives at the
 * END of the identity string). Measuring the real text is the only way to get
 * it right on both a 17-character IRIX path and an 8-character Linux one.
 *
 * The list is XmCONSTANT, so anything still too wide scrolls horizontally
 * rather than shoving the window off screen.
 */
static void fit_list_width(Widget list, const char *widest)
{
	XFontStruct *fs = widget_font(list);
	Dimension margin = 0, shadow = 0;
	int px;

	if (fs == NULL || widest == NULL || *widest == '\0')
		return;

	px = XTextWidth(fs, (char *)widest, (int)strlen(widest));
	if (px <= 0)
		return;

	/* Leave room for the list's own margins/shadow plus a vertical
	 * scrollbar, so the text isn't sitting under them. */
	XtVaGetValues(list, XmNlistMarginWidth, &margin, XmNshadowThickness, &shadow, NULL);
	px += 2 * (int)margin + 2 * (int)shadow + 28;

	XtVaSetValues(list, XmNwidth, (Dimension)px, NULL);
}

/* Currently selected row of a list, 0-based, or -1 if nothing is selected. */
static int list_selection(Widget list)
{
	int *pos = NULL;
	int count = 0;
	int result = -1;

	if (XmListGetSelectedPos(list, &pos, &count) && count > 0)
		result = pos[0] - 1;    /* Motif positions are 1-based */
	if (pos != NULL)
		XtFree((char *)pos);
	return result;
}

/*
 * Open the selected device. Mirrors the CLI's do_drive(): CD operations must
 * open read-only because emulated CD targets refuse a read/write open, and
 * everything else tries read/write and falls back.
 */
static int open_selected(int readonly)
{
	int dev;
	char msg[SCSI_PATH_MAX + 256];

	if (sel_dev < 0) {
		show_msg("No device", "Select a device in the SCSI bus list first.", 1);
		return -1;
	}

	dev = scsi_open(scan[sel_dev].path, readonly);
	if (dev < 0 && !readonly)
		dev = scsi_open(scan[sel_dev].path, 1);
	if (dev < 0) {
		sprintf(msg, "Cannot open %s.\n\n%s", scan[sel_dev].path,
			geteuid() == 0 ? "The device refused to open." :
					 "You are not running as root - that is almost\n"
					 "certainly why. The /dev/scsi nodes are root-owned.");
		show_msg("Open failed", msg, 1);
		return -1;
	}
	return dev;
}

/*
 * Guard for every toolbox operation. A device that answered INQUIRY but never
 * answered 0xD9 is NOT driven as a toolbox target - same rule as the CLI - so
 * say so plainly rather than letting each command fail on its own.
 */
static int require_toolbox(void)
{
	char msg[TOOLBOX_IDENTITY_MAX + 320];

	if (sel_dev < 0) {
		show_msg("No device", "Select a device in the SCSI bus list first.", 1);
		return 0;
	}
	if (scan[sel_dev].confirmed)
		return 1;

	if (scan[sel_dev].claims)
		sprintf(msg, "'%s'\n\nadvertises the toolbox API but did not answer\n"
			     "TOOLBOX_LIST_DEVICES (0xD9), so it does not actually\n"
			     "implement it. Refusing to drive it as a toolbox target.",
			scan[sel_dev].identity);
	else
		sprintf(msg, "'%s'\n\ndoes not advertise the toolbox API.\n\n"
			     "Known toolbox firmware: BlueSCSI, ZuluSCSI (toolbox must\n"
			     "be enabled on the device). If you believe this one supports\n"
			     "it, switch on Device > Force detection (-F) and rescan.",
			scan[sel_dev].identity);
	show_msg("Not a toolbox device", msg, 1);
	return 0;
}

/* Grey out the operation buttons unless the selection can actually do them. */
static void update_sensitivity(void)
{
	int usable = (sel_dev >= 0 && scan[sel_dev].confirmed);

	XtSetSensitive(refresh_btn, usable);
	XtSetSensitive(get_btn, usable && content_mode == CONTENT_SHARED);
	XtSetSensitive(put_btn, usable);
	XtSetSensitive(cd_btn, usable && content_mode == CONTENT_CDS);
}

/* ------------------------------------------------------------------ *
 * bus scan
 * ------------------------------------------------------------------ */

/*
 * Scan the bus and render it. Column widths are computed from the rows we
 * actually got - see fit_list_width() for why this isn't a fixed format.
 */
static void scan_bus(void)
{
	char paths[MAX_SCAN_DEVICES][SCSI_PATH_MAX];
	char line[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + 64];
	char widest[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + 64];
	char fmt[32];
	int count, i;
	int w_path = 0, w_type = 0;
	int len, best = 0;
	int answered = 0, found = 0, unconfirmed = 0;

	XmListDeleteAllItems(device_list_w);
	XmListDeleteAllItems(content_list_w);
	entries_n = 0;
	sel_dev = -1;
	scan_n = 0;
	widest[0] = '\0';

	count = scsi_enum_devices(paths, MAX_SCAN_DEVICES);
	if (count < 0) {
		set_status("Could not enumerate SCSI devices.");
		update_sensitivity();
		return;
	}
	if (count == 0) {
		set_status("No generic SCSI device nodes found.");
		update_sensitivity();
		return;
	}

	for (i = 0; i < count && scan_n < MAX_SCAN_DEVICES; i++) {
		if (toolbox_probe(paths[i], &scan[scan_n]) != 0)
			continue;
		scan_n++;
	}

	/* Pass 1: how wide does each column actually need to be? */
	for (i = 0; i < scan_n; i++) {
		len = (int)strlen(scan[i].path);
		if (len > w_path)
			w_path = len;
		len = (int)strlen(scan[i].type_name);
		if (len > w_type)
			w_type = len;
	}
	sprintf(fmt, "%%-%ds  %%-%ds  %%s%%s", w_path, w_type);

	/* Pass 2: render, and remember the longest row for the width preflight. */
	for (i = 0; i < scan_n; i++) {
		answered++;
		if (scan[i].confirmed)
			found++;
		else if (scan[i].claims)
			unconfirmed++;

		sprintf(line, fmt, scan[i].path, scan[i].type_name, scan[i].identity,
			scan[i].confirmed ? "  [TOOLBOX]" :
				(scan[i].claims ? "  [claims toolbox, no 0xD9 answer]" : ""));
		list_add(device_list_w, line);

		len = (int)strlen(line);
		if (len > best) {
			best = len;
			strcpy(widest, line);
		}
	}

	fit_list_width(device_list_w, widest);

	sprintf(line, "%d device(s) answered, %d toolbox-capable", answered, found);
	if (unconfirmed > 0)
		sprintf(line + strlen(line), " (%d claimed but failed 0xD9)", unconfirmed);
	strcat(line, ".");
	if (answered == 0 && geteuid() != 0)
		strcat(line, "  Not running as root - that is probably why.");
	else if (found > 0)
		strcat(line, "  Select one to work with it.");
	set_status(line);

	/* Preselect the first toolbox-capable device: it is almost always the
	 * one wanted, and it saves a click on a single-device bus. */
	for (i = 0; i < scan_n; i++) {
		if (scan[i].confirmed) {
			XmListSelectPos(device_list_w, i + 1, True);
			sel_dev = i;
			break;
		}
	}
	update_sensitivity();
}

/* ------------------------------------------------------------------ *
 * content pane: /shared listing and CD images
 * ------------------------------------------------------------------ */

static void refresh_content(void)
{
	char line[NAME_BUF_SIZE + 64];
	char widest[NAME_BUF_SIZE + 64];
	char fmt[48];
	int dev, i, n, len, best = 0;
	int w_name = 0;

	XmListDeleteAllItems(content_list_w);
	entries_n = 0;
	widest[0] = '\0';

	if (!require_toolbox())
		return;

	/* CD listings must open read-only; the shared dir does not. */
	dev = open_selected(content_mode == CONTENT_CDS);
	if (dev < 0)
		return;

	if (content_mode == CONTENT_CDS)
		n = toolbox_listcds(dev, entries, MAX_FILES);
	else
		n = toolbox_listfiles(dev, entries, MAX_FILES);
	scsi_close(dev);

	if (n < 0) {
		set_status(content_mode == CONTENT_CDS ?
			   "Could not list CD images." : "Could not list /shared.");
		show_msg("Listing failed",
			 content_mode == CONTENT_CDS ?
			 "TOOLBOX_LIST_CDS (0xD7) failed.\n\nIf this target is not an\n"
			 "emulated CD drive there is nothing to list." :
			 "TOOLBOX_LIST_FILES (0xD0) failed.", 1);
		return;
	}
	entries_n = n;

	for (i = 0; i < n; i++) {
		len = (int)strlen(entries[i].name);
		if (len > w_name)
			w_name = len;
	}
	sprintf(fmt, "#%%-3d %%-%ds  %%ld bytes", w_name + 1);

	for (i = 0; i < n; i++) {
		sprintf(line, fmt, entries[i].index, entries[i].name,
			size_to_long(entries[i].size));
		/* Directories get the same trailing marker the CLI uses. */
		if (entries[i].type == 1)
			strcat(line, "  (dir)");
		list_add(content_list_w, line);

		len = (int)strlen(line);
		if (len > best) {
			best = len;
			strcpy(widest, line);
		}
	}
	fit_list_width(content_list_w, widest);

	sprintf(line, "%d %s.", n, content_mode == CONTENT_CDS ? "CD image(s)" : "file(s) in /shared");
	set_status(line);
}

/* ------------------------------------------------------------------ *
 * operations
 * ------------------------------------------------------------------ */

static void do_interrogate(void)
{
	ToolboxDetect det;
	char text[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + 768];
	char ver[64];
	char claim[128];
	int dev, dbg, ret;

	if (sel_dev < 0) {
		show_msg("No device", "Select a device in the SCSI bus list first.", 1);
		return;
	}

	dev = open_selected(0);
	if (dev < 0)
		return;

	if (toolbox_identify(dev, &det) != TOOLBOX_OK) {
		scsi_close(dev);
		show_msg("Interrogate failed", "INQUIRY did not succeed on this device.", 1);
		return;
	}
	ret = toolbox_qualify(dev, &det);
	dbg = det.confirmed ? toolbox_getdebug(dev) : -1;
	scsi_close(dev);

	if (det.api_version >= 0)
		sprintf(ver, "%d%s", det.api_version,
			det.api_version < TOOLBOX_API_VER ? "  (older than this tool expects)" : "");
	else
		strcpy(ver, "not available (length mismatch)");

	if (!det.claims)
		strcpy(claim, "does not advertise the toolbox");
	else if (force_toolbox)
		strcpy(claim, "identity check skipped (-F)");
	else if (det.claimed_via_page31)
		strcpy(claim, "MODE SENSE page 0x31 magic");
	else if (det.claim_id != NULL)
		sprintf(claim, "INQUIRY firmware id '%s'", det.claim_id);
	else
		strcpy(claim, "yes");

	sprintf(text,
		"Device:     %s\n"
		"Identity:   %s\n"
		"Type:       %s\n"
		"SCSI ver:   %d\n\n"
		"Vendor:     %s\n"
		"Product:    %s\n"
		"Revision:   %s\n\n"
		"Toolbox API version: %s\n"
		"Claim:      %s\n"
		"Confirmed:  %s",
		scan[sel_dev].path,
		det.identity,
		inquiry_pdt_name(det.inq.dev_type),
		det.inq.version,
		det.inq.vendor_id,
		det.inq.product_id,
		det.inq.product_rev,
		ver,
		claim,
		ret == TOOLBOX_OK ? "yes - answered TOOLBOX_LIST_DEVICES (0xD9)" :
			(ret == TOOLBOX_ERR_NO_ANSWER ? "NO - claimed the toolbox but never answered 0xD9" :
							"NO - not a toolbox device"));

	if (dbg >= 0)
		sprintf(text + strlen(text), "\nDebug mode: %s", dbg ? "on" : "off");

	show_msg("Interrogate", text, 0);
	set_status("Interrogated the selected device.");
}

static void do_targets(void)
{
	unsigned char map[8];
	char text[512];
	int dev, i;

	if (!require_toolbox())
		return;
	dev = open_selected(0);
	if (dev < 0)
		return;

	if (toolbox_listdevices(dev, map) != 0) {
		scsi_close(dev);
		show_msg("Device map failed",
			 "Could not fetch the device-type map (0xD9).", 1);
		return;
	}
	scsi_close(dev);

	strcpy(text, "Emulated SCSI targets:\n\n");
	for (i = 0; i < 8; i++)
		sprintf(text + strlen(text), "    ID %d:  %s\n", i, dev_type_name(map[i]));

	show_msg("Emulated Targets", text, 0);
	set_status("Read the emulated device map.");
}

static void do_debug_show(void)
{
	char text[128];
	int dev, dbg;

	if (!require_toolbox())
		return;
	dev = open_selected(0);
	if (dev < 0)
		return;
	dbg = toolbox_getdebug(dev);
	scsi_close(dev);

	if (dbg < 0) {
		show_msg("Debug", "Could not read the debug flag.", 1);
		return;
	}
	sprintf(text, "Firmware debug logging is currently %s.", dbg ? "ON" : "OFF");
	show_msg("Debug", text, 0);
	set_status(text);
}

static void do_debug_set(int on)
{
	char text[160];
	int dev;

	if (!require_toolbox())
		return;
	dev = open_selected(0);
	if (dev < 0)
		return;
	if (toolbox_setdebug(dev, on) != 0) {
		scsi_close(dev);
		show_msg("Debug", "Could not set the debug flag.", 1);
		return;
	}
	scsi_close(dev);

	sprintf(text, "Firmware debug logging turned %s.%s", on ? "ON" : "OFF",
		on ? "\n\nNote: debug logging significantly slows the device down."
		   : "");
	show_msg("Debug", text, 0);
	set_status(on ? "Debug logging enabled." : "Debug logging disabled.");
}

/* ARGSUSED */
static void cd_force_ok_cb(Widget w, XtPointer client, XtPointer call)
{
	cd_swap_forced = 1;
	do_switch_cd();
	cd_swap_forced = 0;
}

/*
 * The volume would not unmount. Say who is holding it and make the operator
 * decide: proceeding swaps the image under a live mount, which is how the
 * mounted filesystem gets corrupted, so the default action is to back out.
 */
static void confirm_busy_swap(const char *mnt, const char *why)
{
	char text[768];
	char holders[384];
	XmString xs, xt;

	copy_clamped(holders, (int)sizeof(holders), why);

	sprintf(text,
		"%s is still mounted and could not be unmounted.\n\n"
		"%s%s"
		"Switching the image now risks corrupting that filesystem, and the\n"
		"new disc may not appear until it is unmounted.\n\n"
		"Close whatever is using it - a shell sitting in the directory counts -\n"
		"then try again.",
		mnt,
		holders[0] != '\0' ? "Still in use by:\n" : "",
		holders[0] != '\0' ? holders : "");

	if (cd_force_dialog == NULL) {
		cd_force_dialog = XmCreateQuestionDialog(toplevel, "cdForceDialog", NULL, 0);
		XtUnmanageChild(XmMessageBoxGetChild(cd_force_dialog, XmDIALOG_HELP_BUTTON));
		XtAddCallback(cd_force_dialog, XmNokCallback, cd_force_ok_cb, NULL);

		xs = XmStringCreateLtoR("Switch Anyway", XmSTRING_DEFAULT_CHARSET);
		XtVaSetValues(cd_force_dialog, XmNokLabelString, xs, NULL);
		XmStringFree(xs);
		xs = XmStringCreateLtoR("Cancel", XmSTRING_DEFAULT_CHARSET);
		XtVaSetValues(cd_force_dialog, XmNcancelLabelString, xs, NULL);
		XmStringFree(xs);

		/* Cancel is the safe answer, so it is the one that has focus. */
		XtVaSetValues(cd_force_dialog, XmNdefaultButtonType,
			      XmDIALOG_CANCEL_BUTTON, NULL);
	}

	xs = XmStringCreateLtoR(text, XmSTRING_DEFAULT_CHARSET);
	xt = XmStringCreateLtoR("Volume busy", XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(cd_force_dialog, XmNmessageString, xs, XmNdialogTitle, xt, NULL);
	XmStringFree(xs);
	XmStringFree(xt);

	XtManageChild(cd_force_dialog);
	set_status("CD not switched - the volume is still mounted.");
}

/* Switch the emulated CD drive to the image selected in the lower pane. */
static void do_switch_cd(void)
{
	char text[NAME_BUF_SIZE + 160];
	char mnt[SCSI_PATH_MAX];
	char why[512];
	int dev, row, scsi_id, swap;

	if (!require_toolbox())
		return;

	row = list_selection(content_list_w);
	if (row < 0 || row >= entries_n) {
		show_msg("No CD selected", "Select a CD image in the lower list first.", 1);
		return;
	}

	/* Same gate the CLI applies: the target has to actually be a CD. */
	scsi_id = path_to_devnum(scan[sel_dev].path);
	if (scsi_id < 0) {
		show_msg("Cannot switch CD",
			 "Could not read the SCSI target id from the device path.", 1);
		return;
	}
	if (device_list[scsi_id] != TYPE_CD) {
		sprintf(text, "SCSI ID %d is emulating a %s, not a CD drive.\n\n"
			      "Only a CD target can have its image switched.",
			scsi_id, dev_type_name(device_list[scsi_id]));
		show_msg("Not a CD drive", text, 1);
		return;
	}

	/*
	 * Pause IRIX's removable-media daemon for the swap. mediad watches the
	 * CD device and will fight an image change underneath it - this is the
	 * same guard the CLI puts around -c, and it matters on real hardware
	 * where mediad is running by default. Restarted on every exit path.
	 */
	mediad_stop();

	/*
	 * Now clear the mount. mediad -k unmounts what mediad itself mounted,
	 * but not a volume the operator mounted by hand - and swapping the image
	 * under a live mount is what leaves the host with cached metadata for a
	 * disc that is gone. Verified on hardware: the new disc does not appear
	 * until /CDROM is unmounted.
	 */
	swap = toolbox_prepare_cd_swap(scan[sel_dev].path, mnt, sizeof(mnt), why, sizeof(why));
	if (swap == CDSWAP_BUSY && !cd_swap_forced) {
		mediad_start();
		confirm_busy_swap(mnt, why);
		return;
	}

	dev = open_selected(1);
	if (dev < 0) {
		mediad_start();
		return;
	}
	if (toolbox_setnextcd(dev, entries[row].index) != 0) {
		scsi_close(dev);
		mediad_start();
		show_msg("Switch failed", "TOOLBOX_SET_NEXT_CD (0xD8) failed.", 1);
		return;
	}
	scsi_close(dev);
	mediad_start();

	sprintf(text, "Switched to CD image:\n\n    %s%s", entries[row].name,
		swap == CDSWAP_UNMOUNTED ? "\n\n(the old volume was unmounted first)" :
			(swap == CDSWAP_BUSY ? "\n\nWARNING: the old volume was still mounted." : ""));
	show_msg("CD switched", text, 0);
	set_status("CD image switched.");
}

/* ------------------------------------------------------------------ *
 * callbacks
 * ------------------------------------------------------------------ */

/* ARGSUSED */
static void device_select_cb(Widget w, XtPointer client, XtPointer call)
{
	XmListCallbackStruct *cbs = (XmListCallbackStruct *)call;
	char msg[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + 64];

	sel_dev = cbs->item_position - 1;
	if (sel_dev < 0 || sel_dev >= scan_n) {
		sel_dev = -1;
		update_sensitivity();
		return;
	}

	XmListDeleteAllItems(content_list_w);
	entries_n = 0;
	update_sensitivity();

	if (scan[sel_dev].confirmed)
		sprintf(msg, "Selected %s - toolbox ready.", scan[sel_dev].path);
	else if (scan[sel_dev].claims)
		sprintf(msg, "%s claims the toolbox but failed 0xD9 - not usable.",
			scan[sel_dev].path);
	else
		sprintf(msg, "%s is not a toolbox device.", scan[sel_dev].path);
	set_status(msg);
}

/* ARGSUSED */
static void rescan_cb(Widget w, XtPointer client, XtPointer call)
{
	set_status("Scanning...");
	scan_bus();
}

/* ARGSUSED */
static void quit_cb(Widget w, XtPointer client, XtPointer call)
{
	exit(0);
}

/* ARGSUSED */
static void mode_cb(Widget w, XtPointer client, XtPointer call)
{
	XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;

	if (!cbs->set)
		return;                 /* only react to the button being turned ON */
	content_mode = (int)(long)client;
	XmListDeleteAllItems(content_list_w);
	entries_n = 0;
	update_sensitivity();
	if (sel_dev >= 0 && scan[sel_dev].confirmed)
		refresh_content();
}

/* ARGSUSED */
static void force_cb(Widget w, XtPointer client, XtPointer call)
{
	XmToggleButtonCallbackStruct *cbs = (XmToggleButtonCallbackStruct *)call;

	force_toolbox = cbs->set ? 1 : 0;
	set_status(force_toolbox ?
		   "Force detection ON - rescanning; every device gets tested with 0xD9." :
		   "Force detection off - rescanning.");
	scan_bus();
}

/* ARGSUSED */
static void refresh_cb(Widget w, XtPointer client, XtPointer call)
{
	refresh_content();
}

/* ARGSUSED */
static void interrogate_cb(Widget w, XtPointer client, XtPointer call)
{
	do_interrogate();
}

/* ARGSUSED */
static void targets_cb(Widget w, XtPointer client, XtPointer call)
{
	do_targets();
}

/* ARGSUSED */
static void debug_show_cb(Widget w, XtPointer client, XtPointer call)
{
	do_debug_show();
}

/* ARGSUSED */
static void debug_set_cb(Widget w, XtPointer client, XtPointer call)
{
	do_debug_set((int)(long)client);
}

/* ARGSUSED */
static void switch_cd_cb(Widget w, XtPointer client, XtPointer call)
{
	do_switch_cd();
}

/* ARGSUSED */
static void about_cb(Widget w, XtPointer client, XtPointer call)
{
	char text[1024];

	sprintf(text,
		"scsitbgui - toolbox for emulated SCSI devices\n\n"
		"Motif front end for BlueSCSI / ZuluSCSI targets that\n"
		"implement the Toolbox API (SCSI vendor commands 0xD0-0xDA).\n\n"
		"A device is only driven as a toolbox target if it both\n"
		"advertises the API and answers TOOLBOX_LIST_DEVICES (0xD9);\n"
		"a claim on its own is never trusted.\n\n"
		"Shares its protocol core with the irixscsitb command-line tool.\n\n"
		"Revision:   %s\n"
		"Stamped:    %s\n"
		"Compiled:   %s\n"
		"Target:     %s\n"
		"Built on:   %s\n\n"
		"Project:    %s",
		build_revision(), build_stamp(), build_compiled(),
		build_abi(), build_libs(), PROJECT_URL);
	show_msg("About scsitbgui", text, 0);
}

/* Get File: OK on the output-directory prompt actually runs the transfer. */
/* ARGSUSED */
static void get_ok_cb(Widget w, XtPointer client, XtPointer call)
{
	XmSelectionBoxCallbackStruct *cbs = (XmSelectionBoxCallbackStruct *)call;
	char outdir[1024];
	char shown[256];
	char text[NAME_BUF_SIZE + 384];
	char *dir = NULL;
	int dev, row;

	row = list_selection(content_list_w);
	if (row < 0 || row >= entries_n)
		return;

	if (!XmStringGetLtoR(cbs->value, XmSTRING_DEFAULT_CHARSET, &dir) || dir == NULL)
		return;
	/* Leave room for the trailing '/' appended below. */
	copy_clamped(outdir, (int)sizeof(outdir) - 1, dir);
	XtFree(dir);

	/* toolbox_getfile() concatenates outdir + name directly, so make sure
	 * there is a separator; it defaults to "./" for anything too short. */
	if (strlen(outdir) > 1 && outdir[strlen(outdir) - 1] != '/')
		strcat(outdir, "/");

	dev = open_selected(0);
	if (dev < 0)
		return;
	if (toolbox_getfile(dev, entries[row].index, outdir) != 0) {
		scsi_close(dev);
		show_msg("Get failed",
			 "TOOLBOX_GET_FILE (0xD1) failed. Check the output directory\n"
			 "exists and is writable.", 1);
		return;
	}
	scsi_close(dev);

	copy_clamped(shown, (int)sizeof(shown), outdir);
	sprintf(text, "Fetched:\n\n    %s\n\ninto %s", entries[row].name, shown);
	show_msg("File fetched", text, 0);
	set_status("File fetched from /shared.");
}

/* ARGSUSED */
static void get_file_cb(Widget w, XtPointer client, XtPointer call)
{
	XmString xs, xt;
	int row;

	if (!require_toolbox())
		return;
	row = list_selection(content_list_w);
	if (row < 0 || row >= entries_n) {
		show_msg("No file selected",
			 "Select a file in the lower list first.\n\n"
			 "(Refresh the Shared files list if it is empty.)", 1);
		return;
	}

	if (dir_dialog == NULL) {
		dir_dialog = XmCreatePromptDialog(toplevel, "dirDialog", NULL, 0);
		XtUnmanageChild(XmSelectionBoxGetChild(dir_dialog, XmDIALOG_HELP_BUTTON));
		XtAddCallback(dir_dialog, XmNokCallback, get_ok_cb, NULL);
		xs = XmStringCreateLtoR("./", XmSTRING_DEFAULT_CHARSET);
		XtVaSetValues(dir_dialog, XmNtextString, xs, NULL);
		XmStringFree(xs);
	}

	xs = XmStringCreateLtoR("Save into which directory?", XmSTRING_DEFAULT_CHARSET);
	xt = XmStringCreateLtoR("Get File", XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(dir_dialog, XmNselectionLabelString, xs, XmNdialogTitle, xt, NULL);
	XmStringFree(xs);
	XmStringFree(xt);

	XtManageChild(dir_dialog);
}

/* Put File: OK on the file selection box sends the chosen local file. */
/* ARGSUSED */
static void put_ok_cb(Widget w, XtPointer client, XtPointer call)
{
	XmFileSelectionBoxCallbackStruct *cbs = (XmFileSelectionBoxCallbackStruct *)call;
	char shown[512];
	char text[576];
	char *path = NULL;
	int dev;

	if (!XmStringGetLtoR(cbs->value, XmSTRING_DEFAULT_CHARSET, &path) || path == NULL)
		return;

	dev = open_selected(0);
	if (dev < 0) {
		XtFree(path);
		return;
	}
	if (toolbox_sendfile(dev, path) != 0) {
		scsi_close(dev);
		show_msg("Put failed",
			 "The send sequence (0xD3/0xD4/0xD5) failed.\n\n"
			 "Names longer than 32 characters cannot be stored on the\n"
			 "device - that limit is structural to the protocol.", 1);
		XtFree(path);
		return;
	}
	scsi_close(dev);

	copy_clamped(shown, (int)sizeof(shown), path);
	XtFree(path);
	sprintf(text, "Sent:\n\n    %s\n\ninto the device's /shared directory.", shown);
	show_msg("File sent", text, 0);
	set_status("File sent to /shared.");

	/* The listing is now stale. */
	if (content_mode == CONTENT_SHARED)
		refresh_content();
}

/*
 * Give the file browser an explicit starting directory, with exactly one
 * trailing slash.
 *
 * XmNdirMask is XmNdirectory concatenated with XmNpattern, so the separator
 * has to be there - but only one of it. A directory that already ends in a
 * slash is what produces the doubled separators seen in the filter field and
 * the directory list.
 *
 * Setting it ourselves also sidesteps a platform hazard: left alone, Motif
 * derives the starting directory by calling getcwd(3), which on IRIX 5.3 fails
 * outright inside an IRIS NFS mount ("getcwd (bu5)."). $HOME is a better
 * default for "pick a file to send" anyway than wherever the binary was
 * launched from.
 */
static void set_dialog_start_dir(Widget dlg)
{
	char dir[1024];
	const char *home = getenv("HOME");
	XmString xs;
	int n;

	if (home == NULL || *home == '\0')
		home = "/";
	copy_clamped(dir, (int)sizeof(dir) - 2, home);

	n = (int)strlen(dir);
	while (n > 0 && dir[n - 1] == '/')
		dir[--n] = '\0';
	if (n == 0)
		strcpy(dir, "/");
	else
		strcat(dir, "/");

	xs = XmStringCreateLtoR(dir, XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(dlg, XmNdirectory, xs, NULL);
	XmStringFree(xs);

	xs = XmStringCreateLtoR("*", XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(dlg, XmNpattern, xs, NULL);
	XmStringFree(xs);
}

/* ARGSUSED */
static void put_file_cb(Widget w, XtPointer client, XtPointer call)
{
	XmString xs, xt;

	if (!require_toolbox())
		return;

	if (file_dialog == NULL) {
		file_dialog = XmCreateFileSelectionDialog(toplevel, "fileDialog", NULL, 0);
		XtUnmanageChild(XmFileSelectionBoxGetChild(file_dialog, XmDIALOG_HELP_BUTTON));
		XtAddCallback(file_dialog, XmNokCallback, put_ok_cb, NULL);
		/* Cancel needs no callback: dialogs default to XmNautoUnmanage
		 * True, so they take themselves down. It is relabelled "Close"
		 * because that is what the button does here - the dialog is a
		 * browser you are done with, not an operation being aborted. */
		xs = XmStringCreateLtoR("Close", XmSTRING_DEFAULT_CHARSET);
		XtVaSetValues(file_dialog, XmNcancelLabelString, xs, NULL);
		XmStringFree(xs);

		set_dialog_start_dir(file_dialog);
	}
	xt = XmStringCreateLtoR("Put File into /shared", XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(file_dialog, XmNdialogTitle, xt, NULL);
	XmStringFree(xt);

	XtManageChild(file_dialog);
}

/* ------------------------------------------------------------------ *
 * widget tree
 * ------------------------------------------------------------------ */

static void build_menu(Widget menubar)
{
	Widget pane, item;

	/* --- File --- */
	pane = XmCreatePulldownMenu(menubar, "filePane", NULL, 0);
	XtVaCreateManagedWidget("file", xmCascadeButtonWidgetClass, menubar,
				XmNmnemonic, 'F', XmNsubMenuId, pane, NULL);

	item = XtVaCreateManagedWidget("rescan", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, rescan_cb, NULL);
	XtVaCreateManagedWidget("sep1", xmSeparatorWidgetClass, pane, NULL);
	item = XtVaCreateManagedWidget("quit", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, quit_cb, NULL);

	/* --- Device --- */
	pane = XmCreatePulldownMenu(menubar, "devicePane", NULL, 0);
	XtVaCreateManagedWidget("device", xmCascadeButtonWidgetClass, menubar,
				XmNmnemonic, 'D', XmNsubMenuId, pane, NULL);

	item = XtVaCreateManagedWidget("interrogate", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, interrogate_cb, NULL);
	item = XtVaCreateManagedWidget("targets", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, targets_cb, NULL);

	XtVaCreateManagedWidget("sep2", xmSeparatorWidgetClass, pane, NULL);

	item = XtVaCreateManagedWidget("debugShow", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, debug_show_cb, NULL);
	item = XtVaCreateManagedWidget("debugOn", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, debug_set_cb, (XtPointer)1);
	item = XtVaCreateManagedWidget("debugOff", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, debug_set_cb, (XtPointer)0);

	XtVaCreateManagedWidget("sep3", xmSeparatorWidgetClass, pane, NULL);

	/* Same escape hatch as the CLI's -F: test firmware we don't know by
	 * name by issuing a real toolbox command instead of trusting INQUIRY. */
	item = XtVaCreateManagedWidget("forceToggle", xmToggleButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNvalueChangedCallback, force_cb, NULL);

	/* --- Help --- */
	pane = XmCreatePulldownMenu(menubar, "helpPane", NULL, 0);
	item = XtVaCreateManagedWidget("help", xmCascadeButtonWidgetClass, menubar,
				       XmNmnemonic, 'H', XmNsubMenuId, pane, NULL);
	/* Convention: the Help cascade sits at the right-hand end of the bar. */
	XtVaSetValues(menubar, XmNmenuHelpWidget, item, NULL);

	item = XtVaCreateManagedWidget("about", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, about_cb, NULL);
}

/* Upper pane: the bus. */
static void build_bus_pane(Widget parent)
{
	Widget frame, list;
	Arg args[4];
	int n;

	frame = XtVaCreateManagedWidget("busFrame", xmFrameWidgetClass, parent, NULL);
	XtVaCreateManagedWidget("busFrameLabel", xmLabelWidgetClass, frame,
				XmNchildType, XmFRAME_TITLE_CHILD, NULL);

	n = 0;
	/* XmCONSTANT: a row wider than the list scrolls sideways instead of
	 * forcing the whole window wider behind the user's back. */
	XtSetArg(args[n], XmNlistSizePolicy, XmCONSTANT); n++;
	list = XmCreateScrolledList(frame, "deviceList", args, n);
	device_list_w = list;
	XtAddCallback(list, XmNbrowseSelectionCallback, device_select_cb, NULL);
	XtManageChild(list);
}

/* Lower pane: /shared or CD images, plus the buttons that act on them. */
static void build_content_pane(Widget parent)
{
	Widget form, radio, b, sw, list;
	Arg args[4];
	int n;

	form = XtVaCreateManagedWidget("contentForm", xmFormWidgetClass, parent, NULL);

	radio = XmCreateRadioBox(form, "modeBox", NULL, 0);
	XtVaSetValues(radio,
		      XmNorientation, XmHORIZONTAL,
		      XmNtopAttachment, XmATTACH_FORM,
		      XmNleftAttachment, XmATTACH_FORM,
		      NULL);
	b = XtVaCreateManagedWidget("modeShared", xmToggleButtonWidgetClass, radio,
				    XmNset, True, NULL);
	XtAddCallback(b, XmNvalueChangedCallback, mode_cb, (XtPointer)CONTENT_SHARED);
	b = XtVaCreateManagedWidget("modeCds", xmToggleButtonWidgetClass, radio, NULL);
	XtAddCallback(b, XmNvalueChangedCallback, mode_cb, (XtPointer)CONTENT_CDS);
	XtManageChild(radio);

	/* Button row along the bottom. */
	refresh_btn = XtVaCreateManagedWidget("refresh", xmPushButtonWidgetClass, form,
					      XmNbottomAttachment, XmATTACH_FORM,
					      XmNleftAttachment, XmATTACH_FORM,
					      NULL);
	XtAddCallback(refresh_btn, XmNactivateCallback, refresh_cb, NULL);

	get_btn = XtVaCreateManagedWidget("getFile", xmPushButtonWidgetClass, form,
					  XmNbottomAttachment, XmATTACH_FORM,
					  XmNleftAttachment, XmATTACH_WIDGET,
					  XmNleftWidget, refresh_btn,
					  NULL);
	XtAddCallback(get_btn, XmNactivateCallback, get_file_cb, NULL);

	put_btn = XtVaCreateManagedWidget("putFile", xmPushButtonWidgetClass, form,
					  XmNbottomAttachment, XmATTACH_FORM,
					  XmNleftAttachment, XmATTACH_WIDGET,
					  XmNleftWidget, get_btn,
					  NULL);
	XtAddCallback(put_btn, XmNactivateCallback, put_file_cb, NULL);

	cd_btn = XtVaCreateManagedWidget("switchCd", xmPushButtonWidgetClass, form,
					 XmNbottomAttachment, XmATTACH_FORM,
					 XmNleftAttachment, XmATTACH_WIDGET,
					 XmNleftWidget, put_btn,
					 NULL);
	XtAddCallback(cd_btn, XmNactivateCallback, switch_cd_cb, NULL);

	n = 0;
	XtSetArg(args[n], XmNlistSizePolicy, XmCONSTANT); n++;
	list = XmCreateScrolledList(form, "contentList", args, n);
	content_list_w = list;
	sw = XtParent(list);
	XtVaSetValues(sw,
		      XmNtopAttachment, XmATTACH_WIDGET,
		      XmNtopWidget, radio,
		      XmNleftAttachment, XmATTACH_FORM,
		      XmNrightAttachment, XmATTACH_FORM,
		      XmNbottomAttachment, XmATTACH_WIDGET,
		      XmNbottomWidget, refresh_btn,
		      NULL);
	XtManageChild(list);
}

int main(int argc, char *argv[])
{
	XtAppContext app;
	Widget mainw, menubar, paned;
	XtArgcType xargc;
	int i;

	xargc = (XtArgcType)argc;
	toplevel = XtVaAppInitialize(&app, "Scsitbgui", NULL, 0,
				     &xargc, argv, fallback_resources, NULL);
	argc = (int)xargc;

	/* Xt has stripped its own options (-display, -geometry, ...); take the
	 * two of ours that change how detection behaves. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
		else if (strcmp(argv[i], "-F") == 0)
			force_toolbox = 1;
	}

	set_window_icon(toplevel);

	mainw = XmCreateMainWindow(toplevel, "mainw", NULL, 0);
	XtManageChild(mainw);

	menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
	XtManageChild(menubar);
	build_menu(menubar);

	paned = XtVaCreateManagedWidget("paned", xmPanedWindowWidgetClass, mainw, NULL);
	build_bus_pane(paned);
	build_content_pane(paned);

	status_w = XtVaCreateManagedWidget("status", xmLabelWidgetClass, mainw,
					   XmNalignment, XmALIGNMENT_BEGINNING, NULL);

	XmMainWindowSetAreas(mainw, menubar, NULL, NULL, NULL, paned);
	XtVaSetValues(mainw, XmNmessageWindow, status_w, NULL);

	/*
	 * Scan BEFORE realizing. The width preflight measures the rows we
	 * actually got, so the window opens already the right size instead of
	 * appearing at a guessed size and then jumping.
	 */
	scan_bus();

	XtRealizeWidget(toplevel);
	XtAppMainLoop(app);
	return 0;
}
