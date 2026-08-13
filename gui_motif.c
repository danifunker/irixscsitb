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
#include <Xm/TextF.h>
#include <X11/Shell.h>
#include <X11/cursorfont.h>

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
	"*wifi.labelString: Wi-Fi",
	"*wifiScan.labelString: Scan For Networks",
	"*wifiInfo.labelString: Current Network...",
	"*wifiJoinItem.labelString: Join Network...",
	"*help.labelString: Help",
	"*about.labelString: About...",
	"*busFrameLabel.labelString: SCSI bus",
	"*modeShared.labelString: Shared files",
	"*modeCds.labelString: CD images",
	"*modeWifi.labelString: Wi-Fi networks",
	"*refresh.labelString: Refresh",
	"*getFile.labelString: Get File...",
	"*putFile.labelString: Put File...",
	"*switchCd.labelString: Switch To CD",
	"*joinWifi.labelString: Join...",
	"*rescanButton.labelString: Rescan",
	"*status.labelString: Ready.",
	/* The join dialog's own labels, so they can be re-worded or localised
	 * from app-defaults like every other string in this file. */
	"*joinSsidLabel.labelString: Network name (SSID):",
	"*joinKeyLabel.labelString: Password (leave blank if open):",
	"*joinOk.labelString: Join",
	"*joinCancel.labelString: Cancel",
	NULL
};

/*
 * Window-manager icon: the SCSI mark - a diamond outline opening to the right
 * onto a bar, which is itself a transfer glyph, so it says both "SCSI" and
 * "moves data" in one shape at 32x32.
 *
 * A CD was tried alongside it and dropped: at this size the disc collides with
 * the diamond's edge and the whole thing turns to noise. The mark alone reads
 * instantly; a second element needs 48x48.
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
   0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xc0, 0x03, 0x00,
   0x00, 0xe0, 0x07, 0x00, 0x00, 0xf0, 0x0f, 0x00, 0x00, 0x78, 0x1e, 0x00,
   0x00, 0x3c, 0x3c, 0x00, 0x00, 0x1e, 0x78, 0x00, 0x00, 0x0f, 0xf0, 0x00,
   0x80, 0x07, 0xe0, 0x01, 0xc0, 0x03, 0xc0, 0x03, 0xe0, 0x01, 0x80, 0x07,
   0xf0, 0x00, 0x00, 0x0f, 0x78, 0xe0, 0xff, 0xff, 0x3c, 0xf0, 0xff, 0xff,
   0x1e, 0xf0, 0xff, 0xff, 0x1e, 0xf0, 0xff, 0xff, 0x3c, 0xf0, 0xff, 0xff,
   0x78, 0xe0, 0xff, 0xff, 0xf0, 0x00, 0x00, 0x0f, 0xe0, 0x01, 0x80, 0x07,
   0xc0, 0x03, 0xc0, 0x03, 0x80, 0x07, 0xe0, 0x01, 0x00, 0x0f, 0xf0, 0x00,
   0x00, 0x1e, 0x78, 0x00, 0x00, 0x3c, 0x3c, 0x00, 0x00, 0x78, 0x1e, 0x00,
   0x00, 0xf0, 0x0f, 0x00, 0x00, 0xe0, 0x07, 0x00, 0x00, 0xc0, 0x03, 0x00,
   0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void set_window_icon(Widget shell)
{
	Display *dpy = XtDisplay(shell);
	Pixmap pm;

	if (dpy == NULL)
		return;
	/*
	 * Depth 1 on purpose. ICCCM allows WM_HINTS.icon_pixmap to be depth 1 or
	 * the screen depth, but 4Dwm carries a "color icon pixmap not supported"
	 * error string, so the bitmap is the safe form for the WM.
	 */
	pm = XCreateBitmapFromData(dpy, RootWindow(dpy, DefaultScreen(dpy)),
				   (char *)icon_bits, ICON_WIDTH, ICON_HEIGHT);
	if (pm == None)
		return;
	XtVaSetValues(shell, XtNiconPixmap, pm, XtNiconName, "scsitbgui", NULL);
}

/*
 * The same artwork as a SCREEN-DEPTH pixmap, for drawing inside the app - a
 * Motif XmNsymbolPixmap will not take a depth-1 bitmap. Foreground/background
 * come from the widget it is going into so it tracks the current scheme
 * instead of being hardcoded black on white.
 */
static Pixmap icon_pixmap_for(Widget w)
{
	Display *dpy = XtDisplay(w);
	Screen *scr;
	Pixel fg = 0, bg = 0;

	if (dpy == NULL)
		return None;
	scr = XtScreen(w);
	XtVaGetValues(w, XmNforeground, &fg, XmNbackground, &bg, NULL);
	return XCreatePixmapFromBitmapData(dpy, RootWindowOfScreen(scr),
					   (char *)icon_bits, ICON_WIDTH, ICON_HEIGHT,
					   fg, bg, (unsigned int)DefaultDepthOfScreen(scr));
}

/* Which listing the lower pane is showing. */
#define CONTENT_SHARED 0
#define CONTENT_CDS    1
#define CONTENT_WIFI   2

static Widget toplevel;
static Widget device_list_w;    /* upper pane: devices on the bus */
static Widget content_list_w;   /* lower pane: /shared, CD images or Wi-Fi */
static Widget status_w;
static Widget get_btn, put_btn, cd_btn, join_btn, refresh_btn;
static Widget mode_wifi_btn;    /* so the Wi-Fi menu can flip the lower pane */
static Widget info_dialog, error_dialog, dir_dialog, file_dialog;

static ToolboxScanEntry scan[MAX_SCAN_DEVICES];
static int scan_n;
static int sel_dev = -1;        /* index into scan[], -1 = nothing selected */

static ToolboxFileEntry entries[MAX_FILES];
static int entries_n;
static int content_mode = CONTENT_SHARED;

/* Results of the last Wi-Fi scan, shown when content_mode is CONTENT_WIFI. */
static ToolboxWifiNetwork wifi_nets[WIFI_MAX_NETWORKS];
static int wifi_nets_n;

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
			Pixmap pm;

			info_dialog = XmCreateInformationDialog(toplevel, "infoDialog", NULL, 0);
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_CANCEL_BUTTON));
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_HELP_BUTTON));

			/* Replace Motif's generic "i" with the SCSI mark, so the
			 * artwork is visible in the app and not only when the
			 * window happens to be iconified. */
			pm = icon_pixmap_for(info_dialog);
			if (pm != None)
				XtVaSetValues(info_dialog, XmNsymbolPixmap, pm, NULL);
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

/*
 * Which bus row carries the Wi-Fi radio.
 *
 * This is a SEPARATE question from "which device is selected", and has to be,
 * because the two never coincide: the firmware answers the Wi-Fi commands on
 * its emulated network target and the toolbox commands on the disk, so the row
 * the user picked to browse /shared is by definition not the row that can scan
 * for access points. Honour an explicit pick of the Wi-Fi row if there is one,
 * otherwise just find it - making the operator hunt for it would be asking them
 * to know something only the firmware's config file says.
 */
static int wifi_index(void)
{
	int i;

	if (sel_dev >= 0 && sel_dev < scan_n && scan[sel_dev].wifi)
		return sel_dev;
	for (i = 0; i < scan_n; i++)
		if (scan[i].wifi)
			return i;
	return -1;
}

/* Guard for the Wi-Fi operations, the counterpart of require_toolbox(). */
static int require_wifi(void)
{
	if (wifi_index() >= 0)
		return 1;

	show_msg("No Wi-Fi device",
		 "No Wi-Fi device was found on the SCSI bus.\n\n"
		 "The Wi-Fi commands are answered by the emulated NETWORK\n"
		 "target - a DaynaPort SCSI/Link - and not by the disk or CD\n"
		 "the same board is emulating. Check that a network device is\n"
		 "enabled in the firmware's configuration and that the board\n"
		 "has a radio, then rescan the bus.\n\n"
		 "Device > Emulated Targets... lists what the firmware is\n"
		 "emulating on each SCSI ID.", 1);
	return 0;
}

/* Open the Wi-Fi target. Only Join writes; scan and info are read-only. */
static int open_wifi(int readonly)
{
	char msg[SCSI_PATH_MAX + 256];
	int idx = wifi_index();
	int dev;

	if (idx < 0)
		return -1;

	dev = scsi_open(scan[idx].path, readonly);
	if (dev < 0 && !readonly)
		dev = scsi_open(scan[idx].path, 1);
	if (dev < 0) {
		sprintf(msg, "Cannot open the Wi-Fi device %s.\n\n%s", scan[idx].path,
			geteuid() == 0 ? "The device refused to open." :
					 "You are not running as root - that is almost\n"
					 "certainly why. The /dev/scsi nodes are root-owned.");
		show_msg("Open failed", msg, 1);
		return -1;
	}
	return dev;
}

/*
 * Say we are busy, and mean it.
 *
 * A Wi-Fi scan is seconds of real radio time and the core blocks for all of
 * it, so the event loop stops: without this the window simply freezes mid-click
 * with no explanation. The status text is pushed out with XmUpdateDisplay()
 * because the expose that would normally paint it cannot be processed until we
 * come back.
 */
static void set_busy(const char *text, int busy)
{
	static Cursor watch_cursor = None;
	Display *dpy = XtDisplay(toplevel);

	if (text != NULL)
		set_status(text);

	if (dpy != NULL && XtIsRealized(toplevel)) {
		if (busy) {
			if (watch_cursor == None)
				watch_cursor = XCreateFontCursor(dpy, XC_watch);
			if (watch_cursor != None)
				XDefineCursor(dpy, XtWindow(toplevel), watch_cursor);
		} else {
			XUndefineCursor(dpy, XtWindow(toplevel));
		}
	}

	XmUpdateDisplay(toplevel);
}

/* Grey out the operation buttons unless the selection can actually do them. */
static void update_sensitivity(void)
{
	int usable = (sel_dev >= 0 && scan[sel_dev].confirmed);
	int has_wifi = (wifi_index() >= 0);

	/* Refresh means "re-read the lower pane", so in Wi-Fi mode it is the
	 * Wi-Fi device that has to be present, not a toolbox one. */
	XtSetSensitive(refresh_btn, content_mode == CONTENT_WIFI ? has_wifi : usable);
	XtSetSensitive(get_btn, usable && content_mode == CONTENT_SHARED);
	XtSetSensitive(put_btn, usable);
	XtSetSensitive(cd_btn, usable && content_mode == CONTENT_CDS);
	XtSetSensitive(join_btn, has_wifi);
}

/* ------------------------------------------------------------------ *
 * bus scan
 * ------------------------------------------------------------------ */

/*
 * The markers on one bus row. Kept identical to the CLI's scan_tags(): the two
 * front ends must describe a device the same way, and the two questions -
 * "does it implement the toolbox" and "does it have a radio" - are independent,
 * so they accumulate rather than compete.
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
 * Scan the bus and render it. Column widths are computed from the rows we
 * actually got - see fit_list_width() for why this isn't a fixed format.
 */
static void scan_bus(void)
{
	char paths[MAX_SCAN_DEVICES][SCSI_PATH_MAX];
	char line[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + SCAN_TAG_MAX];
	char widest[SCSI_PATH_MAX + TOOLBOX_IDENTITY_MAX + SCAN_TAG_MAX];
	char tags[SCAN_TAG_MAX];
	char fmt[32];
	int count, i;
	int w_path = 0, w_type = 0;
	int len, best = 0;
	int answered = 0, found = 0, unconfirmed = 0, wifi = 0;

	XmListDeleteAllItems(device_list_w);
	XmListDeleteAllItems(content_list_w);
	entries_n = 0;
	wifi_nets_n = 0;
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
		if (scan[i].wifi)
			wifi++;

		scan_tags(&scan[i], tags);
		sprintf(line, fmt, scan[i].path, scan[i].type_name, scan[i].identity, tags);
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
	if (wifi > 0)
		sprintf(line + strlen(line), ", %d with Wi-Fi", wifi);
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

/*
 * One rendered row per Wi-Fi network. The bar meter is there so the list can be
 * read without knowing that -30 dBm beats -80; the dBm is kept alongside it
 * because it is the number you compare between rows.
 */
static void wifi_row(const ToolboxWifiNetwork *net, char *out, int ssid_width)
{
	char bssid[18];
	char bars[8];
	char fmt[64];
	int n, i;

	wifi_bssid_str(net->bssid, bssid, sizeof(bssid));

	n = wifi_signal_bars(net->rssi);
	for (i = 0; i < 4; i++)
		bars[i] = (i < n) ? '#' : '.';
	bars[4] = '\0';

	sprintf(fmt, "%%-%ds  %%s  %%4d dBm  ch %%-3d  %%-8s  %%s", ssid_width);
	sprintf(out, fmt, net->ssid, bars, net->rssi, net->channel,
		wifi_auth_name(net->flags), bssid);
}

/*
 * Scan for networks and show them in the lower pane. Blocking, by several
 * seconds - see set_busy().
 */
static void refresh_wifi(void)
{
	char line[WIFI_SSID_MAX + 96];
	char widest[WIFI_SSID_MAX + 96];
	int dev, i, n, len, best = 0;
	int w_ssid = 0;

	XmListDeleteAllItems(content_list_w);
	wifi_nets_n = 0;
	widest[0] = '\0';

	if (!require_wifi())
		return;
	dev = open_wifi(1);
	if (dev < 0)
		return;

	set_busy("Scanning for Wi-Fi networks - this takes a few seconds...", 1);
	n = (toolbox_wifi_scan(dev, WIFI_SCAN_TIMEOUT_SEC) == 0)
		? toolbox_wifi_results(dev, wifi_nets, WIFI_MAX_NETWORKS)
		: -1;
	scsi_close(dev);
	set_busy(NULL, 0);

	if (n < 0) {
		set_status("Wi-Fi scan failed.");
		show_msg("Scan failed",
			 "The Wi-Fi scan did not complete.\n\n"
			 "The device accepted the scan command but never reported it\n"
			 "finished, or refused to return the results. If the board has\n"
			 "no radio fitted this is what that looks like.", 1);
		return;
	}
	wifi_nets_n = n;

	for (i = 0; i < n; i++) {
		len = (int)strlen(wifi_nets[i].ssid);
		if (len > w_ssid)
			w_ssid = len;
	}

	for (i = 0; i < n; i++) {
		wifi_row(&wifi_nets[i], line, w_ssid);
		list_add(content_list_w, line);

		len = (int)strlen(line);
		if (len > best) {
			best = len;
			strcpy(widest, line);
		}
	}
	fit_list_width(content_list_w, widest);

	if (n == 0)
		set_status("No Wi-Fi networks found.");
	else {
		sprintf(line, "%d Wi-Fi network(s). Select one and press Join.", n);
		set_status(line);
	}
}

static void refresh_content(void)
{
	char line[NAME_BUF_SIZE + 64];
	char widest[NAME_BUF_SIZE + 64];
	char fmt[48];
	int dev, i, n, len, best = 0;
	int w_name = 0;

	/* Wi-Fi is a different target with a different gate, so it branches
	 * before require_toolbox() - which would (correctly) reject the network
	 * device for not implementing 0xD0-0xDA. */
	if (content_mode == CONTENT_WIFI) {
		refresh_wifi();
		return;
	}

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
 * Wi-Fi
 * ------------------------------------------------------------------ */

static Widget join_dialog, join_ssid_w, join_key_w;

/*
 * The password as actually typed. The text field itself only ever holds
 * asterisks - see join_key_modify_cb() - so this is where the real characters
 * live, and it is cleared as soon as the join has been sent.
 */
static char join_key_text[WIFI_KEY_MAX + 2];

/* Device > Wi-Fi > Current Network. */
static void do_wifi_info(void)
{
	ToolboxWifiNetwork net;
	char text[WIFI_SSID_MAX + 384];
	char bssid[18];
	int dev, idx;

	if (!require_wifi())
		return;
	idx = wifi_index();
	dev = open_wifi(1);
	if (dev < 0)
		return;

	if (toolbox_wifi_info(dev, &net) != 0) {
		scsi_close(dev);
		show_msg("Wi-Fi", "Could not read the current Wi-Fi network (0x1C/0x04).", 1);
		return;
	}
	scsi_close(dev);

	if (net.ssid[0] == '\0') {
		sprintf(text, "The Wi-Fi device at\n\n    %s\n\n"
			      "is not joined to any network.\n\n"
			      "Switch the lower pane to \"Wi-Fi networks\" and press\n"
			      "Refresh to scan, then select one and press Join.",
			scan[idx].path);
		show_msg("Wi-Fi", text, 0);
		set_status("Not joined to a Wi-Fi network.");
		return;
	}

	wifi_bssid_str(net.bssid, bssid, sizeof(bssid));
	sprintf(text,
		"Device:     %s\n\n"
		"Network:    %s\n"
		"BSSID:      %s\n"
		"Signal:     %d dBm  (%d of 4 bars)\n"
		"Channel:    %d\n"
		"Security:   %s",
		scan[idx].path, net.ssid, bssid, net.rssi,
		wifi_signal_bars(net.rssi), net.channel, wifi_auth_name(net.flags));

	show_msg("Current Wi-Fi Network", text, 0);
	set_status("Read the current Wi-Fi network.");
}

/*
 * Send a join request and then find out whether it took.
 *
 * The firmware answers the JOIN command as soon as it has handed the
 * credentials to the radio - GOOD means "request accepted", not "associated" -
 * so the only honest way to report success is to wait and then ask what the
 * radio is actually joined to.
 */
static void do_wifi_join(const char *ssid, const char *key)
{
	ToolboxWifiNetwork net;
	char text[(2 * WIFI_SSID_MAX) + 384];
	char shown[WIFI_SSID_MAX + 1];
	int dev;

	if (!require_wifi())
		return;

	copy_clamped(shown, (int)sizeof(shown), ssid);
	if (shown[0] == '\0') {
		show_msg("Join Wi-Fi", "Enter the name of the network to join.", 1);
		return;
	}

	dev = open_wifi(0);
	if (dev < 0)
		return;

	if (toolbox_wifi_join(dev, ssid, key, 0) != 0) {
		scsi_close(dev);
		show_msg("Join failed",
			 "The device rejected the join request (0x1C/0x05).\n\n"
			 "The network name and password are limited to 63\n"
			 "characters each by the protocol.", 1);
		return;
	}

	set_busy("Joining - waiting for the radio to associate...", 1);
	sleep(5);
	if (toolbox_wifi_info(dev, &net) != 0)
		net.ssid[0] = '\0';
	scsi_close(dev);
	set_busy(NULL, 0);

	if (net.ssid[0] == '\0') {
		sprintf(text, "The join request for\n\n    %s\n\nwas accepted, but the device is not "
			      "associated yet.\n\nCheck the password, or look again in a few seconds with\n"
			      "Wi-Fi > Current Network...", shown);
		show_msg("Not joined yet", text, 1);
		set_status("Join request sent; not associated yet.");
		return;
	}
	if (strcmp(net.ssid, ssid) != 0) {
		sprintf(text, "The device is still joined to\n\n    %s\n\nrather than the network "
			      "requested. The join was not taken.", net.ssid);
		show_msg("Not joined", text, 1);
		set_status("Join request sent; the device kept its old network.");
		return;
	}

	sprintf(text, "Joined:\n\n    %s\n\nSignal %d dBm on channel %d (%s).",
		net.ssid, net.rssi, net.channel, wifi_auth_name(net.flags));
	show_msg("Joined", text, 0);
	set_status("Joined the Wi-Fi network.");
}

/*
 * Echo the password field as asterisks.
 *
 * Motif 1.2 has no password widget, so the field is kept in step by hand: the
 * verify callback is told exactly which span of the visible text is being
 * replaced and with what, so applying the identical edit to join_key_text keeps
 * the two in sync for inserts, deletes, replacements and pastes alike - and
 * then the inserted characters are overwritten with '*' before Motif draws
 * them. Rejecting the edit outright is the safe response to anything that
 * doesn't add up, since a mismatch would mean sending a password nobody typed.
 */
/* ARGSUSED */
static void join_key_modify_cb(Widget w, XtPointer client, XtPointer call)
{
	XmTextVerifyCallbackStruct *cbs = (XmTextVerifyCallbackStruct *)call;
	char tail[WIFI_KEY_MAX + 2];
	int len = (int)strlen(join_key_text);
	int start = (int)cbs->startPos;
	int end = (int)cbs->endPos;
	int ins = (cbs->text != NULL) ? (int)cbs->text->length : 0;
	int i;

	if (start < 0 || end < start || start > len || end > len) {
		cbs->doit = False;
		return;
	}
	/* Same 63 the field's XmNmaxLength enforces and toolbox_wifi_join()
	 * insists on - the wire field is 64 bytes including its terminator. */
	if (len - (end - start) + ins > WIFI_KEY_MAX - 1) {
		cbs->doit = False;
		return;
	}

	copy_clamped(tail, (int)sizeof(tail), join_key_text + end);
	if (ins > 0)
		memcpy(join_key_text + start, cbs->text->ptr, ins);
	strcpy(join_key_text + start + ins, tail);

	for (i = 0; i < ins; i++)
		cbs->text->ptr[i] = '*';
}

/* ARGSUSED */
static void join_ok_cb(Widget w, XtPointer client, XtPointer call)
{
	char ssid[WIFI_SSID_MAX + 2];
	char key[WIFI_KEY_MAX + 2];
	char *typed;

	typed = XmTextFieldGetString(join_ssid_w);
	copy_clamped(ssid, (int)sizeof(ssid), typed != NULL ? typed : "");
	if (typed != NULL)
		XtFree(typed);

	copy_clamped(key, (int)sizeof(key), join_key_text);

	XtUnmanageChild(join_dialog);

	/* Don't leave the password sitting in the widget tree afterwards. */
	memset(join_key_text, 0, sizeof(join_key_text));
	XmTextFieldSetString(join_key_w, "");

	do_wifi_join(ssid, key);
	memset(key, 0, sizeof(key));
}

/* ARGSUSED */
static void join_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
	memset(join_key_text, 0, sizeof(join_key_text));
	XmTextFieldSetString(join_key_w, "");
	XtUnmanageChild(join_dialog);
}

/*
 * The join dialog: two fields and two buttons.
 *
 * Built as a plain form dialog rather than an XmPromptDialog because a prompt
 * only has one text field and this needs two - and a network name with no way
 * to type the password beside it would be a dialog you cannot finish.
 */
static void build_join_dialog(void)
{
	Widget l1, l2, sep, ok, cancel;

	join_dialog = XmCreateFormDialog(toplevel, "joinDialog", NULL, 0);
	XtVaSetValues(join_dialog, XmNautoUnmanage, False, NULL);

	l1 = XtVaCreateManagedWidget("joinSsidLabel", xmLabelWidgetClass, join_dialog,
				     XmNtopAttachment, XmATTACH_FORM,
				     XmNleftAttachment, XmATTACH_FORM,
				     XmNalignment, XmALIGNMENT_BEGINNING,
				     NULL);
	join_ssid_w = XtVaCreateManagedWidget("joinSsid", xmTextFieldWidgetClass, join_dialog,
					      XmNtopAttachment, XmATTACH_WIDGET,
					      XmNtopWidget, l1,
					      XmNleftAttachment, XmATTACH_FORM,
					      XmNrightAttachment, XmATTACH_FORM,
					      XmNcolumns, 28,
					      XmNmaxLength, WIFI_SSID_MAX - 1,
					      NULL);

	l2 = XtVaCreateManagedWidget("joinKeyLabel", xmLabelWidgetClass, join_dialog,
				     XmNtopAttachment, XmATTACH_WIDGET,
				     XmNtopWidget, join_ssid_w,
				     XmNleftAttachment, XmATTACH_FORM,
				     XmNalignment, XmALIGNMENT_BEGINNING,
				     NULL);
	join_key_w = XtVaCreateManagedWidget("joinKey", xmTextFieldWidgetClass, join_dialog,
					     XmNtopAttachment, XmATTACH_WIDGET,
					     XmNtopWidget, l2,
					     XmNleftAttachment, XmATTACH_FORM,
					     XmNrightAttachment, XmATTACH_FORM,
					     XmNcolumns, 28,
					     XmNmaxLength, WIFI_KEY_MAX - 1,
					     NULL);
	XtAddCallback(join_key_w, XmNmodifyVerifyCallback, join_key_modify_cb, NULL);

	sep = XtVaCreateManagedWidget("joinSep", xmSeparatorWidgetClass, join_dialog,
				      XmNtopAttachment, XmATTACH_WIDGET,
				      XmNtopWidget, join_key_w,
				      XmNleftAttachment, XmATTACH_FORM,
				      XmNrightAttachment, XmATTACH_FORM,
				      NULL);

	ok = XtVaCreateManagedWidget("joinOk", xmPushButtonWidgetClass, join_dialog,
				     XmNtopAttachment, XmATTACH_WIDGET,
				     XmNtopWidget, sep,
				     XmNleftAttachment, XmATTACH_FORM,
				     XmNbottomAttachment, XmATTACH_FORM,
				     NULL);
	XtAddCallback(ok, XmNactivateCallback, join_ok_cb, NULL);

	cancel = XtVaCreateManagedWidget("joinCancel", xmPushButtonWidgetClass, join_dialog,
					 XmNtopAttachment, XmATTACH_WIDGET,
					 XmNtopWidget, sep,
					 XmNrightAttachment, XmATTACH_FORM,
					 XmNbottomAttachment, XmATTACH_FORM,
					 NULL);
	XtAddCallback(cancel, XmNactivateCallback, join_cancel_cb, NULL);

	XtVaSetValues(join_dialog, XmNdefaultButton, ok, NULL);
}

/*
 * Open the join dialog, pre-filled from the selected network if the lower pane
 * is showing a scan - that is the common case, and retyping an SSID you can see
 * on screen is exactly the kind of thing that gets typed wrong.
 */
static void open_join_dialog(void)
{
	XmString xt;
	int row;

	if (!require_wifi())
		return;

	if (join_dialog == NULL)
		build_join_dialog();

	memset(join_key_text, 0, sizeof(join_key_text));
	XmTextFieldSetString(join_key_w, "");
	XmTextFieldSetString(join_ssid_w, "");

	if (content_mode == CONTENT_WIFI) {
		row = list_selection(content_list_w);
		if (row >= 0 && row < wifi_nets_n)
			XmTextFieldSetString(join_ssid_w, wifi_nets[row].ssid);
	}

	xt = XmStringCreateLtoR("Join Wi-Fi Network", XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(join_dialog, XmNdialogTitle, xt, NULL);
	XmStringFree(xt);

	XtManageChild(join_dialog);
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
	wifi_nets_n = 0;
	update_sensitivity();

	if (scan[sel_dev].confirmed)
		sprintf(msg, "Selected %s - toolbox ready.", scan[sel_dev].path);
	else if (scan[sel_dev].wifi)
		sprintf(msg, "%s is the Wi-Fi device - use the Wi-Fi menu.", scan[sel_dev].path);
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
	wifi_nets_n = 0;
	update_sensitivity();

	/*
	 * Switching to Wi-Fi does NOT scan. The other two listings are a single
	 * quick command, but a scan is seconds of radio time that freezes the
	 * window, and nobody expects clicking a radio button to do that - so it
	 * waits to be asked.
	 */
	if (content_mode == CONTENT_WIFI) {
		set_status(wifi_index() >= 0 ?
			   "Press Refresh to scan for Wi-Fi networks." :
			   "No Wi-Fi device found on this bus.");
		return;
	}

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

/*
 * Wi-Fi > Scan For Networks. The results belong in the lower pane, so this
 * flips the pane to Wi-Fi rather than opening a listing in a dialog that would
 * then have nowhere to Join from.
 */
/* ARGSUSED */
static void wifi_scan_cb(Widget w, XtPointer client, XtPointer call)
{
	if (!require_wifi())
		return;
	/* notify True on purpose: the RowColumn only unsets the sibling
	 * toggles when it is told, so a silent SetState would leave two of
	 * the three radio buttons looking selected. */
	if (mode_wifi_btn != NULL)
		XmToggleButtonSetState(mode_wifi_btn, True, True);
	content_mode = CONTENT_WIFI;
	update_sensitivity();
	refresh_content();
}

/* ARGSUSED */
static void wifi_info_cb(Widget w, XtPointer client, XtPointer call)
{
	do_wifi_info();
}

/* ARGSUSED */
static void wifi_join_cb(Widget w, XtPointer client, XtPointer call)
{
	open_join_dialog();
}

/* ARGSUSED */
static void about_cb(Widget w, XtPointer client, XtPointer call)
{
	char text[1024];

	sprintf(text,
		"scsitbgui - toolbox for emulated SCSI devices\n\n"
		"Motif front end for BlueSCSI / ZuluSCSI targets that\n"
		"implement the Toolbox API (SCSI vendor commands 0xD0-0xDA),\n"
		"and for the Wi-Fi commands (0x1C) on their emulated\n"
		"DaynaPort network target.\n\n"
		"A device is only driven as a toolbox target if it both\n"
		"advertises the API and answers TOOLBOX_LIST_DEVICES (0xD9);\n"
		"a claim on its own is never trusted. The same rule decides\n"
		"which node carries the radio.\n\n"
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

	/* --- Wi-Fi ---
	 * A menu of its own rather than three more entries under Device,
	 * because these do not act on the device selected in the bus list: they
	 * act on the emulated NETWORK target, which is a different SCSI ID and
	 * is found automatically. Filing them under Device would say otherwise.
	 */
	pane = XmCreatePulldownMenu(menubar, "wifiPane", NULL, 0);
	XtVaCreateManagedWidget("wifi", xmCascadeButtonWidgetClass, menubar,
				XmNmnemonic, 'W', XmNsubMenuId, pane, NULL);

	item = XtVaCreateManagedWidget("wifiScan", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, wifi_scan_cb, NULL);
	item = XtVaCreateManagedWidget("wifiInfo", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, wifi_info_cb, NULL);

	XtVaCreateManagedWidget("sep4", xmSeparatorWidgetClass, pane, NULL);

	item = XtVaCreateManagedWidget("wifiJoinItem", xmPushButtonWidgetClass, pane, NULL);
	XtAddCallback(item, XmNactivateCallback, wifi_join_cb, NULL);

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
	mode_wifi_btn = XtVaCreateManagedWidget("modeWifi", xmToggleButtonWidgetClass, radio, NULL);
	XtAddCallback(mode_wifi_btn, XmNvalueChangedCallback, mode_cb, (XtPointer)CONTENT_WIFI);
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

	join_btn = XtVaCreateManagedWidget("joinWifi", xmPushButtonWidgetClass, form,
					   XmNbottomAttachment, XmATTACH_FORM,
					   XmNleftAttachment, XmATTACH_WIDGET,
					   XmNleftWidget, cd_btn,
					   NULL);
	XtAddCallback(join_btn, XmNactivateCallback, wifi_join_cb, NULL);

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
