/* Run only on the private X server started by tests/run.sh. */
#define main twmx_main
#include "../twmx.c"
#undef main
#include <assert.h>

static int
testerror(Display *d, XErrorEvent *e)
{
	xerror(d, e);
	assert(!"unexpected X error");
	return 0;
}

static Client *
newclient(void)
{
	Window w = XCreateSimpleWindow(dpy, root, 20, 20, 300, 200, 0, 0, 0);
	XWindowAttributes wa;

	assert(XGetWindowAttributes(dpy, w, &wa));
	manage(w, &wa);
	return wintoclient(w);
}

static Window
topclient(void)
{
	Window r, parent, *wins, top = None;
	unsigned int n, i;

	XSync(dpy, False);
	assert(XQueryTree(dpy, root, &r, &parent, &wins, &n));
	for (i = 0; i < n; i++) {
		Client *c = wintoclient(wins[i]);
		if (c && isvisible(c))
			top = wins[i];
	}
	XFree(wins);
	return top;
}

int
main(void)
{
	Client *a, *b;
	Window child, actual, dock;
	int revert, i;
	XEvent e = {0};
	XSizeHints hints = {0};
	long strut[12] = {0};
	Atom states[24];
	char name[64];

	dpy = XOpenDisplay(NULL);
	assert(dpy);
	checkotherwm();
	setup();
	XSetErrorHandler(testerror);
	a = newclient();
	b = newclient();
	wss[0].layout = 1;
	arrange();
	focus(a);
	assert(topclient() == a->win);
	viewws(1);
	viewws(0);
	assert(focused == a && topclient() == a->win);

	focus(b);
	child = XCreateSimpleWindow(dpy, b->win, 0, 0, 100, 100, 0, 0, 0);
	XMapWindow(dpy, child);
	XSetInputFocus(dpy, child, RevertToParent, CurrentTime);
	e.xfocus.window = a->win; /* A queued event must not revoke child focus. */
	focusin(&e);
	XGetInputFocus(dpy, &actual, &revert);
	assert(actual == child);
	XSetInputFocus(dpy, a->win, RevertToParent, CurrentTime);
	focusin(&e);
	XGetInputFocus(dpy, &actual, &revert);
	assert(actual == b->win);

	hints.flags = PMinSize | PMaxSize;
	hints.min_width = 100; hints.min_height = 80;
	hints.max_width = 400; hints.max_height = 300;
	XSetWMNormalHints(dpy, b->win, &hints);
	updatesizehints(b);
	b->isfloating = 1;
	e.xconfigurerequest.window = b->win;
	e.xconfigurerequest.value_mask = CWWidth | CWHeight;
	e.xconfigurerequest.width = 900; e.xconfigurerequest.height = 600;
	configurerequest(&e);
	assert(b->w == 400 && b->h == 300);
	resize(b, b->x, b->y, 900, 600); /* Mouse resizing uses this path. */
	assert(b->w == 400 && b->h == 300);
	resize(b, b->x, b->y, 1, 1);
	assert(b->w == 100 && b->h == 80);
	setfullscreen(b, 1);
	assert(b->w == mons[0].mw && b->h == mons[0].mh);
	setfullscreen(b, 0);

	memset(&e, 0, sizeof e);
	e.xclient.window = b->win;
	e.xclient.message_type = atoms[NetWMDesktop];
	e.xclient.data.l[0] = 2;
	clientmessage(&e);
	assert(b->ws == 2 && focused == a && !isvisible(b));
	{
		long ws = -1;
		assert(getprop(b->win, atoms[NetWMDesktop], XA_CARDINAL, &ws, 1) == 1);
		assert(ws == 2);
	}
	e.xclient.data.l[0] = -1;
	clientmessage(&e);
	e.xclient.data.l[0] = NWS;
	clientmessage(&e);
	assert(b->ws == 2);
	memset(&e, 0, sizeof e);
	e.xconfigurerequest.window = b->win;
	e.xconfigurerequest.value_mask = CWWidth | CWHeight;
	e.xconfigurerequest.width = 900; e.xconfigurerequest.height = 600;
	configurerequest(&e);
	assert(b->w == 400 && b->h == 300);
	{
		XWindowAttributes wa;
		assert(XGetWindowAttributes(dpy, b->win, &wa));
		assert(wa.x < 0);
	}
	viewws(2);
	assert(focused == b);

	dock = XCreateSimpleWindow(dpy, root, 0, 0, 1280, 30, 0, 0, 0);
	strut[2] = 30; strut[9] = 1279;
	XChangeProperty(dpy, dock, atoms[NetWMStrutPartial], XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)strut, 12);
	managedock(dock);
	assert(mons[0].wy == 30);
	XDeleteProperty(dpy, dock, atoms[NetWMStrutPartial]);
	memset(&e, 0, sizeof e);
	e.xproperty.window = dock;
	e.xproperty.atom = atoms[NetWMStrutPartial];
	e.xproperty.state = PropertyDelete;
	propertynotify(&e);
	assert(mons[0].wy == 0);

	for (i = 0; i < (int)LENGTH(states); i++) {
		snprintf(name, sizeof name, "_TWMX_TEST_STATE_%d", i);
		states[i] = XInternAtom(dpy, name, False);
	}
	XChangeProperty(dpy, b->win, atoms[NetWMState], XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)states, LENGTH(states));
	setfullscreen(b, 1);
	setfullscreen(b, 1);
	setfullscreen(b, 0);
	{
		long got[25];
		assert(getprop(b->win, atoms[NetWMState], XA_ATOM, got, LENGTH(got)) == 24);
		for (i = 0; i < 24; i++)
			assert((Atom)got[i] == states[i]);
	}
	cleanup();
	XCloseDisplay(dpy);
	puts("All six regression checks passed.");
	return 0;
}
