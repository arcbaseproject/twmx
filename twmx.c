/* twmx - a small tiling window manager for X11.
 *
 * Grown out of TinyWM, written by Nick Welch <nick@incise.org> in 2005 & 2011.
 * This software is in the public domain and is provided AS IS, with NO WARRANTY.
 *
 * Design notes:
 * - Windows are never reparented. Hidden windows (on workspaces not shown on
 *   any monitor) are moved offscreen instead of unmapped, so every real
 *   UnmapNotify means the client withdrew its window.
 * - There are N global workspaces; each monitor shows exactly one of them.
 * - Everything is driven by X events; the WM sleeps in XNextEvent otherwise.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define LENGTH(a)    (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define WIDTH(c)     ((c)->w + 2 * (c)->bw)
#define HEIGHT(c)    ((c)->h + 2 * (c)->bw)
#define CLEANMASK(m) ((m) & ~(numlockmask | LockMask) & \
	(ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask))
#define MOUSEMASK    (ButtonPressMask | ButtonReleaseMask | PointerMotionMask)

typedef union {
	int i;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	Arg arg;
} Key;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	Arg arg;
} Button;

typedef struct {
	const char *class, *instance;
	int isfloating, ws;
} Rule;

typedef struct Client Client;
struct Client {
	Window win;
	int x, y, w, h;          /* window geometry, border excluded */
	int fx, fy, fw, fh;      /* geometry saved before going fullscreen */
	int bw, oldbw;
	int minw, minh, maxw, maxh;
	int ws;
	int isfloating, isfullscreen, neverfocus;
	Client *next;            /* tiling order */
	Client *snext;           /* focus history */
};

typedef struct {
	int mx, my, mw, mh;      /* monitor geometry */
	int wx, wy, ww, wh;      /* work area: monitor minus dock struts */
	int ws;                  /* workspace shown on this monitor */
} Monitor;

typedef void (*Layout)(Monitor *);

typedef struct {
	float mfact;
	int layout;
	Client *sel;             /* last focused client on this workspace */
} Workspace;

typedef struct Dock Dock;
struct Dock {
	Window win;
	long strut[12];          /* _NET_WM_STRUT_PARTIAL layout */
	Dock *next;
};

enum { WMProtocols, WMDelete, WMState, WMTakeFocus,
       NetSupported, NetWMName, NetWMState, NetWMCheck, NetWMFullscreen,
       NetActiveWindow, NetWMWindowType, NetWMWindowTypeDialog,
       NetWMWindowTypeUtility, NetWMWindowTypeSplash, NetWMWindowTypeToolbar,
       NetWMWindowTypeDock, NetClientList, NetNumberOfDesktops,
       NetCurrentDesktop, NetDesktopNames, NetWMDesktop, NetCloseWindow,
       NetWMStrut, NetWMStrutPartial, Utf8String, AtomLast };
#define NetFirst NetSupported
#define NetLast  Utf8String

enum { CurNormal, CurMove, CurResize, CurLast };

/* layouts */
static void tile(Monitor *m);
static void monocle(Monitor *m);

/* commands bound in config.h */
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static void killclient(const Arg *arg);
static void mousedrag(const Arg *arg);
static void movestack(const Arg *arg);
static void quit(const Arg *arg);
static void sendmon(const Arg *arg);
static void sendtows(const Arg *arg);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void spawn(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void view(const Arg *arg);

#include "config.h"

#define NWS ((int)LENGTH(wsnames))

static Display *dpy;
static Window root, wmcheckwin;
static int screen, sw, sh;
static int running = 1;
static int userandr, rrevbase;
static unsigned int numlockmask;
static unsigned long colnorm, colsel;
static Atom atoms[AtomLast];
static Cursor cursors[CurLast];
static Client *clients, *stack, *focused;
static Dock *docks;
static Monitor *mons;
static int nmons, selmon;
static Workspace wss[LENGTH(wsnames)];

static void arrange(void);
static void focus(Client *c);
static void restack(void);
static void unmanage(Client *c, int destroyed);
static void manage(Window w, XWindowAttributes *wa);
static void (*handler[LASTEvent])(XEvent *);

/* ---------- helpers ---------- */

static void *
ecalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);

	if (!p) {
		fputs("twmx: out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static int
xerror(Display *d, XErrorEvent *ee)
{
	char buf[128];

	/* Windows can vanish at any time; errors about them are expected. */
	if (ee->error_code == BadWindow
	|| (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	|| (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	|| (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	|| (ee->request_code == X_GrabKey && ee->error_code == BadAccess))
		return 0;
	XGetErrorText(d, ee->error_code, buf, sizeof buf);
	fprintf(stderr, "twmx: X error: request %d: %s\n", ee->request_code, buf);
	return 0;
}

static int
xerrordummy(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	return 0;
}

static int
xerrorstart(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	fputs("twmx: another window manager is already running\n", stderr);
	exit(1);
}

/* Read up to max 32-bit items of a window property; returns the count. */
static int
getprop(Window w, Atom prop, Atom type, long *out, int max)
{
	Atom real;
	int fmt, i, got = 0;
	unsigned long n, extra;
	unsigned char *p = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0L, max, False, type, &real, &fmt,
	                       &n, &extra, &p) == Success && p) {
		if (fmt == 32)
			for (i = 0; i < (int)n && i < max; i++, got++)
				out[i] = ((long *)p)[i];
		XFree(p);
	}
	return got;
}

static int
hasatom(Window w, Atom prop, Atom value)
{
	long list[16];
	int i, n = getprop(w, prop, XA_ATOM, list, LENGTH(list));

	for (i = 0; i < n; i++)
		if ((Atom)list[i] == value)
			return 1;
	return 0;
}

static long
getstate(Window w)
{
	long state = -1;

	getprop(w, atoms[WMState], atoms[WMState], &state, 1);
	return state;
}

static void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, atoms[WMState], atoms[WMState], 32,
	                PropModeReplace, (unsigned char *)data, 2);
}

static int
sendevent(Client *c, Atom proto)
{
	Atom *protos;
	int n, found = 0;
	XEvent ev;

	if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
		while (!found && n--)
			found = protos[n] == proto;
		XFree(protos);
	}
	if (found) {
		memset(&ev, 0, sizeof ev);
		ev.type = ClientMessage;
		ev.xclient.window = c->win;
		ev.xclient.message_type = atoms[WMProtocols];
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = proto;
		ev.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	}
	return found;
}

static Client *
wintoclient(Window w)
{
	Client *c;

	for (c = clients; c && c->win != w; c = c->next);
	return c;
}

static Dock *
wintodock(Window w)
{
	Dock *d;

	for (d = docks; d && d->win != w; d = d->next);
	return d;
}

/* Monitor index showing workspace ws, or -1 if it is hidden. */
static int
wsmon(int ws)
{
	int i;

	for (i = 0; i < nmons; i++)
		if (mons[i].ws == ws)
			return i;
	return -1;
}

static int
isvisible(Client *c)
{
	return wsmon(c->ws) >= 0;
}

static int
montopoint(int x, int y)
{
	int i;

	for (i = 0; i < nmons; i++)
		if (x >= mons[i].mx && x < mons[i].mx + mons[i].mw
		&&  y >= mons[i].my && y < mons[i].my + mons[i].mh)
			return i;
	return selmon;
}

static Client *
nexttiled(Client *c, int ws)
{
	for (; c && (c->ws != ws || c->isfloating || c->isfullscreen); c = c->next);
	return c;
}

static Client *
selclient(void)
{
	return focused && focused->ws == mons[selmon].ws ? focused : NULL;
}

/* ---------- client lists ---------- */

static void
attach(Client *c)
{
	c->next = clients;
	clients = c;
}

static void
detach(Client *c)
{
	Client **tc;

	for (tc = &clients; *tc && *tc != c; tc = &(*tc)->next);
	if (*tc)
		*tc = c->next;
}

static void
attachstack(Client *c)
{
	c->snext = stack;
	stack = c;
}

static void
detachstack(Client *c)
{
	Client **tc;

	for (tc = &stack; *tc && *tc != c; tc = &(*tc)->snext);
	if (*tc)
		*tc = c->snext;
}

/* ---------- EWMH bookkeeping ---------- */

static void
updateclientlist(void)
{
	Client *c;

	XDeleteProperty(dpy, root, atoms[NetClientList]);
	for (c = clients; c; c = c->next)
		XChangeProperty(dpy, root, atoms[NetClientList], XA_WINDOW, 32,
		                PropModeAppend, (unsigned char *)&c->win, 1);
}

static void
updatecurrentdesktop(void)
{
	static long last = -1;
	long ws = mons[selmon].ws;

	if (ws != last)
		XChangeProperty(dpy, root, atoms[NetCurrentDesktop], XA_CARDINAL, 32,
		                PropModeReplace, (unsigned char *)&ws, 1);
	last = ws;
}

static void
setws(Client *c, int ws)
{
	long d = ws;

	if (wss[c->ws].sel == c)
		wss[c->ws].sel = NULL;
	c->ws = ws;
	XChangeProperty(dpy, c->win, atoms[NetWMDesktop], XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)&d, 1);
}

/* ---------- geometry ---------- */

static void
configure(Client *c)
{
	XConfigureEvent ce;

	memset(&ce, 0, sizeof ce);
	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static void
applysizehints(Client *c, int *w, int *h)
{
	*w = MAX(1, MAX(*w, c->minw));
	*h = MAX(1, MAX(*h, c->minh));
	if (c->isfloating && c->maxw > 0)
		*w = MIN(*w, c->maxw);
	if (c->isfloating && c->maxh > 0)
		*h = MIN(*h, c->maxh);
}

static void
resize(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	if (c->isfloating && !c->isfullscreen)
		applysizehints(c, &w, &h);
	c->x = wc.x = x;
	c->y = wc.y = y;
	c->w = wc.width = MAX(1, w);
	c->h = wc.height = MAX(1, h);
	wc.border_width = c->bw;
	XConfigureWindow(dpy, c->win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &wc);
	configure(c);
}

/* Resize so that the window including its border fills the given cell. */
static void
place(Client *c, int x, int y, int w, int h)
{
	resize(c, x, y, w - 2 * c->bw, h - 2 * c->bw);
}

/* Center a floating window on m if it is not already within it. */
static void
placefloat(Client *c, Monitor *m)
{
	c->w = MIN(c->w, m->ww - 2 * c->bw);
	c->h = MIN(c->h, m->wh - 2 * c->bw);
	if (c->x + WIDTH(c) <= m->mx || c->x >= m->mx + m->mw
	||  c->y + HEIGHT(c) <= m->my || c->y >= m->my + m->mh
	|| (c->x == 0 && c->y == 0)) {
		c->x = m->wx + (m->ww - WIDTH(c)) / 2;
		c->y = m->wy + (m->wh - HEIGHT(c)) / 2;
	}
}

/* ---------- layouts ---------- */

static void
tile(Monitor *m)
{
	Client *c;
	int n = 0, i, nm, g = gappx, mw, my, ty, h;
	int ax = m->wx + g, ay = m->wy + g, aw = m->ww - 2 * g, ah = m->wh - 2 * g;

	for (c = nexttiled(clients, m->ws); c; c = nexttiled(c->next, m->ws))
		n++;
	if (n == 0)
		return;
	nm = MIN(MAX(nmaster, 0), n);
	if (nm == 0)
		mw = 0;
	else if (n > nm)
		mw = (aw - g) * wss[m->ws].mfact;
	else
		mw = aw;
	my = ty = ay;
	for (i = 0, c = nexttiled(clients, m->ws); c; c = nexttiled(c->next, m->ws), i++) {
		if (i < nm) {
			/* divide the remaining height evenly among remaining windows */
			h = (ay + ah - my - (nm - i - 1) * g) / (nm - i);
			place(c, ax, my, mw, h);
			my += h + g;
		} else {
			h = (ay + ah - ty - (n - i - 1) * g) / (n - i);
			place(c, nm ? ax + mw + g : ax, ty, nm ? aw - mw - g : aw, h);
			ty += h + g;
		}
	}
}

static void
monocle(Monitor *m)
{
	Client *c;
	int g = gappx;

	for (c = nexttiled(clients, m->ws); c; c = nexttiled(c->next, m->ws))
		place(c, m->wx + g, m->wy + g, m->ww - 2 * g, m->wh - 2 * g);
}

/* ---------- arrangement and stacking ---------- */

static void
showhide(void)
{
	Client *c;

	for (c = clients; c; c = c->next)
		XMoveWindow(dpy, c->win, isvisible(c) ? c->x : WIDTH(c) * -2, c->y);
}

static void
arrangemon(Monitor *m)
{
	Client *c;

	for (c = clients; c; c = c->next) {
		if (c->ws != m->ws)
			continue;
		if (c->isfullscreen)
			resize(c, m->mx, m->my, m->mw, m->mh);
		else if (c->isfloating) {
			placefloat(c, m);
			resize(c, c->x, c->y, c->w, c->h);
		}
	}
	layouts[wss[m->ws].layout](m);
}

static void
arrange(void)
{
	int i;

	showhide();
	for (i = 0; i < nmons; i++)
		arrangemon(&mons[i]);
	restack();
}

/* Tiled windows at the bottom (focused one on top of them), then
 * fullscreen windows, then floating windows, focused window topmost. */
static void
restack(void)
{
	Client *c;
	XEvent ev;

	if (focused && !focused->isfloating && !focused->isfullscreen)
		XLowerWindow(dpy, focused->win);
	for (c = clients; c; c = c->next)
		if (c != focused && !c->isfloating && !c->isfullscreen)
			XLowerWindow(dpy, c->win);
	for (c = clients; c; c = c->next)
		if (c->isfullscreen)
			XRaiseWindow(dpy, c->win);
	for (c = clients; c; c = c->next)
		if (c->isfloating && !c->isfullscreen)
			XRaiseWindow(dpy, c->win);
	if (focused && (focused->isfloating || focused->isfullscreen))
		XRaiseWindow(dpy, focused->win);
	/* Windows moving under the pointer must not steal focus. */
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
}

/* ---------- input grabs ---------- */

static void
updatenumlockmask(void)
{
	int i, j;
	XModifierKeymap *mm = XGetModifierMapping(dpy);
	KeyCode nl = XKeysymToKeycode(dpy, XK_Num_Lock);

	numlockmask = 0;
	for (i = 0; i < 8; i++)
		for (j = 0; j < mm->max_keypermod; j++)
			if (nl && mm->modifiermap[i * mm->max_keypermod + j] == nl)
				numlockmask = 1u << i;
	XFreeModifiermap(mm);
}

static void
grabkeys(void)
{
	unsigned int i, j, mods[] = { 0, LockMask, 0, LockMask };
	KeyCode code;

	updatenumlockmask();
	mods[2] = numlockmask;
	mods[3] = numlockmask | LockMask;
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < LENGTH(keys); i++)
		if ((code = XKeysymToKeycode(dpy, keys[i].keysym)))
			for (j = 0; j < LENGTH(mods); j++)
				XGrabKey(dpy, code, keys[i].mod | mods[j], root, True,
				         GrabModeAsync, GrabModeAsync);
}

static void
grabbuttons(Client *c, int isfocused)
{
	unsigned int i, j, mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };

	XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
	/* Unfocused windows: catch any click (synchronously, so it can be
	 * replayed to the client after focusing it). */
	if (!isfocused)
		XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
		            ButtonPressMask | ButtonReleaseMask, GrabModeSync,
		            GrabModeSync, None, None);
	for (i = 0; i < LENGTH(buttons); i++)
		for (j = 0; j < LENGTH(mods); j++)
			XGrabButton(dpy, buttons[i].button, buttons[i].mod | mods[j],
			            c->win, False, ButtonPressMask | ButtonReleaseMask,
			            GrabModeAsync, GrabModeAsync, None, None);
}

/* ---------- focus ---------- */

static void
setfocus(Client *c)
{
	if (!c->neverfocus)
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
	XChangeProperty(dpy, root, atoms[NetActiveWindow], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&c->win, 1);
	sendevent(c, atoms[WMTakeFocus]);
}

static void
unfocus(Client *c)
{
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, colnorm);
}

/* Focus c, or if c is NULL/hidden the most recently focused client on the
 * selected monitor's workspace. */
static void
focus(Client *c)
{
	int ws, m;

	if (!c || !isvisible(c)) {
		ws = mons[selmon].ws;
		if (!(c = wss[ws].sel))
			for (c = stack; c && c->ws != ws; c = c->snext);
	}
	if (focused && focused != c)
		unfocus(focused);
	if (c) {
		if ((m = wsmon(c->ws)) >= 0)
			selmon = m;
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win, colsel);
		setfocus(c);
		wss[c->ws].sel = c;
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, atoms[NetActiveWindow]);
	}
	focused = c;
	updatecurrentdesktop();
	restack();
}

/* ---------- client state ---------- */

static void
updatesizehints(Client *c)
{
	XSizeHints h;
	long supplied;

	c->minw = c->minh = c->maxw = c->maxh = 0;
	if (!XGetWMNormalHints(dpy, c->win, &h, &supplied))
		return;
	if (h.flags & PMinSize) {
		c->minw = h.min_width;
		c->minh = h.min_height;
	} else if (h.flags & PBaseSize) {
		c->minw = h.base_width;
		c->minh = h.base_height;
	}
	if (h.flags & PMaxSize) {
		c->maxw = h.max_width;
		c->maxh = h.max_height;
	}
	if (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh)
		c->isfloating = 1;
}

static void
updatewmhints(Client *c)
{
	XWMHints *wmh = XGetWMHints(dpy, c->win);

	if (!wmh)
		return;
	c->neverfocus = (wmh->flags & InputHint) ? !wmh->input : 0;
	XFree(wmh);
}

static void
applyrules(Client *c)
{
	unsigned int i;
	const Rule *r;
	XClassHint ch = { NULL, NULL };

	if (!XGetClassHint(dpy, c->win, &ch))
		return;
	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->class || (ch.res_class && strstr(ch.res_class, r->class)))
		&&  (!r->instance || (ch.res_name && strstr(ch.res_name, r->instance)))) {
			c->isfloating |= r->isfloating;
			if (r->ws >= 0 && r->ws < NWS)
				c->ws = r->ws;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
}

static void
setfullscreen(Client *c, int fs)
{
	Atom real;
	int fmt;
	unsigned long n, extra, i, count = 0;
	unsigned char *data = NULL;
	Atom *states;

	if (!!fs == c->isfullscreen)
		return;
	if (XGetWindowProperty(dpy, c->win, atoms[NetWMState], 0L, 0x1fffffffL,
	                       False, XA_ATOM, &real, &fmt, &n, &extra, &data) != Success)
		return;
	if (real != XA_ATOM || fmt != 32)
		n = 0;
	states = ecalloc(n + 1, sizeof *states);
	for (i = 0; i < n; i++)
		if (((Atom *)data)[i] != atoms[NetWMFullscreen])
			states[count++] = ((Atom *)data)[i];
	if (fs)
		states[count++] = atoms[NetWMFullscreen];
	XChangeProperty(dpy, c->win, atoms[NetWMState], XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)states, count);
	if (data)
		XFree(data);
	free(states);
	if (fs && !c->isfullscreen) {
		c->isfullscreen = 1;
		c->fx = c->x; c->fy = c->y; c->fw = c->w; c->fh = c->h;
		c->bw = 0;
	} else if (!fs && c->isfullscreen) {
		c->isfullscreen = 0;
		c->x = c->fx; c->y = c->fy; c->w = c->fw; c->h = c->fh;
		c->bw = borderpx;
	} else
		return;
	arrange();
}

/* ---------- monitors and docks ---------- */

static int
spans(int start, int len, long a, long b)
{
	return start <= b && start + len > a;
}

/* Shrink each monitor's work area by the struts of dock windows. */
static void
updateworkarea(void)
{
	int i, x1, y1, x2, y2;
	Monitor *m;
	Dock *d;
	long *s;

	for (i = 0; i < nmons; i++) {
		m = &mons[i];
		x1 = m->mx; y1 = m->my;
		x2 = m->mx + m->mw; y2 = m->my + m->mh;
		for (d = docks; d; d = d->next) {
			s = d->strut;
			if (s[0] && spans(m->my, m->mh, s[4], s[5]) && m->mx < s[0])
				x1 = MAX(x1, s[0]);
			if (s[1] && spans(m->my, m->mh, s[6], s[7]) && x2 > sw - s[1])
				x2 = MIN(x2, sw - s[1]);
			if (s[2] && spans(m->mx, m->mw, s[8], s[9]) && m->my < s[2])
				y1 = MAX(y1, s[2]);
			if (s[3] && spans(m->mx, m->mw, s[10], s[11]) && y2 > sh - s[3])
				y2 = MIN(y2, sh - s[3]);
		}
		if (x2 - x1 < 2 * (int)gappx + 1 || y2 - y1 < 2 * (int)gappx + 1) {
			x1 = m->mx; y1 = m->my;
			x2 = m->mx + m->mw; y2 = m->my + m->mh;
		}
		m->wx = x1; m->wy = y1;
		m->ww = x2 - x1; m->wh = y2 - y1;
	}
}

static void
readstrut(Dock *d)
{
	long *s = d->strut;
	int i;

	memset(s, 0, sizeof d->strut);
	if (getprop(d->win, atoms[NetWMStrutPartial], XA_CARDINAL, s, 12) == 12)
		return;
	if (getprop(d->win, atoms[NetWMStrut], XA_CARDINAL, s, 4) == 4)
		for (i = 4; i < 12; i += 2) {
			s[i] = 0;
			s[i + 1] = INT_MAX;
		}
}

static void
managedock(Window w)
{
	Dock *d = ecalloc(1, sizeof *d);

	d->win = w;
	d->next = docks;
	docks = d;
	XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
	readstrut(d);
	XMapWindow(dpy, w);
	updateworkarea();
	arrange();
}

static void
unmanagedock(Dock *d)
{
	Dock **td;

	for (td = &docks; *td && *td != d; td = &(*td)->next);
	if (*td)
		*td = d->next;
	free(d);
	updateworkarea();
	arrange();
}

static int
samegeom(const Monitor *a, const Monitor *b)
{
	return a->mx == b->mx && a->my == b->my && a->mw == b->mw && a->mh == b->mh;
}

/* Does RandR monitor a strictly contain monitor b? */
static int
encloses(const XRRMonitorInfo *a, const XRRMonitorInfo *b)
{
	return b->x >= a->x && b->y >= a->y
	    && b->x + b->width <= a->x + a->width && b->y + b->height <= a->y + a->height
	    && (b->width < a->width || b->height < a->height);
}

/* Rebuild the monitor list from RandR, keeping each monitor's workspace. */
static void
updatemonitors(void)
{
	XRRMonitorInfo *info = NULL;
	Monitor *nm;
	int n = 0, i, j, k = 0, primary = 0, ws, changed;

	if (userandr)
		info = XRRGetMonitors(dpy, root, True, &n);
	nm = ecalloc(MAX(n, 1), sizeof *nm);
	for (i = 0; i < n && k < NWS; i++) {
		/* an output split into virtual monitors (xrandr --setmonitor)
		 * can still be listed whole; keep only the parts */
		for (j = 0; j < n && !encloses(&info[i], &info[j]); j++);
		if (j < n)
			continue;
		nm[k].mx = info[i].x;
		nm[k].my = info[i].y;
		nm[k].mw = info[i].width;
		nm[k].mh = info[i].height;
		for (j = 0; j < k && !samegeom(&nm[j], &nm[k]); j++);
		if (j < k)
			continue; /* mirrored output */
		if (info[i].primary)
			primary = k;
		k++;
	}
	if (info)
		XRRFreeMonitors(info);
	if (k == 0) {
		nm[0].mw = sw;
		nm[0].mh = sh;
		k = 1;
	}
	changed = k != nmons;
	for (i = 0; i < k && !changed; i++)
		changed = !samegeom(&nm[i], &mons[i]);
	if (!changed) {
		free(nm);
		return;
	}
	for (i = 0; i < k; i++)
		nm[i].ws = i < nmons ? mons[i].ws : -1;
	for (i = 0; i < k; i++) {
		if (nm[i].ws >= 0)
			continue;
		for (ws = 0; ws < NWS; ws++) {
			for (j = 0; j < k && nm[j].ws != ws; j++);
			if (j == k)
				break;
		}
		nm[i].ws = ws;
	}
	if (!mons)
		selmon = primary;
	free(mons);
	mons = nm;
	nmons = k;
	if (selmon >= nmons)
		selmon = 0;
	updateworkarea();
	arrange();
	focus(NULL);
}

/* ---------- window lifecycle ---------- */

static void
manage(Window w, XWindowAttributes *wa)
{
	Client *c, *t;
	Window trans = None;
	Monitor *m;
	long d;
	Atom type;
	int mi;

	type = getprop(w, atoms[NetWMWindowType], XA_ATOM, &d, 1) ? (Atom)d : None;
	if (type == atoms[NetWMWindowTypeDock]) {
		managedock(w);
		return;
	}
	c = ecalloc(1, sizeof *c);
	c->win = w;
	c->x = wa->x;
	c->y = wa->y;
	c->w = wa->width;
	c->h = wa->height;
	c->oldbw = wa->border_width;
	c->bw = borderpx;
	c->ws = mons[selmon].ws;
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->ws = t->ws;
		c->isfloating = 1;
	} else if (getprop(w, atoms[NetWMDesktop], XA_CARDINAL, &d, 1) && d >= 0 && d < NWS) {
		c->ws = d; /* keeps workspaces across a WM restart */
	}
	if (trans != None
	|| type == atoms[NetWMWindowTypeDialog] || type == atoms[NetWMWindowTypeUtility]
	|| type == atoms[NetWMWindowTypeSplash] || type == atoms[NetWMWindowTypeToolbar])
		c->isfloating = 1;
	applyrules(c);
	updatesizehints(c);
	updatewmhints(c);
	applysizehints(c, &c->w, &c->h);
	mi = wsmon(c->ws);
	m = &mons[mi >= 0 ? mi : selmon];
	if (c->isfloating)
		placefloat(c, m);

	XSetWindowBorderWidth(dpy, w, c->bw);
	XSetWindowBorder(dpy, w, colnorm);
	XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask
	             | StructureNotifyMask);
	grabbuttons(c, 0);
	attach(c);
	attachstack(c);
	XChangeProperty(dpy, root, atoms[NetClientList], XA_WINDOW, 32, PropModeAppend,
	                (unsigned char *)&c->win, 1);
	setws(c, c->ws);
	setclientstate(c, NormalState);
	/* park offscreen so the window first appears at its final geometry */
	XMoveResizeWindow(dpy, w, c->x + 2 * sw, c->y, c->w, c->h);
	if (hasatom(w, atoms[NetWMState], atoms[NetWMFullscreen]))
		setfullscreen(c, 1);
	arrange();
	XMapWindow(dpy, w);
	if (isvisible(c)) {
		focus(c);
	}
}

static void
unmanage(Client *c, int destroyed)
{
	if (wss[c->ws].sel == c)
		wss[c->ws].sel = NULL;
	if (focused == c)
		focused = NULL;
	detach(c);
	detachstack(c);
	if (!destroyed) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSelectInput(dpy, c->win, NoEventMask);
		XSetWindowBorderWidth(dpy, c->win, c->oldbw);
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);
	updateclientlist();
	arrange();
	focus(NULL);
}

/* Adopt windows that existed before the WM started. */
static void
scan(void)
{
	unsigned int i, n, pass;
	Window d1, d2, *wins = NULL, trans;
	XWindowAttributes wa;
	int istrans;

	if (!XQueryTree(dpy, root, &d1, &d2, &wins, &n))
		return;
	/* normal windows first, transients second so their parents exist */
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < n; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect
			|| wins[i] == wmcheckwin)
				continue;
			istrans = XGetTransientForHint(dpy, wins[i], &trans);
			if (istrans != (int)pass)
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
	if (wins)
		XFree(wins);
}

/* ---------- commands ---------- */

static void
spawn(const Arg *arg)
{
	char *const *cmd = (char *const *)arg->v;
	struct sigaction sa;

	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sa.sa_handler = SIG_DFL;
		sigaction(SIGCHLD, &sa, NULL);
		execvp(cmd[0], cmd);
		fprintf(stderr, "twmx: execvp '%s' failed\n", cmd[0]);
		_exit(1);
	}
}

static void
killclient(const Arg *arg)
{
	Client *c = selclient();

	(void)arg;
	if (!c || sendevent(c, atoms[WMDelete]))
		return;
	XGrabServer(dpy);
	XSetErrorHandler(xerrordummy);
	XSetCloseDownMode(dpy, DestroyAll);
	XKillClient(dpy, c->win);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XUngrabServer(dpy);
}

static void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i, *sel = selclient();
	int ws = mons[selmon].ws;

	if (!sel)
		return;
	if (arg->i > 0) {
		for (c = sel->next; c && c->ws != ws; c = c->next);
		if (!c)
			for (c = clients; c && c->ws != ws; c = c->next);
	} else {
		for (i = clients; i != sel; i = i->next)
			if (i->ws == ws)
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (i->ws == ws)
					c = i;
	}
	if (c && c != sel) {
		focus(c);
	}
}

static void
movestack(const Arg *arg)
{
	Client *c = selclient(), *t = NULL, *i, **pp;

	if (!c || c->isfloating || c->isfullscreen)
		return;
	if (arg->i > 0) {
		t = nexttiled(c->next, c->ws);
		detach(c);
		if (t) {
			c->next = t->next;
			t->next = c;
		} else
			attach(c); /* wrap around to the top */
	} else {
		for (i = nexttiled(clients, c->ws); i && i != c; i = nexttiled(i->next, c->ws))
			t = i;
		detach(c);
		for (pp = &clients; *pp && *pp != t; pp = &(*pp)->next);
		c->next = *pp; /* before t, or at the end when wrapping around */
		*pp = c;
	}
	arrange();
}

static void
togglefloating(const Arg *arg)
{
	Client *c = selclient();

	(void)arg;
	if (!c || c->isfullscreen)
		return;
	c->isfloating = !c->isfloating;
	arrange();
}

static void
togglefullscreen(const Arg *arg)
{
	Client *c = selclient();

	(void)arg;
	if (c)
		setfullscreen(c, !c->isfullscreen);
}

static void
viewws(int ws)
{
	int m;

	if (ws < 0 || ws >= NWS)
		return;
	if ((m = wsmon(ws)) >= 0)
		selmon = m;  /* already shown: just move to that monitor */
	else {
		mons[selmon].ws = ws;
		arrange();
	}
	focus(NULL);
}

static void
view(const Arg *arg)
{
	viewws(arg->i);
}

static void
sendtows(const Arg *arg)
{
	Client *c = selclient();

	if (!c || arg->i < 0 || arg->i >= NWS || arg->i == c->ws)
		return;
	setws(c, arg->i);
	arrange();
	focus(NULL);
}

static void
focusmon(const Arg *arg)
{
	if (nmons < 2)
		return;
	selmon = (selmon + arg->i + nmons) % nmons;
	focus(NULL);
}

static void
sendmon(const Arg *arg)
{
	Client *c = selclient();

	if (!c || nmons < 2)
		return;
	setws(c, mons[(selmon + arg->i + nmons) % nmons].ws);
	arrange();
	focus(NULL);
}

static void
setlayout(const Arg *arg)
{
	if (arg->i < 0 || arg->i >= (int)LENGTH(layouts))
		return;
	wss[mons[selmon].ws].layout = arg->i;
	arrange();
}

static void
setmfact(const Arg *arg)
{
	Workspace *w = &wss[mons[selmon].ws];
	float f = w->mfact + arg->f;

	if (f < 0.05 || f > 0.95)
		return;
	w->mfact = f;
	arrange();
}

static void
quit(const Arg *arg)
{
	(void)arg;
	running = 0;
}

/* Interactive move (arg 0) or resize (arg 1); floats the window. */
static void
mousedrag(const Arg *arg)
{
	Client *c = selclient();
	XEvent ev;
	Time last = 0;
	int px, py, ox, oy, ow, oh, dx, dy, di, m;
	unsigned int dui;
	Window dw;

	if (!c || c->isfullscreen)
		return;
	if (!XQueryPointer(dpy, root, &dw, &dw, &px, &py, &di, &di, &dui))
		return;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	                 None, cursors[arg->i ? CurResize : CurMove], CurrentTime) != GrabSuccess)
		return;
	ox = c->x; oy = c->y; ow = c->w; oh = c->h;
	do {
		XMaskEvent(dpy, MOUSEMASK | SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case MapRequest:
			/* keep serving other clients during the drag */
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if (ev.xmotion.time - last < 1000 / 60)
				continue; /* cap at ~60 updates per second */
			last = ev.xmotion.time;
			dx = ev.xmotion.x_root - px;
			dy = ev.xmotion.y_root - py;
			if (!c->isfloating) {
				c->isfloating = 1;
				arrange();
			}
			if (arg->i)
				resize(c, ox, oy, MAX(ow + dx, MAX(c->minw, 1)), MAX(oh + dy, MAX(c->minh, 1)));
			else
				resize(c, ox + dx, oy + dy, ow, oh);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	/* dropped onto another monitor: adopt that monitor's workspace */
	m = montopoint(c->x + WIDTH(c) / 2, c->y + HEIGHT(c) / 2);
	if (m != wsmon(c->ws)) {
		setws(c, mons[m].ws);
		arrange();
		focus(c);
	}
}

/* ---------- event handlers ---------- */

static void
buttonpress(XEvent *e)
{
	XButtonPressedEvent *ev = &e->xbutton;
	Client *c = wintoclient(ev->window);
	unsigned int i;
	int m;

	if (!c) {
		if (ev->window == root && (m = montopoint(ev->x_root, ev->y_root)) != selmon) {
			selmon = m;
			focus(NULL);
		}
		return;
	}
	if (c != focused) {
		focus(c);
	}
	XAllowEvents(dpy, ReplayPointer, CurrentTime);
	for (i = 0; i < LENGTH(buttons); i++)
		if (buttons[i].button == ev->button
		&& CLEANMASK(buttons[i].mod) == CLEANMASK(ev->state))
			buttons[i].func(&buttons[i].arg);
}

static void
clientmessage(XEvent *e)
{
	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);
	Atom fs = atoms[NetWMFullscreen];

	if (cme->message_type == atoms[NetCurrentDesktop]) {
		viewws(cme->data.l[0]);
		return;
	}
	if (!c)
		return;
	if (cme->message_type == atoms[NetWMDesktop]) {
		if (cme->data.l[0] >= 0 && cme->data.l[0] < NWS) {
			setws(c, cme->data.l[0]);
			arrange();
			focus(NULL);
		}
	} else if (cme->message_type == atoms[NetWMState]) {
		if ((Atom)cme->data.l[1] == fs || (Atom)cme->data.l[2] == fs)
			setfullscreen(c, cme->data.l[0] == 1 /* add */
			              || (cme->data.l[0] == 2 /* toggle */ && !c->isfullscreen));
	} else if (cme->message_type == atoms[NetActiveWindow]) {
		if (!isvisible(c))
			viewws(c->ws);
		focus(c);
	} else if (cme->message_type == atoms[NetCloseWindow]) {
		if (!sendevent(c, atoms[WMDelete]))
			XKillClient(dpy, c->win);
	}
}

static void
configurenotify(XEvent *e)
{
	XConfigureEvent *ev = &e->xconfigure;

	if (ev->window == root) {
		sw = ev->width;
		sh = ev->height;
		updatemonitors();
	}
}

static void
configurerequest(XEvent *e)
{
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;
	Client *c = wintoclient(ev->window);

	if (c) {
		if (c->isfloating && !c->isfullscreen) {
			if (ev->value_mask & CWX)
				c->x = ev->x;
			if (ev->value_mask & CWY)
				c->y = ev->y;
			if (ev->value_mask & CWWidth)
				c->w = ev->width;
			if (ev->value_mask & CWHeight)
				c->h = ev->height;
			applysizehints(c, &c->w, &c->h);
			if (isvisible(c))
				resize(c, c->x, c->y, c->w, c->h);
			else
				configure(c);
		} else
			configure(c); /* tiled: geometry is ours, tell the client */
	} else {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

static void
destroynotify(XEvent *e)
{
	Client *c;
	Dock *d;

	if ((c = wintoclient(e->xdestroywindow.window)))
		unmanage(c, 1);
	else if ((d = wintodock(e->xdestroywindow.window)))
		unmanagedock(d);
}

static void
enternotify(XEvent *e)
{
	XCrossingEvent *ev = &e->xcrossing;
	Client *c;
	int m;

	if (!focusfollowsmouse)
		return;
	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	if ((c = wintoclient(ev->window))) {
		if (c != focused)
			focus(c);
	} else if (ev->window == root && (m = montopoint(ev->x_root, ev->y_root)) != selmon) {
		selmon = m;
		focus(NULL);
	}
}

static void
focusin(XEvent *e)
{
	Window w, r, parent, *children;
	unsigned int n;
	int revert;

	/* some clients grab focus on their own; give it back */
	if (!focused || e->xfocus.window == focused->win)
		return;
	XGetInputFocus(dpy, &w, &revert);
	while (w != None && w != PointerRoot && w != root) {
		if (w == focused->win)
			return; /* The client may focus any of its child windows. */
		if (!XQueryTree(dpy, w, &r, &parent, &children, &n))
			break;
		if (children)
			XFree(children);
		w = parent;
	}
	setfocus(focused);
}

static void
keypress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XLookupKeysym(ev, 0);
	unsigned int i;

	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym
		&& CLEANMASK(keys[i].mod) == CLEANMASK(ev->state))
			keys[i].func(&keys[i].arg);
}

static void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard || ev->request == MappingModifier)
		grabkeys();
}

static void
maprequest(XEvent *e)
{
	XWindowAttributes wa;
	Window w = e->xmaprequest.window;

	if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect)
		return;
	if (!wintoclient(w) && !wintodock(w))
		manage(w, &wa);
}

static void
motionnotify(XEvent *e)
{
	XMotionEvent *ev = &e->xmotion;
	int m;

	/* pointer crossed into another monitor over the root window */
	if (focusfollowsmouse && ev->window == root
	&& (m = montopoint(ev->x_root, ev->y_root)) != selmon) {
		selmon = m;
		focus(NULL);
	}
}

static void
propertynotify(XEvent *e)
{
	XPropertyEvent *ev = &e->xproperty;
	Client *c;
	Dock *d;
	int wasfloating;

	if ((d = wintodock(ev->window))) {
		if (ev->atom == atoms[NetWMStrut] || ev->atom == atoms[NetWMStrutPartial]) {
			readstrut(d);
			updateworkarea();
			arrange();
		}
		return;
	}
	if (ev->state == PropertyDelete)
		return;
	if (!(c = wintoclient(ev->window)))
		return;
	if (ev->atom == XA_WM_NORMAL_HINTS) {
		wasfloating = c->isfloating;
		updatesizehints(c);
		if (c->isfloating != wasfloating)
			arrange();
	} else if (ev->atom == XA_WM_HINTS)
		updatewmhints(c);
}

static void
unmapnotify(XEvent *e)
{
	XUnmapEvent *ev = &e->xunmap;
	Client *c;
	Dock *d;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	} else if ((d = wintodock(ev->window)))
		unmanagedock(d);
}

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureNotify] = configurenotify,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[FocusIn] = focusin,
	[KeyPress] = keypress,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify,
	[PropertyNotify] = propertynotify,
	[UnmapNotify] = unmapnotify,
};

/* ---------- setup and main loop ---------- */

static void
checkotherwm(void)
{
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	XSetErrorHandler(xerrorstart);
	XSelectInput(dpy, root, SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

static unsigned long
getcolor(const char *name)
{
	XColor c, exact;

	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &c, &exact)) {
		fprintf(stderr, "twmx: cannot allocate color '%s'\n", name);
		return BlackPixel(dpy, screen);
	}
	return c.pixel;
}

static void
setupewmh(void)
{
	static char *names[AtomLast] = {
		"WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_STATE", "WM_TAKE_FOCUS",
		"_NET_SUPPORTED", "_NET_WM_NAME", "_NET_WM_STATE",
		"_NET_SUPPORTING_WM_CHECK", "_NET_WM_STATE_FULLSCREEN",
		"_NET_ACTIVE_WINDOW", "_NET_WM_WINDOW_TYPE",
		"_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
		"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_TOOLBAR",
		"_NET_WM_WINDOW_TYPE_DOCK", "_NET_CLIENT_LIST",
		"_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
		"_NET_DESKTOP_NAMES", "_NET_WM_DESKTOP", "_NET_CLOSE_WINDOW",
		"_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL", "UTF8_STRING",
	};
	char namebuf[256];
	long nws = NWS;
	int i, len = 0;

	XInternAtoms(dpy, names, AtomLast, False, atoms);
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, atoms[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, atoms[NetWMName], atoms[Utf8String], 8,
	                PropModeReplace, (unsigned char *)"twmx", 4);
	XChangeProperty(dpy, root, atoms[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, root, atoms[NetSupported], XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&atoms[NetFirst], NetLast - NetFirst);
	XChangeProperty(dpy, root, atoms[NetNumberOfDesktops], XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)&nws, 1);
	/* desktop names: NUL-separated list */
	for (i = 0; i < NWS && len + strlen(wsnames[i]) + 1 <= sizeof namebuf; i++) {
		strcpy(namebuf + len, wsnames[i]);
		len += strlen(wsnames[i]) + 1;
	}
	XChangeProperty(dpy, root, atoms[NetDesktopNames], atoms[Utf8String], 8,
	                PropModeReplace, (unsigned char *)namebuf, len);
	XDeleteProperty(dpy, root, atoms[NetClientList]);
}

static void
setup(void)
{
	struct sigaction sa;
	XSetWindowAttributes wa;
	int i, dummy, major = 0, minor = 0;

	/* reap children automatically */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	colnorm = getcolor(normbordercolor);
	colsel = getcolor(selbordercolor);
	cursors[CurNormal] = XCreateFontCursor(dpy, XC_left_ptr);
	cursors[CurMove] = XCreateFontCursor(dpy, XC_fleur);
	cursors[CurResize] = XCreateFontCursor(dpy, XC_sizing);
	for (i = 0; i < NWS; i++) {
		wss[i].mfact = mfact;
		wss[i].layout = defaultlayout >= 0 && defaultlayout < (int)LENGTH(layouts)
		              ? defaultlayout : 0;
	}
	setupewmh();

	/* XRRGetMonitors needs RandR >= 1.5 */
	userandr = XRRQueryExtension(dpy, &rrevbase, &dummy)
	        && XRRQueryVersion(dpy, &major, &minor)
	        && (major > 1 || (major == 1 && minor >= 5));
	if (userandr)
		XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask
		               | RROutputChangeNotifyMask);
	updatemonitors();

	wa.cursor = cursors[CurNormal];
	wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask
	              | ButtonPressMask | EnterWindowMask | StructureNotifyMask
	              | PropertyChangeMask | (focusfollowsmouse ? PointerMotionMask : 0);
	XChangeWindowAttributes(dpy, root, CWEventMask | CWCursor, &wa);
	grabkeys();
	focus(NULL);
}

static void
run(void)
{
	XEvent ev;

	XSync(dpy, False);
	while (running && !XNextEvent(dpy, &ev)) {
		if (userandr && (ev.type == rrevbase + RRScreenChangeNotify
		              || ev.type == rrevbase + RRNotify)) {
			XRRUpdateConfiguration(&ev);
			sw = DisplayWidth(dpy, screen);
			sh = DisplayHeight(dpy, screen);
			updatemonitors();
		} else if (ev.type < LASTEvent && handler[ev.type])
			handler[ev.type](&ev);
	}
}

/* Give every window back in a usable state for the next WM. */
static void
cleanup(void)
{
	Client *c;
	Dock *d;
	int i;

	while ((c = clients)) {
		clients = c->next;
		XSelectInput(dpy, c->win, NoEventMask);
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		XMoveWindow(dpy, c->win, c->x, c->y);
		XSetWindowBorderWidth(dpy, c->win, c->oldbw);
		free(c);
	}
	while ((d = docks)) {
		docks = d->next;
		free(d);
	}
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < CurLast; i++)
		XFreeCursor(dpy, cursors[i]);
	XDestroyWindow(dpy, wmcheckwin);
	XDeleteProperty(dpy, root, atoms[NetSupported]);
	XDeleteProperty(dpy, root, atoms[NetWMCheck]);
	XDeleteProperty(dpy, root, atoms[NetActiveWindow]);
	XDeleteProperty(dpy, root, atoms[NetClientList]);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync(dpy, False);
	free(mons);
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("twmx-" VERSION);
		return 0;
	}
	if (argc != 1) {
		fputs("usage: twmx [-v]\n", stderr);
		return 1;
	}
	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("twmx: cannot open display\n", stderr);
		return 1;
	}
	checkotherwm();
	setup();
	scan();
	run();
	cleanup();
	XCloseDisplay(dpy);
	return 0;
}
