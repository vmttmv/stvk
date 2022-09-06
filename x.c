/* See LICENSE for license details. */
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"
#include "vk.h"

/* types used in config.h */
typedef struct {
        uint mod;
        KeySym keysym;
        void (*func)(const Arg *);
        const Arg arg;
} Shortcut;

typedef struct {
        uint mod;
        uint button;
        void (*func)(const Arg *);
        const Arg arg;
        uint  release;
} MouseShortcut;

typedef struct {
        KeySym k;
        uint mask;
        char *s;
        /* three-valued logic variables: 0 indifferent, 1 on, -1 off */
        signed char appkey;    /* application keypad */
        signed char appcursor; /* application cursor */
} Key;

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13)

/* function definitions used in config.h */
static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);
static void ttysend(const Arg *);

/* config.h for applying patches and the configuration. */
#include "config.h"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* macros */
#define IS_SET(flag)		((win.mode & (flag)) != 0)
#define TRUERED(x)              (((x) >> 16) & 0xff)
#define TRUEGREEN(x)            (((x) >> 8) & 0xff)
#define TRUEBLUE(x)             ((x) & 0xff)

typedef struct {
        int offx;
        int offy;
        uint16_t w;
        uint16_t h;
        uint16_t uvx;
        uint16_t uvy;
} GlyphSpec;

/* Purely graphic info */
typedef struct {
        int tw, th; /* tty width and height */
        int w, h; /* window width and height */
        int ch; /* char height */
        int cw; /* char width  */
        int mode; /* window state/mode flags */
        int cursor; /* cursor style */
} TermWindow;

typedef struct {
        Display *dpy;
        Colormap cmap;
        Window win;
        Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid;
        struct {
                XIM xim;
                XIC xic;
                XPoint spot;
                XVaNestedList spotlist;
        } ime;
        Visual *vis;
        XSetWindowAttributes attrs;
        int scr;
        int isfixed; /* is fixed geometry? */
        int l, t; /* left and top offset */
        int gm; /* geometry mask */
} XWindow;

typedef struct {
        Atom xtarget;
        char *primary, *clipboard;
        struct timespec tclick1;
        struct timespec tclick2;
} XSelection;

#define FONTDPI         96
#define NOKEY           0xffffffff
#define MAPINITSZ       256

/* Font structure */
#define Font Font_
typedef struct {
        int height;
        int width;
        int ascent;
        int descent;
        int badslant;
        int badweight;
        FcPattern *pattern;
        FcPattern *match;
        FcFontSet *set;
        FT_Face face;

        /* Glyph cache */
        size_t nb; /* num buckets */
        size_t ng; /* num glyphs */
        Rune *keys;
        GlyphSpec *vals;
} Font;

/* Drawing Context */
typedef struct {
        Color *col;
        size_t collen;
        FT_Library ft;
        Font font, bfont, ifont, ibfont;
} DC;

static inline void rehash(Font *);
static inline GlyphSpec *getglyphspec(Font *, Rune);
static void xdrawglyphs(Glyph *, int, int, int);
static void xclear(int, int, int, int);
static int xgeommasktogravity(int);
static int ximopen(Display *);
static void ximinstantiate(Display *, XPointer, XPointer);
static void ximdestroy(XIM, XPointer, XPointer);
static int xicdestroy(XIC, XPointer, XPointer);
static void xinit(int, int);
static void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static inline Color rgb16_to_8(XColor *);
static void xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(char *, double);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void xsetenv(void);
static void xseturgency(int);
static int evcol(XEvent *);
static int evrow(XEvent *);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static uint buttonmask(uint);
static int mouseaction(XEvent *, uint);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);
static void setsel(char *, Time);
static void mousesel(XEvent *, int);
static void mousereport(XEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);

static void run(void);
static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
        [KeyPress] = kpress,
        [ClientMessage] = cmessage,
        [ConfigureNotify] = resize,
        [VisibilityNotify] = visibility,
        [UnmapNotify] = unmap,
        [Expose] = expose,
        [FocusIn] = focus,
        [FocusOut] = focus,
        [MotionNotify] = bmotion,
        [ButtonPress] = bpress,
        [ButtonRelease] = brelease,
        /*
         * Uncomment if you want the selection to disappear when you select something
         * different in another window.
         */
        /*	[SelectionClear] = selclear_, */
        [SelectionNotify] = selnotify,
        /*
         * PropertyNotify is only turned on when there is some INCR transfer happening
         * for the selection retrieval.
         */
        [PropertyNotify] = propnotify,
        [SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static XSelection xsel;
static TermWindow win;

/* Font Ring Cache */
enum {
        FRC_NORMAL,
        FRC_ITALIC,
        FRC_BOLD,
        FRC_ITALICBOLD
};

typedef struct {
        Font font;
        int flags;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;

static int oldbutton = 3; /* button event on startup: 3 = release */

void
clipcopy(const Arg *dummy)
{
        Atom clipboard;

        free(xsel.clipboard);
        xsel.clipboard = NULL;

        if (xsel.primary != NULL) {
                xsel.clipboard = xstrdup(xsel.primary);
                clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
                XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
        }
}

void
clippaste(const Arg *dummy)
{
        Atom clipboard;

        clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
        XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard,
                        xw.win, CurrentTime);
}

void
selpaste(const Arg *dummy)
{
        XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY,
                        xw.win, CurrentTime);
}

void
numlock(const Arg *dummy)
{
        win.mode ^= MODE_NUMLOCK;
}

void
zoom(const Arg *arg)
{
        Arg larg;

        larg.f = usedfontsize + arg->f;
        zoomabs(&larg);
}

void
zoomabs(const Arg *arg)
{
        xunloadfonts();
        xloadfonts(usedfont, arg->f);
        cresize(0, 0);
        redraw();
        xhints();
}

void
zoomreset(const Arg *arg)
{
        Arg larg;

        if (defaultfontsize > 0) {
                larg.f = defaultfontsize;
                zoomabs(&larg);
        }
}

void
ttysend(const Arg *arg)
{
        ttywrite(arg->s, strlen(arg->s), 1);
}

int
evcol(XEvent *e)
{
        int x = e->xbutton.x - borderpx;
        LIMIT(x, 0, win.tw - 1);
        return x / win.cw;
}

int
evrow(XEvent *e)
{
        int y = e->xbutton.y - borderpx;
        LIMIT(y, 0, win.th - 1);
        return y / win.ch;
}

void
mousesel(XEvent *e, int done)
{
        int type, seltype = SEL_REGULAR;
        uint state = e->xbutton.state & ~(Button1Mask | forcemousemod);

        for (type = 1; type < LEN(selmasks); ++type) {
                if (match(selmasks[type], state)) {
                        seltype = type;
                        break;
                }
        }
        selextend(evcol(e), evrow(e), seltype, done);
        if (done)
                setsel(getsel(), e->xbutton.time);
}

void
mousereport(XEvent *e)
{
        int len, x = evcol(e), y = evrow(e),
            button = e->xbutton.button, state = e->xbutton.state;
        char buf[40];
        static int ox, oy;

        /* from urxvt */
        if (e->xbutton.type == MotionNotify) {
                if (x == ox && y == oy)
                        return;
                if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
                        return;
                /* MOUSE_MOTION: no reporting if no button is pressed */
                if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
                        return;

                button = oldbutton + 32;
                ox = x;
                oy = y;
        } else {
                if (!IS_SET(MODE_MOUSESGR) && e->xbutton.type == ButtonRelease) {
                        button = 3;
                } else {
                        button -= Button1;
                        if (button >= 3)
                                button += 64 - 3;
                }
                if (e->xbutton.type == ButtonPress) {
                        oldbutton = button;
                        ox = x;
                        oy = y;
                } else if (e->xbutton.type == ButtonRelease) {
                        oldbutton = 3;
                        /* MODE_MOUSEX10: no button release reporting */
                        if (IS_SET(MODE_MOUSEX10))
                                return;
                        if (button == 64 || button == 65)
                                return;
                }
        }

        if (!IS_SET(MODE_MOUSEX10)) {
                button += ((state & ShiftMask  ) ? 4  : 0)
                        + ((state & Mod4Mask   ) ? 8  : 0)
                        + ((state & ControlMask) ? 16 : 0);
        }

        if (IS_SET(MODE_MOUSESGR)) {
                len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
                                button, x+1, y+1,
                                e->xbutton.type == ButtonRelease ? 'm' : 'M');
        } else if (x < 223 && y < 223) {
                len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
                                32+button, 32+x+1, 32+y+1);
        } else {
                return;
        }

        ttywrite(buf, len, 0);
}

uint
buttonmask(uint button)
{
        return button == Button1 ? Button1Mask
                : button == Button2 ? Button2Mask
                : button == Button3 ? Button3Mask
                : button == Button4 ? Button4Mask
                : button == Button5 ? Button5Mask
                : 0;
}

int
mouseaction(XEvent *e, uint release)
{
        MouseShortcut *ms;

        /* ignore Button<N>mask for Button<N> - it's set on release */
        uint state = e->xbutton.state & ~buttonmask(e->xbutton.button);

        for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
                if (ms->release == release &&
                                ms->button == e->xbutton.button &&
                                (match(ms->mod, state) ||  /* exact or forced */
                                 match(ms->mod, state & ~forcemousemod))) {
                        ms->func(&(ms->arg));
                        return 1;
                }
        }

        return 0;
}

void
bpress(XEvent *e)
{
        struct timespec now;
        int snap;

        if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
                mousereport(e);
                return;
        }

        if (mouseaction(e, 0))
                return;

        if (e->xbutton.button == Button1) {
                /*
                 * If the user clicks below predefined timeouts specific
                 * snapping behaviour is exposed.
                 */
                clock_gettime(CLOCK_MONOTONIC, &now);
                if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
                        snap = SNAP_LINE;
                } else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
                        snap = SNAP_WORD;
                } else {
                        snap = 0;
                }
                xsel.tclick2 = xsel.tclick1;
                xsel.tclick1 = now;

                selstart(evcol(e), evrow(e), snap);
        }
}

void
propnotify(XEvent *e)
{
        XPropertyEvent *xpev;
        Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

        xpev = &e->xproperty;
        if (xpev->state == PropertyNewValue &&
                        (xpev->atom == XA_PRIMARY ||
                         xpev->atom == clipboard)) {
                selnotify(e);
        }
}

void
selnotify(XEvent *e)
{
        ulong nitems, ofs, rem;
        int format;
        uchar *data, *last, *repl;
        Atom type, incratom, property = None;

        incratom = XInternAtom(xw.dpy, "INCR", 0);

        ofs = 0;
        if (e->type == SelectionNotify)
                property = e->xselection.property;
        else if (e->type == PropertyNotify)
                property = e->xproperty.atom;

        if (property == None)
                return;

        do {
                if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
                                        BUFSIZ/4, False, AnyPropertyType,
                                        &type, &format, &nitems, &rem,
                                        &data)) {
                        fprintf(stderr, "Clipboard allocation failed\n");
                        return;
                }

                if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
                        /*
                         * If there is some PropertyNotify with no data, then
                         * this is the signal of the selection owner that all
                         * data has been transferred. We won't need to receive
                         * PropertyNotify events anymore.
                         */
                        MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
                        XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
                                        &xw.attrs);
                }

                if (type == incratom) {
                        /*
                         * Activate the PropertyNotify events so we receive
                         * when the selection owner does send us the next
                         * chunk of data.
                         */
                        MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
                        XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
                                        &xw.attrs);

                        /*
                         * Deleting the property is the transfer start signal.
                         */
                        XDeleteProperty(xw.dpy, xw.win, (int)property);
                        continue;
                }

                /*
                 * As seen in getsel:
                 * Line endings are inconsistent in the terminal and GUI world
                 * copy and pasting. When receiving some selection data,
                 * replace all '\n' with '\r'.
                 * FIXME: Fix the computer world.
                 */
                repl = data;
                last = data + nitems * format / 8;
                while ((repl = memchr(repl, '\n', last - repl))) {
                        *repl++ = '\r';
                }

                if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
                        ttywrite("\033[200~", 6, 0);
                ttywrite((char *)data, nitems * format / 8, 1);
                if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
                        ttywrite("\033[201~", 6, 0);
                XFree(data);
                /* number of 32-bit chunks returned */
                ofs += nitems * format / 32;
        } while (rem > 0);

        /*
         * Deleting the property again tells the selection owner to send the
         * next data chunk in the property.
         */
        XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void
xclipcopy(void)
{
        clipcopy(NULL);
}

void
selclear_(XEvent *e)
{
        selclear();
}

void
selrequest(XEvent *e)
{
        XSelectionRequestEvent *xsre;
        XSelectionEvent xev;
        Atom xa_targets, string, clipboard;
        char *seltext;

        xsre = (XSelectionRequestEvent *) e;
        xev.type = SelectionNotify;
        xev.requestor = xsre->requestor;
        xev.selection = xsre->selection;
        xev.target = xsre->target;
        xev.time = xsre->time;
        if (xsre->property == None)
                xsre->property = xsre->target;

        /* reject */
        xev.property = None;

        xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
        if (xsre->target == xa_targets) {
                /* respond with the supported type */
                string = xsel.xtarget;
                XChangeProperty(xsre->display, xsre->requestor, xsre->property,
                                XA_ATOM, 32, PropModeReplace,
                                (uchar *) &string, 1);
                xev.property = xsre->property;
        } else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
                /*
                 * xith XA_STRING non ascii characters may be incorrect in the
                 * requestor. It is not our problem, use utf8.
                 */
                clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
                if (xsre->selection == XA_PRIMARY) {
                        seltext = xsel.primary;
                } else if (xsre->selection == clipboard) {
                        seltext = xsel.clipboard;
                } else {
                        fprintf(stderr,
                                        "Unhandled clipboard selection 0x%lx\n",
                                        xsre->selection);
                        return;
                }
                if (seltext != NULL) {
                        XChangeProperty(xsre->display, xsre->requestor,
                                        xsre->property, xsre->target,
                                        8, PropModeReplace,
                                        (uchar *)seltext, strlen(seltext));
                        xev.property = xsre->property;
                }
        }

        /* all done, send a notification to the listener */
        if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
                fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
setsel(char *str, Time t)
{
        if (!str)
                return;

        free(xsel.primary);
        xsel.primary = str;

        XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
        if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
                selclear();
}

void
xsetsel(char *str)
{
        setsel(str, CurrentTime);
}

void
brelease(XEvent *e)
{
        if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
                mousereport(e);
                return;
        }

        if (mouseaction(e, 1))
                return;
        if (e->xbutton.button == Button1)
                mousesel(e, 1);
}

void
bmotion(XEvent *e)
{
        if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
                mousereport(e);
                return;
        }

        mousesel(e, 0);
}

void
cresize(int width, int height)
{
        int col, row;

        if (width != 0)
                win.w = width;
        if (height != 0)
                win.h = height;

        col = (win.w - 2 * borderpx) / win.cw;
        row = (win.h - 2 * borderpx) / win.ch;
        col = MAX(1, col);
        row = MAX(1, row);

        tresize(col, row);
        xresize(col, row);
        ttyresize(win.tw, win.th);
        vkresize(width, height);
}

void
xresize(int col, int row)
{
        win.tw = col * win.cw;
        win.th = row * win.ch;
        xclear(0, 0, win.w, win.h);
}

Color
rgb16_to_8(XColor *color)
{
        Color c;

        c.r = (uint8_t)(color->red >> 8);
        c.g = (uint8_t)(color->green >> 8);
        c.b = (uint8_t)(color->blue >> 8);
        c.a = 0xff;

        return c;
}

void
xloadcolor(int i, const char *name, Color *ncolor)
{
        const uint8_t valuerange[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
        uint8_t v;
        XColor col, dcol;

        if (!name) {
                if (BETWEEN(i, 16, 255)) {
                        if (i < 6*6*6+16) {
                                i -= 16;
                                ncolor->r = valuerange[(i/36) % 6];
                                ncolor->g = valuerange[(i/6) % 6];
                                ncolor->b = valuerange[i % 6];
                                ncolor->a = 0xff;
                        } else {
                                i -= (6*6*6+16);
                                v = 8 + i*10;
                                ncolor->r = ncolor->g = ncolor->b = v;
                                ncolor->a = 0xff;
                        }

                        return;
                } else {
                        name = colorname[i];
                }
        }

        XLookupColor(xw.dpy, xw.cmap, name, &col, &dcol);
        *ncolor = rgb16_to_8(&col);
}

void
xloadcols(void)
{
        int i;
        static int loaded;

        if (!loaded) {
                dc.collen = MAX(LEN(colorname), 256);
                dc.col = xmalloc(dc.collen * sizeof(Color));
        }

        for (i = 0; i < dc.collen; i++)
                xloadcolor(i, NULL, &dc.col[i]);

        loaded = 1;
}

int
xsetcolorname(int x, const char *name)
{
        if (!BETWEEN(x, 0, dc.collen))
                return 1;

        xloadcolor(x, name, &dc.col[x]);

        return 0;
}

/*
 * Absolute coordinates.
 * TODO: Figure out can x1 > x2 || y1 > y2?
 */
void
xclear(int x1, int y1, int x2, int y2)
{
        uint16_t w, h;
        Color col;

        w = (uint16_t)(x2 - x1);
        h = (uint16_t)(y2 - y1);
        col = dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg];
        vkpushquad((uint16_t)x1, (uint16_t)y1, w, h, NOUV, NOUV, col, col);
}

void
xhints(void)
{
        XClassHint class = {opt_name ? opt_name : termname,
                opt_class ? opt_class : termname};
        XWMHints wm = {.flags = InputHint, .input = 1};
        XSizeHints *sizeh;

        sizeh = XAllocSizeHints();

        sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
        sizeh->height = win.h;
        sizeh->width = win.w;
        sizeh->height_inc = win.ch;
        sizeh->width_inc = win.cw;
        sizeh->base_height = 2 * borderpx;
        sizeh->base_width = 2 * borderpx;
        sizeh->min_height = win.ch + 2 * borderpx;
        sizeh->min_width = win.cw + 2 * borderpx;
        if (xw.isfixed) {
                sizeh->flags |= PMaxSize;
                sizeh->min_width = sizeh->max_width = win.w;
                sizeh->min_height = sizeh->max_height = win.h;
        }
        if (xw.gm & (XValue|YValue)) {
                sizeh->flags |= USPosition | PWinGravity;
                sizeh->x = xw.l;
                sizeh->y = xw.t;
                sizeh->win_gravity = xgeommasktogravity(xw.gm);
        }

        XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
                        &class);
        XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
        switch (mask & (XNegative|YNegative)) {
                case 0:
                        return NorthWestGravity;
                case XNegative:
                        return NorthEastGravity;
                case YNegative:
                        return SouthWestGravity;
        }

        return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
        FcPattern *configured;
        FcPattern *match;
        FcResult result;
        int wantattr, haveattr, index, glyphidx, h;
        char *path;
        FT_Size_Metrics metrics;
        FT_GlyphSlot slot;
        FT_Bitmap bitmap;

        /*
         * Manually configure instead of calling XftMatchFont
         * so that we can use the configured pattern for
         * "missing glyph" lookups.
         */
        configured = FcPatternDuplicate(pattern);
        if (!configured)
                return 1;

        FcConfigSubstitute(NULL, configured, FcMatchPattern);
        FcDefaultSubstitute(configured);
        match = FcFontMatch(NULL, configured, &result);
        if (!match) {
                fputs("sadface1\n", stderr);
                FcPatternDestroy(configured);
                return 1;
        }

        if ((FcPatternGetInteger(pattern, "slant", 0, &wantattr) == FcResultMatch)) {
                /*
                 * Check if xft was unable to find a font with the appropriate
                 * slant but gave us one anyway. Try to mitigate.
                 */
                if ((FcPatternGetInteger(match, "slant", 0, &haveattr) != FcResultMatch) ||
                                haveattr < wantattr) {
                        f->badslant = 1;
                        fputs("font slant does not match\n", stderr);
                }
        }

        if ((FcPatternGetInteger(pattern, "weight", 0, &wantattr) == FcResultMatch)) {
                if ((FcPatternGetInteger(match, "weight", 0, &haveattr) != FcResultMatch) ||
                                haveattr != wantattr) {
                        f->badweight = 1;
                        fputs("font weight does not match\n", stderr);
                }
        }

        if (FcPatternGetString(match, FC_FILE, 0, (FcChar8 **)&path) != FcResultMatch) {
                fputs("failed to get font file\n", stderr);
                return 1;
        }

        if (FcPatternGetInteger(match, FC_INDEX, 0, &index) != FcResultMatch) {
                fputs("failed to get font index\n", stderr);
                return 1;
        }

        if (FT_New_Face(dc.ft, path, index, &f->face)) {
                fputs("failed to open font file\n", stderr);
                return 1;
        }

        if (FT_Set_Char_Size(f->face, (1<<6)*font_size, 0, FONTDPI, 0)) {
                fputs("failed to set font size\n", stderr);
                return 1;
        }

        metrics = f->face->size->metrics;
        f->pattern = configured;
        f->set = NULL;
        f->ascent = metrics.ascender >> 6;
        f->descent = metrics.descender >> 6;
        f->height = f->ascent - f->descent;

        glyphidx = FT_Get_Char_Index(f->face, 'W');
        if (FT_Load_Glyph(f->face, glyphidx, FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT)) {
                FT_Done_Face(f->face);
                fputs("failed to load glyph\n", stderr);
                return 1;
        }

        slot = f->face->glyph;
        f->width = slot->bitmap.width;

        /* NOTE: This is taken from kitty,
         * fix the font height, in case the font has an underscore glyph which
         * renders outside of the bbox */
        glyphidx = FT_Get_Char_Index(f->face, '_');
        if (FT_Load_Glyph(f->face, glyphidx, FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT)) {
                FT_Done_Face(f->face);
                fputs("failed to load glyph\n", stderr);
                return 1;
        }

        slot = f->face->glyph;
        h = f->ascent - slot->bitmap_top + slot->bitmap.rows;
        if (h > f->height)
                f->height = h;

        /* Allocate the initial cache */
        f->keys = xmalloc(MAPINITSZ * sizeof *f->keys);
        memset(f->keys, 0xff, MAPINITSZ * sizeof *f->keys);
        f->vals = xmalloc(MAPINITSZ * sizeof *f->vals);
        f->nb = MAPINITSZ;
        f->ng = 0;

        return 0;
}

void
xloadfonts(char *fontstr, double fontsize)
{
        FcPattern *pattern;
        double fontval;

        if (FT_Init_FreeType(&dc.ft))
                die("can't initialize freetype\n");

        pattern = FcNameParse((FcChar8 *)fontstr);
        if (!pattern)
                die("can't open font %s\n", fontstr);

        if (xloadfont(&dc.font, pattern))
                die("can't open font %s\n", fontstr);

        /* Setting character width and height. */
        win.cw = ceilf(dc.font.width * cwscale);
        win.ch = ceilf(dc.font.height * chscale);

        FcPatternDel(pattern, FC_SLANT);
        FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
        if (xloadfont(&dc.ifont, pattern))
                die("can't open font (italic) %s\n", fontstr);

        FcPatternDel(pattern, FC_WEIGHT);
        FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
        if (xloadfont(&dc.ibfont, pattern))
                die("can't open font (bold) %s\n", fontstr);

        FcPatternDel(pattern, FC_SLANT);
        FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
        if (xloadfont(&dc.bfont, pattern))
                die("can't open font (bold) %s\n", fontstr);

        FcPatternDestroy(pattern);
}

void
xunloadfont(Font *f)
{
        FT_Done_Face(f->face);
        FcPatternDestroy(f->pattern);
        free(f->keys);
        free(f->vals);
}

void
xunloadfonts(void)
{
        /* Free the loaded fonts in the font cache.  */
        while (frclen > 0)
                xunloadfont(&frc[--frclen].font);

        xunloadfont(&dc.font);
        xunloadfont(&dc.bfont);
        xunloadfont(&dc.ifont);
        xunloadfont(&dc.ibfont);
}

int
ximopen(Display *dpy)
{
        XIMCallback imdestroy = { .client_data = NULL, .callback = ximdestroy };
        XICCallback icdestroy = { .client_data = NULL, .callback = xicdestroy };

        xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
        if (xw.ime.xim == NULL)
                return 0;

        if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
                fprintf(stderr, "XSetIMValues: "
                                "Could not set XNDestroyCallback.\n");

        xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot,
                        NULL);

        if (xw.ime.xic == NULL) {
                xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
                                XIMPreeditNothing | XIMStatusNothing,
                                XNClientWindow, xw.win,
                                XNDestroyCallback, &icdestroy,
                                NULL);
        }
        if (xw.ime.xic == NULL)
                fprintf(stderr, "XCreateIC: Could not create input context.\n");

        return 1;
}

void
ximinstantiate(Display *dpy, XPointer client, XPointer call)
{
        if (ximopen(dpy))
                XUnregisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
                                ximinstantiate, NULL);
}

void
ximdestroy(XIM xim, XPointer client, XPointer call)
{
        xw.ime.xim = NULL;
        XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
                        ximinstantiate, NULL);
        XFree(xw.ime.spotlist);
}

int
xicdestroy(XIC xim, XPointer client, XPointer call)
{
        xw.ime.xic = NULL;
        return 1;
}

void
xinit(int cols, int rows)
{
        Cursor cursor;
        Window parent;
        pid_t thispid = getpid();
        XColor xmousefg, xmousebg;

        if (!(xw.dpy = XOpenDisplay(NULL)))
                die("can't open display\n");
        xw.scr = XDefaultScreen(xw.dpy);
        xw.vis = XDefaultVisual(xw.dpy, xw.scr);

        /* font */
        if (!FcInit())
                die("could not init fontconfig.\n");

        usedfont = (opt_font == NULL)? font : opt_font;
        xloadfonts(usedfont, 0);

        /* colors */
        xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
        xloadcols();

        /* adjust fixed window geometry */
        win.w = 2 * borderpx + cols * win.cw;
        win.h = 2 * borderpx + rows * win.ch;
        if (xw.gm & XNegative)
                xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
        if (xw.gm & YNegative)
                xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

        /* Events */
        xw.attrs.bit_gravity = NorthWestGravity;
        xw.attrs.event_mask = FocusChangeMask | KeyPressMask | KeyReleaseMask
                | ExposureMask | VisibilityChangeMask | StructureNotifyMask
                | ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
        xw.attrs.colormap = xw.cmap;

        if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
                parent = XRootWindow(xw.dpy, xw.scr);
        xw.win = XCreateWindow(xw.dpy, parent, xw.l, xw.t,
                        win.w, win.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
                        xw.vis, CWBitGravity | CWEventMask | CWColormap, &xw.attrs);

        /* TODO: Clear the screen */

        /* input methods */
        if (!ximopen(xw.dpy)) {
                XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
                                ximinstantiate, NULL);
        }

        /* white cursor, black outline */
        cursor = XCreateFontCursor(xw.dpy, mouseshape);
        XDefineCursor(xw.dpy, xw.win, cursor);

        if (XParseColor(xw.dpy, xw.cmap, colorname[mousefg], &xmousefg) == 0) {
                xmousefg.red   = 0xffff;
                xmousefg.green = 0xffff;
                xmousefg.blue  = 0xffff;
        }

        if (XParseColor(xw.dpy, xw.cmap, colorname[mousebg], &xmousebg) == 0) {
                xmousebg.red   = 0x0000;
                xmousebg.green = 0x0000;
                xmousebg.blue  = 0x0000;
        }

        XRecolorCursor(xw.dpy, cursor, &xmousefg, &xmousebg);

        xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
        xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
        xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
        xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
        XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

        xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
        XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
                        PropModeReplace, (uchar *)&thispid, 1);

        win.mode = MODE_NUMLOCK;
        resettitle();
        xhints();
        XMapWindow(xw.dpy, xw.win);
        XSync(xw.dpy, False);

        if (vkinit(xw.dpy, xw.win, win.w, win.h))
                die("can't initialize vulkan");

        clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
        clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
        xsel.primary = NULL;
        xsel.clipboard = NULL;
        xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
        if (xsel.xtarget == None)
                xsel.xtarget = XA_STRING;
}

void
rehash(Font *f)
{
        size_t i, nb, idx;
        Rune *newkeys;
        GlyphSpec *newvals, *val;

        nb = f->nb*2;
        newkeys = xmalloc(nb * sizeof *newkeys);
        memset(newkeys, 0xff, nb * sizeof *newkeys);
        newvals = xmalloc(nb * sizeof *newvals);

        /* NOTE: This loop could be faster with an early-exit */
        for (i = 0; i < f->nb; i++) {
                if (f->keys[i] == NOKEY)
                        continue;

                idx = f->keys[i] % nb;
                while (newkeys[idx] != NOKEY)
                        idx = (idx+1) % nb;

                newkeys[idx] = f->keys[i];
                newvals[idx] = f->vals[i];
        }

        free(f->keys);
        free(f->vals);
        f->keys = newkeys;
        f->vals = newvals;
        f->nb = nb;
}

GlyphSpec *
getglyphspec(Font *f, Rune u)
{
        size_t i, idx;
        int ox, oy, ret;
        uint16_t cw, ch;
        float occ;
        FT_UInt glyphidx;
        FT_Error error;
        FT_GlyphSlot slot;
        FT_Bitmap bitmap;
        GlyphSpec *spec;

        idx = u % f->nb;
        while (f->keys[idx] != NOKEY && f->keys[idx] != u)
                idx = (idx+1) % f->nb;

        if (f->keys[idx] != NOKEY)
                return f->vals + idx;

        /* Not cached, load glyph */
        glyphidx = FT_Get_Char_Index(f->face, u);
        if (glyphidx == 0) {
                /* Can't find the glyph in the font. Use fontcache to get
                 * a fallback font TODO: Implement this. */
                return NULL;
        } else {
                error = FT_Load_Glyph(f->face, glyphidx, FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT);
                if (error) {
                        fputs("freetype load glyph error: %02x\n", stderr);
                        return NULL;
                }

                occ = (float)f->ng / (float)f->nb;
                if (occ > 0.75f) {
                        rehash(f);
                        idx = u % f->nb;
                        while (f->keys[idx] != NOKEY && f->keys[idx] != u)
                                idx = (idx+1) % f->nb;
                }

                spec = f->vals + idx;
                slot = f->face->glyph;
                bitmap = slot->bitmap;

                ox = slot->bitmap_left;
                oy = f->ascent - slot->bitmap_top;
                cw = MAX(win.cw, bitmap.width);
                ch = MAX(win.ch, bitmap.rows);

                ret = blitatlas(&spec->uvx, &spec->uvy, bitmap.width, bitmap.rows,
                                cw, ch, MAX(0, ox), MAX(0, oy),
                                bitmap.pitch, bitmap.buffer);
                if (ret) {
                        fputs("font atlas is full\n", stderr);
                        return NULL;
                }

                f->keys[idx] = u;
                spec->w = cw;
                spec->h = ch;
                spec->offx = MIN(0, ox);
                spec->offy = MIN(0, oy);
                f->ng++;

                return spec;
        }

        return NULL;
}

void
xdrawglyphs(Glyph *glyphs, int len, int x, int y)
{
        Font *font = &dc.font;
        int i, j;
        int frcflags = FRC_NORMAL;
        Glyph *g;
        GlyphSpec *spec;
        uint16_t xp, yp, runewidth;
        Color fg, bg, tmp;
        FcResult fcres;
        FcPattern *fcpattern, *fontpattern;
        FcFontSet *fcsets[] = { NULL };
        FcCharSet *fccharset;

        xp = (uint16_t)(x*win.cw + borderpx);
        yp = (uint16_t)(y*win.ch + borderpx);
        for (i = 0; i < len; i++) {
                g = glyphs + i;
                if (g->mode & ATTR_WDUMMY)
                        continue;

                runewidth = win.cw * ((g->mode & ATTR_WIDE) ? 2 : 1);

                if ((g->mode & ATTR_ITALIC) && (g->mode & ATTR_BOLD)) {
                        font = &dc.ibfont;
                        frcflags = FRC_ITALICBOLD;
                } else if (g->mode & ATTR_ITALIC) {
                        font = &dc.ifont;
                        frcflags = FRC_ITALIC;
                } else if (g->mode & ATTR_BOLD) {
                        font = &dc.bfont;
                        frcflags = FRC_BOLD;
                } else {
                        font = &dc.font;
                }

                if (g->mode & ATTR_ITALIC && g->mode & ATTR_BOLD) {
                        if (dc.ibfont.badslant || dc.ibfont.badweight)
                                g->fg = defaultattr;
                } else if ((g->mode & ATTR_ITALIC && dc.ifont.badslant) ||
                                (g->mode & ATTR_BOLD && dc.bfont.badweight)) {
                        g->fg = defaultattr;
                }

                if (IS_TRUECOL(g->fg)) {
                        fg.r = TRUERED(g->fg);
                        fg.g = TRUEGREEN(g->fg);
                        fg.b = TRUEBLUE(g->fg);
                } else {
                        fg = dc.col[g->fg];
                }

                if (IS_TRUECOL(g->bg)) {
                        bg.r = TRUERED(g->bg);
                        bg.g = TRUEGREEN(g->bg);
                        bg.b = TRUEBLUE(g->bg);
                } else {
                        bg = dc.col[g->bg];
                }

                if ((g->mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(g->fg, 0, 7))
                        fg = dc.col[g->fg + 8];

                if (IS_SET(MODE_REVERSE)) {
                        if (COLOREQ(fg, dc.col[defaultfg])) {
                                fg = dc.col[defaultbg];
                        } else {
                                fg.r = ~fg.r;
                                fg.g = ~fg.g;
                                fg.b = ~fg.b;
                        }

                        if (COLOREQ(bg, dc.col[defaultbg])) {
                                bg = dc.col[defaultfg];
                        } else {
                                bg.r = ~bg.r;
                                bg.g = ~bg.g;
                                bg.b = ~bg.b;
                        }
                }

                if ((g->mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
                        fg.r /= 2;
                        fg.g /= 2;
                        fg.b /= 2;
                }

                if (g->mode & ATTR_REVERSE) {
                        tmp = fg;
                        fg = bg;
                        bg = tmp;
                }

                if (g->mode & ATTR_BLINK && win.mode & MODE_BLINK)
                        fg = bg;

                if (g->mode & ATTR_INVISIBLE)
                        fg = bg;

                spec = getglyphspec(font, g->u);
                if (spec) {
                        vkpushquad(xp + spec->offx, yp - spec->offy,
                                   spec->w, spec->h,
                                   spec->uvx, spec->uvy, fg, bg);
                } else {
                        /* Look up the cache */
                        for (j = 0; j < frclen; j++) {
                                if (frc[j].flags == frcflags) {
                                        spec = getglyphspec(&frc[j].font, g->u);
                                        if (spec) {
                                                font = &frc[j].font;
                                                break;
                                        }
                                }
                        }
                        if (spec) {
                                vkpushquad(xp + spec->offx, yp - spec->offy,
                                           spec->w, spec->h,
                                           spec->uvx, spec->uvy, fg, bg);
                        } else {
                                if (!font->set)
                                        font->set = FcFontSort(0, font->pattern,
                                                               1, 0, &fcres);
                                fcsets[0] = font->set;

                                fcpattern = FcPatternDuplicate(font->pattern);
                                fccharset = FcCharSetCreate();

                                FcCharSetAddChar(fccharset, g->u);
                                FcPatternAddCharSet(fcpattern, FC_CHARSET,
                                                fccharset);
                                FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

                                FcConfigSubstitute(0, fcpattern,
                                                FcMatchPattern);
                                FcDefaultSubstitute(fcpattern);

                                fontpattern = FcFontSetMatch(0, fcsets, 1,
                                                fcpattern, &fcres);
                                if (frclen >= frccap) {
                                        frccap += 16;
                                        frc = xrealloc(frc, frccap * sizeof(Fontcache));
                                }
                                font = &frc[frclen].font;
                                memset(font, 0, sizeof *font);
                                if (xloadfont(font, fontpattern))
                                        die("Failed to load fallback font");
                                frc[frclen].flags = frcflags;
                                frclen++;
                                spec = getglyphspec(font, g->u);
                                if (spec)
                                        vkpushquad(xp + spec->offx, yp - spec->offy,
                                                   spec->w, spec->h,
                                                   spec->uvx, spec->uvy, fg, bg);
                                else
                                        vkpushquad(xp, yp, win.cw, win.ch, NOUV, NOUV, fg, bg);

                                FcPatternDestroy(fcpattern);
                                FcCharSetDestroy(fccharset);
                        }
                }

                /* TODO: Change to the original st-style, in which the modes are lumped together,
                 * so that drawing underline and strikethrough is more efficient */

                if (g->mode & ATTR_UNDERLINE)
                        vkpushquad(xp, yp + font->ascent + 1, runewidth, 1, NOUV, NOUV, fg, fg);

                if (g->mode & ATTR_STRUCK)
                        vkpushquad(xp, yp + 2*font->ascent/3, runewidth, 1, NOUV, NOUV, fg, fg);

                xp += runewidth;
        }

}

void
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
        Color drawcol;

        /* remove the old cursor */
        if (selected(ox, oy))
                og.mode ^= ATTR_REVERSE;
        xdrawglyphs(&og, 1, ox, oy);

        if (IS_SET(MODE_HIDE))
                return;

        /*
         * Select the right color for the right mode.
         */
        g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE;

        if (IS_SET(MODE_REVERSE)) {
                g.mode |= ATTR_REVERSE;
                g.bg = defaultfg;
                if (selected(cx, cy)) {
                        drawcol = dc.col[defaultcs];
                        g.fg = defaultrcs;
                } else {
                        drawcol = dc.col[defaultrcs];
                        g.fg = defaultcs;
                }
        } else {
                if (selected(cx, cy)) {
                        g.fg = defaultfg;
                        g.bg = defaultrcs;
                } else {
                        g.fg = defaultbg;
                        g.bg = defaultcs;
                }
                drawcol = dc.col[g.bg];
        }

        /* draw the new one */
        if (IS_SET(MODE_FOCUSED)) {
                switch (win.cursor) {
                        case 7: /* st extension */
                                g.u = 0x2603; /* snowman (U+2603) */
                                /* FALLTHROUGH */
                        case 0: /* Blinking Block */
                        case 1: /* Blinking Block (Default) */
                        case 2: /* Steady Block */
                                xdrawglyphs(&g, 1, cx, cy);
                                break;
                        case 3: /* Blinking Underline */
                        case 4: /* Steady Underline */
                                vkpushquad(borderpx + cx*win.cw, borderpx + (cy+1)*win.ch - cursorthickness,
                                                win.cw, cursorthickness, NOUV, NOUV, drawcol, drawcol);
                                break;
                        case 5: /* Blinking bar */
                        case 6: /* Steady bar */
                                vkpushquad(borderpx + cx*win.cw, borderpx + cy*win.ch,
                                                cursorthickness, win.ch, NOUV, NOUV, drawcol, drawcol);
                                break;
                }
        } else {
                vkpushquad(borderpx + cx*win.cw, borderpx + cy*win.ch, win.cw-1, 1, NOUV, NOUV, drawcol, drawcol);
                vkpushquad(borderpx + cx*win.cw, borderpx + cy*win.ch, 1, win.ch-1, NOUV, NOUV, drawcol, drawcol);
                vkpushquad(borderpx + (cx+1)*win.cw-1, borderpx + cy*win.ch, 1, win.ch-1, NOUV, NOUV, drawcol, drawcol);
                vkpushquad(borderpx + cx*win.cw, borderpx + (cy+1)*win.ch-1, win.cw, 1, NOUV, NOUV, drawcol, drawcol);
        }
}

void
xsetenv(void)
{
        char buf[sizeof(long) * 8 + 1];

        snprintf(buf, sizeof(buf), "%lu", xw.win);
        setenv("WINDOWID", buf, 1);
}

void
xseticontitle(char *p)
{
        XTextProperty prop;
        DEFAULT(p, opt_title);

        Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
                        &prop);
        XSetWMIconName(xw.dpy, xw.win, &prop);
        XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmiconname);
        XFree(prop.value);
}

void
xsettitle(char *p)
{
        XTextProperty prop;
        DEFAULT(p, opt_title);

        Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
                        &prop);
        XSetWMName(xw.dpy, xw.win, &prop);
        XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
        XFree(prop.value);
}

int
xstartdraw(void)
{
        return IS_SET(MODE_VISIBLE);
}

void
xdrawline(Line line, int x1, int y1, int x2)
{
        xdrawglyphs(&line[x1], x2-x1, x1, y1);
}

void
xfinishdraw(void)
{
        vkflush();
}

void
xximspot(int x, int y)
{
        if (xw.ime.xic == NULL)
                return;

        xw.ime.spot.x = borderpx + x * win.cw;
        xw.ime.spot.y = borderpx + (y + 1) * win.ch;

        XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose(XEvent *ev)
{
        redraw();
}

void
visibility(XEvent *ev)
{
        XVisibilityEvent *e = &ev->xvisibility;

        MODBIT(win.mode, e->state != VisibilityFullyObscured, MODE_VISIBLE);
}

void
unmap(XEvent *ev)
{
        win.mode &= ~MODE_VISIBLE;
}

void
xsetpointermotion(int set)
{
        MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
        XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xsetmode(int set, unsigned int flags)
{
        int mode = win.mode;
        MODBIT(win.mode, set, flags);
        if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE))
                redraw();
}

int
xsetcursor(int cursor)
{
        if (!BETWEEN(cursor, 0, 7)) /* 7: st extension */
                return 1;
        win.cursor = cursor;
        return 0;
}

void
xseturgency(int add)
{
        XWMHints *h = XGetWMHints(xw.dpy, xw.win);

        MODBIT(h->flags, add, XUrgencyHint);
        XSetWMHints(xw.dpy, xw.win, h);
        XFree(h);
}

void
xbell(void)
{
        if (!(IS_SET(MODE_FOCUSED)))
                xseturgency(1);
        if (bellvolume)
                XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
}

void
focus(XEvent *ev)
{
        XFocusChangeEvent *e = &ev->xfocus;

        if (e->mode == NotifyGrab)
                return;

        if (ev->type == FocusIn) {
                if (xw.ime.xic)
                        XSetICFocus(xw.ime.xic);
                win.mode |= MODE_FOCUSED;
                xseturgency(0);
                if (IS_SET(MODE_FOCUS))
                        ttywrite("\033[I", 3, 0);
        } else {
                if (xw.ime.xic)
                        XUnsetICFocus(xw.ime.xic);
                win.mode &= ~MODE_FOCUSED;
                if (IS_SET(MODE_FOCUS))
                        ttywrite("\033[O", 3, 0);
        }
}

int
match(uint mask, uint state)
{
        return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char*
kmap(KeySym k, uint state)
{
        Key *kp;
        int i;

        /* Check for mapped keys out of X11 function keys. */
        for (i = 0; i < LEN(mappedkeys); i++) {
                if (mappedkeys[i] == k)
                        break;
        }
        if (i == LEN(mappedkeys)) {
                if ((k & 0xFFFF) < 0xFD00)
                        return NULL;
        }

        for (kp = key; kp < key + LEN(key); kp++) {
                if (kp->k != k)
                        continue;

                if (!match(kp->mask, state))
                        continue;

                if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
                        continue;

                if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
                        continue;

                if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
                        continue;

                return kp->s;
        }

        return NULL;
}

void
kpress(XEvent *ev)
{
        XKeyEvent *e = &ev->xkey;
        KeySym ksym;
        char buf[64], *customkey;
        int len;
        Rune c;
        Status status;
        Shortcut *bp;

        if (IS_SET(MODE_KBDLOCK))
                return;

        if (xw.ime.xic)
                len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
        else
                len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
        /* 1. shortcuts */
        for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
                if (ksym == bp->keysym && match(bp->mod, e->state)) {
                        bp->func(&(bp->arg));
                        return;
                }
        }

        /* 2. custom keys from config.h */
        if ((customkey = kmap(ksym, e->state))) {
                ttywrite(customkey, strlen(customkey), 1);
                return;
        }

        /* 3. composed string from input method */
        if (len == 0)
                return;
        if (len == 1 && e->state & Mod1Mask) {
                if (IS_SET(MODE_8BIT)) {
                        if (*buf < 0177) {
                                c = *buf | 0x80;
                                len = utf8encode(c, buf);
                        }
                } else {
                        buf[1] = buf[0];
                        buf[0] = '\033';
                        len = 2;
                }
        }
        ttywrite(buf, len, 1);
}

void
cmessage(XEvent *e)
{
        /*
         * See xembed specs
         *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
         */
        if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
                if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
                        win.mode |= MODE_FOCUSED;
                        xseturgency(0);
                } else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
                        win.mode &= ~MODE_FOCUSED;
                }
        } else if (e->xclient.data.l[0] == xw.wmdeletewin) {
                ttyhangup();
                exit(0);
        }
}

void
resize(XEvent *e)
{
        if (e->xconfigure.width == win.w && e->xconfigure.height == win.h)
                return;

        cresize(e->xconfigure.width, e->xconfigure.height);
}

void
run(void)
{
        XEvent ev;
        int w = win.w, h = win.h;
        fd_set rfd;
        int xfd = XConnectionNumber(xw.dpy), ttyfd, xev, drawing;
        struct timespec seltv, *tv, now, lastblink, trigger;
        double timeout;

        /* Waiting for window mapping */
        do {
                XNextEvent(xw.dpy, &ev);
                /*
                 * This XFilterEvent call is required because of XOpenIM. It
                 * does filter out the key event and some client message for
                 * the input method too.
                 */
                if (XFilterEvent(&ev, None))
                        continue;
                if (ev.type == ConfigureNotify) {
                        w = ev.xconfigure.width;
                        h = ev.xconfigure.height;
                }
        } while (ev.type != MapNotify);

        ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);
        cresize(w, h);

        for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
                FD_ZERO(&rfd);
                FD_SET(ttyfd, &rfd);
                FD_SET(xfd, &rfd);

                if (XPending(xw.dpy))
                        timeout = 0;  /* existing events might not set xfd */

                seltv.tv_sec = timeout / 1E3;
                seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
                tv = timeout >= 0 ? &seltv : NULL;

                if (pselect(MAX(xfd, ttyfd)+1, &rfd, NULL, NULL, tv, NULL) < 0) {
                        if (errno == EINTR)
                                continue;
                        die("select failed: %s\n", strerror(errno));
                }
                clock_gettime(CLOCK_MONOTONIC, &now);

                if (FD_ISSET(ttyfd, &rfd))
                        ttyread();

                xev = 0;
                while (XPending(xw.dpy)) {
                        xev = 1;
                        XNextEvent(xw.dpy, &ev);
                        if (XFilterEvent(&ev, None))
                                continue;
                        if (handler[ev.type])
                                (handler[ev.type])(&ev);
                }

                /*
                 * To reduce flicker and tearing, when new content or event
                 * triggers drawing, we first wait a bit to ensure we got
                 * everything, and if nothing new arrives - we draw.
                 * We start with trying to wait minlatency ms. If more content
                 * arrives sooner, we retry with shorter and shorter periods,
                 * and eventually draw even without idle after maxlatency ms.
                 * Typically this results in low latency while interacting,
                 * maximum latency intervals during `cat huge.txt`, and perfect
                 * sync with periodic updates from animations/key-repeats/etc.
                 */
                if (FD_ISSET(ttyfd, &rfd) || xev) {
                        if (!drawing) {
                                trigger = now;
                                drawing = 1;
                        }
                        timeout = (maxlatency - TIMEDIFF(now, trigger)) \
                                  / maxlatency * minlatency;
                        if (timeout > 0)
                                continue;  /* we have time, try to find idle */
                }

                /* idle detected or maxlatency exhausted -> draw */
                timeout = -1;
                if (blinktimeout && tattrset(ATTR_BLINK)) {
                        timeout = blinktimeout - TIMEDIFF(now, lastblink);
                        if (timeout <= 0) {
                                if (-timeout > blinktimeout) /* start visible */
                                        win.mode |= MODE_BLINK;
                                win.mode ^= MODE_BLINK;
                                tsetdirtattr(ATTR_BLINK);
                                lastblink = now;
                                timeout = blinktimeout;
                        }
                }

                draw();
                XFlush(xw.dpy);
                drawing = 0;
        }
}

void
usage(void)
{
        die("usage: %s [-aiv] [-c class] [-f font] [-g geometry]"
                        " [-n name] [-o file]\n"
                        "          [-T title] [-t title] [-w windowid]"
                        " [[-e] command [args ...]]\n"
                        "       %s [-aiv] [-c class] [-f font] [-g geometry]"
                        " [-n name] [-o file]\n"
                        "          [-T title] [-t title] [-w windowid] -l line"
                        " [stty_args ...]\n", argv0, argv0);
}

int
main(int argc, char *argv[])
{
        xw.l = xw.t = 0;
        xw.isfixed = False;
        xsetcursor(cursorshape);

        ARGBEGIN {
                case 'a':
                        allowaltscreen = 0;
                        break;
                case 'c':
                        opt_class = EARGF(usage());
                        break;
                case 'e':
                        if (argc > 0)
                                --argc, ++argv;
                        goto run;
                case 'f':
                        opt_font = EARGF(usage());
                        break;
                case 'g':
                        xw.gm = XParseGeometry(EARGF(usage()),
                                        &xw.l, &xw.t, &cols, &rows);
                        break;
                case 'i':
                        xw.isfixed = 1;
                        break;
                case 'o':
                        opt_io = EARGF(usage());
                        break;
                case 'l':
                        opt_line = EARGF(usage());
                        break;
                case 'n':
                        opt_name = EARGF(usage());
                        break;
                case 't':
                case 'T':
                        opt_title = EARGF(usage());
                        break;
                case 'w':
                        opt_embed = EARGF(usage());
                        break;
                case 'v':
                        die("%s " VERSION "\n", argv0);
                        break;
                default:
                        usage();
        } ARGEND;

run:
        if (argc > 0) /* eat all remaining arguments */
                opt_cmd = argv;

        if (!opt_title)
                opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

        setlocale(LC_CTYPE, "");
        XSetLocaleModifiers("");
        cols = MAX(cols, 1);
        rows = MAX(rows, 1);
        tnew(cols, rows);
        xinit(cols, rows);
        xsetenv();
        selinit();
        run();

        return 0;
}
