/*
 * ilc-uinput — a minimal Input Leap / Barrier *client* for hosts without X11
 * or Wayland, such as the ChromeOS host OS (developer mode, run as root).
 *
 * It speaks the Barrier/Input Leap wire protocol (v1.6, no TLS) and injects
 * the server's keyboard/mouse events through /dev/uinput as virtual devices
 * that ChromeOS's Ozone/evdev layer picks up like any USB device:
 *
 *   - "ilc keyboard"  EV_KEY keyboard
 *   - "ilc mouse"     relative mouse: buttons + wheel (+ motion in --rel mode)
 *   - "ilc tablet"    pen tablet: absolute pointer motion (default mode).
 *                     ChromeOS classifies a bare ABS_X/ABS_Y+BTN_LEFT device
 *                     as a touchscreen, so absolute motion goes through a
 *                     BTN_TOOL_PEN device (INPUT_PROP_POINTER) instead.
 *
 * Build (in Crostini or any Linux box, same arch as the Chromebook):
 *   gcc -O2 -Wall -static -o ilc-uinput ilc-uinput.c
 *
 * Run (ChromeOS host shell, crosh -> shell -> sudo):
 *   ilc-uinput -s 192.168.1.10 -n chromebook
 *
 * The Input Leap server must have "Use SSL encryption" turned OFF.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>

/* ---------------------------------------------------------------- config */

static const char *g_server = NULL;
static int         g_port   = 24800;
static char        g_name[64] = "";
static int         g_width  = 0, g_height = 0;
static int         g_verbose = 0;
static int         g_dryrun  = 0;   /* no uinput; just log */
static int         g_relmode = 0;   /* relative mouse motion instead of tablet */
static volatile sig_atomic_t g_stop = 0;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define DBG(...) do { if (g_verbose) logmsg(__VA_ARGS__); } while (0)

/* --------------------------------------------------------------- uinput */

static int fd_kbd = -1, fd_mouse = -1, fd_tab = -1;
static int cur_x = 0, cur_y = 0;          /* where we believe the cursor is */
static int active = 0;                    /* cursor is on our screen */
static unsigned char key_down[KEY_MAX + 1];
static int btn_down[8];
static int wheel_acc_v = 0, wheel_acc_h = 0;

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev;
    if (fd < 0) return;
    memset(&ev, 0, sizeof ev);
    ev.type = type; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        logmsg("uinput write failed: %s", strerror(errno));
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static int uinput_create(const char *name, void (*setup)(int), int abs_w, int abs_h)
{
    struct uinput_user_dev ud;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { logmsg("open /dev/uinput: %s (are you root?)", strerror(errno)); return -1; }
    setup(fd);
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "%s", name);
    ud.id.bustype = BUS_VIRTUAL; ud.id.vendor = 0x1e0a; ud.id.product = 0x0001; ud.id.version = 1;
    if (abs_w > 0) {
        ud.absmin[ABS_X] = 0; ud.absmax[ABS_X] = abs_w - 1;
        ud.absmin[ABS_Y] = 0; ud.absmax[ABS_Y] = abs_h - 1;
    }
    if (write(fd, &ud, sizeof ud) != (ssize_t)sizeof ud || ioctl(fd, UI_DEV_CREATE) < 0) {
        logmsg("uinput create '%s': %s", name, strerror(errno)); close(fd); return -1;
    }
    return fd;
}

static void setup_kbd(int fd)
{
    int k;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REP);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    /* every key code except the BTN_* ranges, so the device is seen as a keyboard */
    for (k = KEY_ESC; k < BTN_MISC; k++) ioctl(fd, UI_SET_KEYBIT, k);
    for (k = KEY_OK; k < KEY_MAX; k++) ioctl(fd, UI_SET_KEYBIT, k);
}
static void setup_mouse(int fd)
{
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);  ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE); ioctl(fd, UI_SET_KEYBIT, BTN_SIDE);
    ioctl(fd, UI_SET_KEYBIT, BTN_EXTRA);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X); ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL); ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
}
static void setup_tablet(int fd)
{
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X); ioctl(fd, UI_SET_ABSBIT, ABS_Y);
#ifdef UI_SET_PROPBIT
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
#endif
}

static int uinput_init(void)
{
    if (g_dryrun) return 0;
    fd_kbd = uinput_create("ilc keyboard", setup_kbd, 0, 0);
    fd_mouse = uinput_create("ilc mouse", setup_mouse, 0, 0);
    if (!g_relmode) fd_tab = uinput_create("ilc tablet", setup_tablet, g_width, g_height);
    if (fd_kbd < 0 || fd_mouse < 0 || (!g_relmode && fd_tab < 0)) return -1;
    /* give the OS a moment to enumerate the new devices */
    usleep(300 * 1000);
    return 0;
}

static void move_abs(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_width) x = g_width - 1;
    if (y >= g_height) y = g_height - 1;
    if (g_dryrun) { DBG("move -> %d,%d", x, y); cur_x = x; cur_y = y; return; }
    if (g_relmode) {
        emit(fd_mouse, EV_REL, REL_X, x - cur_x);
        emit(fd_mouse, EV_REL, REL_Y, y - cur_y);
        syn(fd_mouse);
    } else {
        emit(fd_tab, EV_ABS, ABS_X, x);
        emit(fd_tab, EV_ABS, ABS_Y, y);
        syn(fd_tab);
    }
    cur_x = x; cur_y = y;
}

static void move_rel(int dx, int dy)
{
    if (g_dryrun) { DBG("relmove %d,%d", dx, dy); return; }
    emit(fd_mouse, EV_REL, REL_X, dx);
    emit(fd_mouse, EV_REL, REL_Y, dy);
    syn(fd_mouse);
    cur_x += dx; cur_y += dy;
}

static void pen_proximity(int in)
{
    if (g_dryrun || g_relmode) return;
    emit(fd_tab, EV_KEY, BTN_TOOL_PEN, in ? 1 : 0);
    syn(fd_tab);
}

static void mouse_button(int id, int down)
{
    static const int map[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };
    if (id < 1 || id > 5) return;
    btn_down[id] = down;
    if (g_dryrun) { DBG("button %d %s", id, down ? "down" : "up"); return; }
    emit(fd_mouse, EV_KEY, map[id], down);
    syn(fd_mouse);
}

static void mouse_wheel(int xdelta, int ydelta)
{
    int v, h;
    wheel_acc_v += ydelta; wheel_acc_h += xdelta;
    v = wheel_acc_v / 120; h = wheel_acc_h / 120;
    wheel_acc_v -= v * 120; wheel_acc_h -= h * 120;
    if (!v && !h) return;
    if (g_dryrun) { DBG("wheel v=%d h=%d", v, h); return; }
    if (v) emit(fd_mouse, EV_REL, REL_WHEEL, v);
    if (h) emit(fd_mouse, EV_REL, REL_HWHEEL, h);
    syn(fd_mouse);
}

static void key_event(int code, int value)   /* value: 0 up, 1 down, 2 repeat */
{
    if (code <= 0 || code > KEY_MAX) return;
    if (value == 1) key_down[code] = 1; else if (value == 0) key_down[code] = 0;
    if (g_dryrun) { DBG("key %d -> %d", code, value); return; }
    emit(fd_kbd, EV_KEY, code, value);
    syn(fd_kbd);
}

static void release_everything(void)
{
    int k;
    for (k = 1; k <= KEY_MAX; k++) if (key_down[k]) key_event(k, 0);
    for (k = 1; k <= 5; k++) if (btn_down[k]) mouse_button(k, 0);
}

/* ----------------------------------------------------------- key mapping */

/* Input Leap KeyIDs (src/lib/inputleap/key_types.h) -> evdev key codes. */
static int keyid_special(uint32_t id)
{
    switch (id) {
    case 0xEF08: return KEY_BACKSPACE;   case 0xEF09: return KEY_TAB;
    case 0xEF0A: return KEY_LINEFEED;    case 0xEF0B: return KEY_CLEAR;
    case 0xEF0D: return KEY_ENTER;       case 0xEF13: return KEY_PAUSE;
    case 0xEF14: return KEY_SCROLLLOCK;  case 0xEF15: return KEY_SYSRQ;
    case 0xEF1B: return KEY_ESC;         case 0xEFFF: return KEY_DELETE;
    case 0xEF50: return KEY_HOME;        case 0xEF51: return KEY_LEFT;
    case 0xEF52: return KEY_UP;          case 0xEF53: return KEY_RIGHT;
    case 0xEF54: return KEY_DOWN;        case 0xEF55: return KEY_PAGEUP;
    case 0xEF56: return KEY_PAGEDOWN;    case 0xEF57: return KEY_END;
    case 0xEF61: return KEY_PRINT;       case 0xEF63: return KEY_INSERT;
    case 0xEF65: return KEY_UNDO;        case 0xEF66: return KEY_REDO;
    case 0xEF67: return KEY_MENU;        case 0xEF68: return KEY_FIND;
    case 0xEF69: return KEY_CANCEL;      case 0xEF6A: return KEY_HELP;
    case 0xEF6B: return KEY_BREAK;       case 0xEF7E: return KEY_RIGHTALT;
    case 0xEF7F: return KEY_NUMLOCK;
    case 0xEF80: return KEY_SPACE;       case 0xEF89: return KEY_TAB;
    case 0xEF8D: return KEY_KPENTER;
    case 0xEF95: return KEY_KP7;         case 0xEF96: return KEY_KP4;
    case 0xEF97: return KEY_KP8;         case 0xEF98: return KEY_KP6;
    case 0xEF99: return KEY_KP2;         case 0xEF9A: return KEY_KP9;
    case 0xEF9B: return KEY_KP3;         case 0xEF9C: return KEY_KP1;
    case 0xEF9D: return KEY_KP5;         case 0xEF9E: return KEY_KP0;
    case 0xEF9F: return KEY_KPDOT;       case 0xEFBD: return KEY_KPEQUAL;
    case 0xEFAA: return KEY_KPASTERISK;  case 0xEFAB: return KEY_KPPLUS;
    case 0xEFAC: return KEY_KPCOMMA;     case 0xEFAD: return KEY_KPMINUS;
    case 0xEFAE: return KEY_KPDOT;       case 0xEFAF: return KEY_KPSLASH;
    case 0xEFB0: return KEY_KP0; case 0xEFB1: return KEY_KP1; case 0xEFB2: return KEY_KP2;
    case 0xEFB3: return KEY_KP3; case 0xEFB4: return KEY_KP4; case 0xEFB5: return KEY_KP5;
    case 0xEFB6: return KEY_KP6; case 0xEFB7: return KEY_KP7; case 0xEFB8: return KEY_KP8;
    case 0xEFB9: return KEY_KP9;
    case 0xEFBE: return KEY_F1;  case 0xEFBF: return KEY_F2;  case 0xEFC0: return KEY_F3;
    case 0xEFC1: return KEY_F4;  case 0xEFC2: return KEY_F5;  case 0xEFC3: return KEY_F6;
    case 0xEFC4: return KEY_F7;  case 0xEFC5: return KEY_F8;  case 0xEFC6: return KEY_F9;
    case 0xEFC7: return KEY_F10; case 0xEFC8: return KEY_F11; case 0xEFC9: return KEY_F12;
    case 0xEFCA: return KEY_F13; case 0xEFCB: return KEY_F14; case 0xEFCC: return KEY_F15;
    case 0xEFCD: return KEY_F16; case 0xEFCE: return KEY_F17; case 0xEFCF: return KEY_F18;
    case 0xEFD0: return KEY_F19; case 0xEFD1: return KEY_F20; case 0xEFD2: return KEY_F21;
    case 0xEFD3: return KEY_F22; case 0xEFD4: return KEY_F23; case 0xEFD5: return KEY_F24;
    case 0xEFE1: return KEY_LEFTSHIFT;   case 0xEFE2: return KEY_RIGHTSHIFT;
    case 0xEFE3: return KEY_LEFTCTRL;    case 0xEFE4: return KEY_RIGHTCTRL;
    case 0xEFE5: return KEY_CAPSLOCK;    case 0xEFE6: return KEY_CAPSLOCK;
    case 0xEFE7: return KEY_LEFTMETA;    case 0xEFE8: return KEY_RIGHTMETA;
    case 0xEFE9: return KEY_LEFTALT;     case 0xEFEA: return KEY_RIGHTALT;
    case 0xEFEB: return KEY_LEFTMETA;    case 0xEFEC: return KEY_RIGHTMETA;
    case 0xEFED: return KEY_LEFTMETA;    case 0xEFEE: return KEY_RIGHTMETA;
    case 0xEF20: return KEY_COMPOSE;     case 0xEE20: return KEY_TAB; /* shift-tab */
    case 0xE001: return KEY_EJECTCD;     case 0xE05F: return KEY_SLEEP;
    case 0xE0A6: return KEY_BACK;        case 0xE0A7: return KEY_FORWARD;
    case 0xE0A8: return KEY_REFRESH;     case 0xE0A9: return KEY_STOP;
    case 0xE0AA: return KEY_SEARCH;      case 0xE0AB: return KEY_BOOKMARKS;
    case 0xE0AC: return KEY_HOMEPAGE;    case 0xE0AD: return KEY_MUTE;
    case 0xE0AE: return KEY_VOLUMEDOWN;  case 0xE0AF: return KEY_VOLUMEUP;
    case 0xE0B0: return KEY_NEXTSONG;    case 0xE0B1: return KEY_PREVIOUSSONG;
    case 0xE0B2: return KEY_STOPCD;      case 0xE0B3: return KEY_PLAYPAUSE;
    default: return 0;
    }
}

/* US-layout character -> evdev code (shift state is whatever the user is holding). */
static int keyid_char(uint32_t c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    switch (c) {
    case 'a': return KEY_A; case 'b': return KEY_B; case 'c': return KEY_C; case 'd': return KEY_D;
    case 'e': return KEY_E; case 'f': return KEY_F; case 'g': return KEY_G; case 'h': return KEY_H;
    case 'i': return KEY_I; case 'j': return KEY_J; case 'k': return KEY_K; case 'l': return KEY_L;
    case 'm': return KEY_M; case 'n': return KEY_N; case 'o': return KEY_O; case 'p': return KEY_P;
    case 'q': return KEY_Q; case 'r': return KEY_R; case 's': return KEY_S; case 't': return KEY_T;
    case 'u': return KEY_U; case 'v': return KEY_V; case 'w': return KEY_W; case 'x': return KEY_X;
    case 'y': return KEY_Y; case 'z': return KEY_Z;
    case '1': case '!': return KEY_1;  case '2': case '@': return KEY_2;
    case '3': case '#': return KEY_3;  case '4': case '$': return KEY_4;
    case '5': case '%': return KEY_5;  case '6': case '^': return KEY_6;
    case '7': case '&': return KEY_7;  case '8': case '*': return KEY_8;
    case '9': case '(': return KEY_9;  case '0': case ')': return KEY_0;
    case '-': case '_': return KEY_MINUS;      case '=': case '+': return KEY_EQUAL;
    case '[': case '{': return KEY_LEFTBRACE;  case ']': case '}': return KEY_RIGHTBRACE;
    case '\\': case '|': return KEY_BACKSLASH; case ';': case ':': return KEY_SEMICOLON;
    case '\'': case '"': return KEY_APOSTROPHE; case '`': case '~': return KEY_GRAVE;
    case ',': case '<': return KEY_COMMA;      case '.': case '>': return KEY_DOT;
    case '/': case '?': return KEY_SLASH;      case ' ': return KEY_SPACE;
    case '\t': return KEY_TAB; case '\r': case '\n': return KEY_ENTER;
    case 0x08: return KEY_BACKSPACE; case 0x1B: return KEY_ESC; case 0x7F: return KEY_DELETE;
    default: return 0;
    }
}

/*
 * Fallback: the server's physical key code ("button"). A Windows server sends
 * the XT set-1 scan code, |0x100 for E0-extended keys. Linux evdev codes for
 * the main block are identical to set-1 scan codes; extended keys need a table.
 */
static int button_to_key(uint32_t b)
{
    if (b == 0) return 0;
    if (!(b & 0x100)) return (b <= 0x58) ? (int)b : 0;
    switch (b & 0xFF) {
    case 0x1C: return KEY_KPENTER;    case 0x1D: return KEY_RIGHTCTRL;
    case 0x35: return KEY_KPSLASH;    case 0x37: return KEY_SYSRQ;
    case 0x38: return KEY_RIGHTALT;   case 0x45: return KEY_PAUSE;
    case 0x46: return KEY_PAUSE;      case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;         case 0x49: return KEY_PAGEUP;
    case 0x4B: return KEY_LEFT;       case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;        case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;   case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;     case 0x5B: return KEY_LEFTMETA;
    case 0x5C: return KEY_RIGHTMETA;  case 0x5D: return KEY_COMPOSE;
    case 0x20: return KEY_MUTE;       case 0x2E: return KEY_VOLUMEDOWN;
    case 0x30: return KEY_VOLUMEUP;   case 0x19: return KEY_NEXTSONG;
    case 0x10: return KEY_PREVIOUSSONG; case 0x22: return KEY_PLAYPAUSE;
    case 0x24: return KEY_STOPCD;     case 0x6A: return KEY_BACK;
    case 0x69: return KEY_FORWARD;    case 0x5F: return KEY_SLEEP;
    case 0x32: return KEY_HOMEPAGE;   case 0x65: return KEY_SEARCH;
    case 0x66: return KEY_BOOKMARKS;  case 0x67: return KEY_REFRESH;
    case 0x68: return KEY_STOP;       case 0x6C: return KEY_MAIL;
    case 0x6D: return KEY_MEDIA;      case 0x6B: return KEY_COMPUTER;
    case 0x21: return KEY_CALC;
    default: return 0;
    }
}

/* Remember which evdev code a (KeyID,button) pair went down as, so the matching
 * key-up releases the same code even if the server reports it differently. */
static int down_code_by_button[0x200];

static int resolve_key(uint32_t id, uint32_t button, int down)
{
    int code = 0;
    if (!down && button < 0x200 && down_code_by_button[button])
        code = down_code_by_button[button];
    if (!code) code = keyid_special(id);
    if (!code) code = keyid_char(id);
    if (!code) code = button_to_key(button);
    if (button < 0x200) down_code_by_button[button] = down ? code : 0;
    return code;
}

/* ----------------------------------------------------------------- wire */

static int sock = -1;

static int send_all(const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = send(sock, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        p += w; n -= w;
    }
    return 0;
}
static int recv_all(void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = recv(sock, p, n, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) { if (g_stop) return -1; continue; } return -1; }
        p += r; n -= r;
    }
    return 0;
}

/* messages are 4-byte big-endian length + payload */
static int send_msg(const void *payload, uint32_t len)
{
    uint32_t be = htonl(len);
    if (send_all(&be, 4)) return -1;
    return send_all(payload, len);
}

static uint8_t *put16(uint8_t *p, uint32_t v) { *p++ = v >> 8; *p++ = v; return p; }
static uint8_t *put32(uint8_t *p, uint32_t v) { *p++ = v >> 24; *p++ = v >> 16; *p++ = v >> 8; *p++ = v; return p; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int send_hello_back(void)
{
    uint8_t buf[128], *p = buf;
    size_t nl = strlen(g_name);
    memcpy(p, "Barrier", 7); p += 7;
    p = put16(p, 1); p = put16(p, 6);          /* protocol 1.6 */
    p = put32(p, (uint32_t)nl); memcpy(p, g_name, nl); p += nl;
    return send_msg(buf, (uint32_t)(p - buf));
}

static int send_info(void)
{
    uint8_t buf[32], *p = buf;
    memcpy(p, "DINF", 4); p += 4;
    p = put16(p, 0); p = put16(p, 0);                     /* x, y */
    p = put16(p, g_width); p = put16(p, g_height);        /* w, h */
    p = put16(p, 0);                                      /* warp size (unused) */
    p = put16(p, cur_x); p = put16(p, cur_y);             /* mouse pos */
    DBG("-> DINF %dx%d", g_width, g_height);
    return send_msg(buf, (uint32_t)(p - buf));
}

static int handle(const uint8_t *m, uint32_t len)
{
    if (len < 4) return 0;
#define IS(code) (memcmp(m, code, 4) == 0)
    if (IS("CALV")) { return send_msg("CALV", 4); }
    if (IS("DMMV")) { if (len >= 8) move_abs((int16_t)get16(m + 4), (int16_t)get16(m + 6)); return 0; }
    if (IS("DMRM")) { if (len >= 8) move_rel((int16_t)get16(m + 4), (int16_t)get16(m + 6)); return 0; }
    if (IS("DMDN")) { if (len >= 5) mouse_button(m[4], 1); return 0; }
    if (IS("DMUP")) { if (len >= 5) mouse_button(m[4], 0); return 0; }
    if (IS("DMWM")) { if (len >= 8) mouse_wheel((int16_t)get16(m + 4), (int16_t)get16(m + 6)); return 0; }
    if (IS("DKDN")) {
        if (len >= 10) {
            uint32_t id = get16(m + 4), button = get16(m + 8);
            int code = resolve_key(id, button, 1);
            DBG("keydown id=0x%04x button=0x%03x -> %d", id, button, code);
            if (code) key_event(code, 1); else logmsg("unmapped key id=0x%04x button=0x%03x", id, button);
        }
        return 0;
    }
    if (IS("DKUP")) {
        if (len >= 10) {
            uint32_t id = get16(m + 4), button = get16(m + 8);
            int code = resolve_key(id, button, 0);
            if (code) key_event(code, 0);
        }
        return 0;
    }
    if (IS("DKRP")) {
        if (len >= 12) {
            uint32_t id = get16(m + 4), n = get16(m + 8), button = get16(m + 10);
            int code = resolve_key(id, button, 1), i;
            if (code) for (i = 0; i < (int)n; i++) key_event(code, 2);
        }
        return 0;
    }
    if (IS("CINN")) {
        if (len >= 12) {
            int x = (int16_t)get16(m + 4), y = (int16_t)get16(m + 6);
            DBG("enter at %d,%d", x, y);
            active = 1;
            pen_proximity(1);
            if (g_relmode && !g_dryrun) {
                /* re-sync: slam the cursor into the top-left corner, then walk to (x,y) */
                emit(fd_mouse, EV_REL, REL_X, -32000); emit(fd_mouse, EV_REL, REL_Y, -32000); syn(fd_mouse);
                cur_x = 0; cur_y = 0;
            }
            move_abs(x, y);
        }
        return 0;
    }
    if (IS("COUT")) { DBG("leave"); active = 0; release_everything(); pen_proximity(0); return 0; }
    if (IS("QINF")) { return send_info(); }
    if (IS("CIAK")) { DBG("info acknowledged"); return 0; }
    if (IS("CROP")) { DBG("reset options"); return 0; }
    if (IS("DSOP")) { DBG("set options (%u bytes)", len - 4); return 0; }
    if (IS("CCLP") || IS("DCLP") || IS("CSEC") || IS("CNOP") || IS("DFTR") || IS("DDRG")) return 0;
    if (IS("CBYE")) { logmsg("server closed the connection"); return -1; }
    if (IS("EBSY")) { logmsg("server says: busy (another client with the name '%s' is connected)", g_name); return -1; }
    if (IS("EUNK")) { logmsg("server says: unknown screen '%s' - add it in the server's screen layout", g_name); return -1; }
    if (IS("EBAD")) { logmsg("server says: bad protocol"); return -1; }
    if (IS("EICV")) { logmsg("server says: incompatible version %u.%u", get16(m + 4), get16(m + 6)); return -1; }
    DBG("unhandled message %.4s (%u bytes)", m, len);
    return 0;
#undef IS
}

static int connect_server(void)
{
    struct addrinfo hints, *res, *ai;
    char port[16];
    int one = 1;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", g_port);
    /* numeric first: a static binary may not be able to load the host's NSS modules */
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(g_server, port, &hints, &res) != 0) {
        hints.ai_flags = 0;
        if (getaddrinfo(g_server, port, &hints, &res) != 0) { logmsg("cannot resolve %s", g_server); return -1; }
    }
    for (ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(sock); sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) { logmsg("connect to %s:%d failed: %s", g_server, g_port, strerror(errno)); return -1; }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return 0;
}

static int session(void)
{
    uint8_t *buf;
    uint32_t len;
    uint8_t hello[16];

    if (connect_server()) return -1;
    logmsg("connected to %s:%d, waiting for hello", g_server, g_port);

    /* server hello: "Barrier" + major + minor (length-prefixed) */
    if (recv_all(&len, 4)) { logmsg("no hello (is SSL disabled on the server?)"); goto fail; }
    len = ntohl(len);
    if (len != 11) { logmsg("unexpected hello length %u (is SSL disabled on the server?)", len); goto fail; }
    if (recv_all(hello, 11)) goto fail;
    if (memcmp(hello, "Barrier", 7) && memcmp(hello, "Synergy", 7)) { logmsg("not a Barrier/Input Leap server"); goto fail; }
    logmsg("server protocol %u.%u", get16(hello + 7), get16(hello + 9));
    if (send_hello_back()) goto fail;
    if (send_info()) goto fail;
    logmsg("handshake done; screen '%s' %dx%d, %s mode", g_name, g_width, g_height, g_relmode ? "relative-mouse" : "tablet");

    buf = malloc(1 << 20);
    while (!g_stop) {
        if (recv_all(&len, 4)) break;
        len = ntohl(len);
        if (len == 0) continue;
        if (len > (1u << 20)) { logmsg("oversized message (%u bytes)", len); break; }
        if (recv_all(buf, len)) break;
        if (handle(buf, len) < 0) break;
    }
    free(buf);
fail:
    release_everything();
    pen_proximity(0);
    active = 0;
    close(sock); sock = -1;
    return -1;
}

/* ---------------------------------------------------------------- setup */

static void detect_screen(void)
{
    glob_t g;
    size_t i;
    if (g_width && g_height) return;
    if (glob("/sys/class/drm/card*-*/status", 0, NULL, &g) == 0) {
        for (i = 0; i < g.gl_pathc && !g_width; i++) {
            char path[256], line[64];
            FILE *f = fopen(g.gl_pathv[i], "r");
            if (!f) continue;
            if (fgets(line, sizeof line, f) && !strncmp(line, "connected", 9)) {
                fclose(f);
                snprintf(path, sizeof path, "%.*s/modes", (int)(strlen(g.gl_pathv[i]) - 7), g.gl_pathv[i]);
                f = fopen(path, "r");
                if (f && fgets(line, sizeof line, f)) {
                    if (sscanf(line, "%dx%d", &g_width, &g_height) == 2)
                        logmsg("screen size %dx%d from %s", g_width, g_height, path);
                }
            }
            if (f) fclose(f);
        }
        globfree(&g);
    }
    if (!g_width || !g_height) { g_width = 1920; g_height = 1080; logmsg("screen size unknown, assuming %dx%d (use -w)", g_width, g_height); }
}

static void on_signal(int s) { (void)s; g_stop = 1; if (sock >= 0) shutdown(sock, SHUT_RDWR); }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s -s server[:port] [-n name] [-w WxH] [--rel] [-v] [--dry-run]\n"
        "  -s   Input Leap server address (SSL must be disabled on the server)\n"
        "  -n   screen name (default: hostname)\n"
        "  -w   screen size, e.g. 1920x1080 (default: from /sys/class/drm)\n"
        "  --rel  move the cursor with a relative mouse instead of a pen tablet\n"
        "  -v   verbose; --dry-run: don't open /dev/uinput, just log\n", argv0);
    exit(2);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            char *c;
            g_server = argv[++i];
            c = strrchr(g_server, ':');
            if (c && !strchr(c, ']')) { *c = 0; g_port = atoi(c + 1); }
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) snprintf(g_name, sizeof g_name, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &g_width, &g_height) != 2) usage(argv[0]); }
        else if (!strcmp(argv[i], "--rel")) g_relmode = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--dry-run")) g_dryrun = 1;
        else usage(argv[0]);
    }
    if (!g_server) usage(argv[0]);
    if (!g_name[0]) { gethostname(g_name, sizeof g_name - 1); }
    detect_screen();

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (uinput_init()) return 1;
    logmsg("virtual input devices created (%s)", g_relmode ? "keyboard + mouse" : "keyboard + mouse + tablet");

    while (!g_stop) {
        session();
        if (g_stop) break;
        logmsg("reconnecting in 3s...");
        for (i = 0; i < 30 && !g_stop; i++) usleep(100 * 1000);
    }
    release_everything();
    if (fd_tab >= 0) { ioctl(fd_tab, UI_DEV_DESTROY); close(fd_tab); }
    if (fd_mouse >= 0) { ioctl(fd_mouse, UI_DEV_DESTROY); close(fd_mouse); }
    if (fd_kbd >= 0) { ioctl(fd_kbd, UI_DEV_DESTROY); close(fd_kbd); }
    logmsg("bye");
    return 0;
}
