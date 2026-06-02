#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

struct geometry {
    int x;
    int y;
    unsigned int w;
    unsigned int h;
};

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: youtube-stream-outline --geometry WxH+X+Y [--parent-pid PID] [--color #a855f7] [--thickness PX]\n"
            "\n"
            "Draws a click-through purple X11 outline around the captured monitor until\n"
            "interrupted or until the optional parent PID exits.\n");
}

static int parse_uint_component(const char **p, unsigned int *out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(*p, &end, 10);
    if (errno != 0 || end == *p || value == 0 || value > 65535)
        return -1;
    *out = (unsigned int)value;
    *p = end;
    return 0;
}

static int parse_int_component(const char **p, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(*p, &end, 10);
    if (errno != 0 || end == *p || value < -65535 || value > 65535)
        return -1;
    *out = (int)value;
    *p = end;
    return 0;
}

static int parse_geometry(const char *text, struct geometry *geo)
{
    const char *p = text;

    if (parse_uint_component(&p, &geo->w) < 0)
        return -1;
    if (*p++ != 'x')
        return -1;
    if (parse_uint_component(&p, &geo->h) < 0)
        return -1;
    if (*p != '+' && *p != '-')
        return -1;
    if (parse_int_component(&p, &geo->x) < 0)
        return -1;
    if (*p != '+' && *p != '-')
        return -1;
    if (parse_int_component(&p, &geo->y) < 0)
        return -1;
    return *p == '\0' ? 0 : -1;
}

static int parse_positive_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 128)
        return -1;
    *out = (int)value;
    return 0;
}

static unsigned long alloc_indicator_color(Display *dpy, int screen, const char *name)
{
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor color;
    XColor exact;

    if (name && *name && XAllocNamedColor(dpy, cmap, name, &color, &exact))
        return color.pixel;
    if (XAllocNamedColor(dpy, cmap, "#a855f7", &color, &exact))
        return color.pixel;
    if (XAllocNamedColor(dpy, cmap, "purple", &color, &exact))
        return color.pixel;
    return WhitePixel(dpy, screen);
}

static void make_window_clickthrough(Display *dpy, Window root, Window win, unsigned int width, unsigned int height)
{
    int shape_event_base;
    int shape_error_base;

    if (!XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base))
        return;
#ifdef ShapeInput
    Pixmap input_mask = XCreatePixmap(dpy, root, width, height, 1);
    if (!input_mask)
        return;
    GC gc = XCreateGC(dpy, input_mask, 0, NULL);
    if (!gc) {
        XFreePixmap(dpy, input_mask);
        return;
    }
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, input_mask, gc, 0, 0, width, height);
    XFreeGC(dpy, gc);
    XShapeCombineMask(dpy, win, ShapeInput, 0, 0, input_mask, ShapeSet);
    XFreePixmap(dpy, input_mask);
#else
    (void)root;
    (void)width;
    (void)height;
#endif
}

static Window create_overlay_window(Display *dpy, Window root, int x, int y,
                                    unsigned int width, unsigned int height,
                                    unsigned long pixel)
{
    XSetWindowAttributes attrs;
    Window win;

    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.border_pixel = 0;
    attrs.background_pixel = pixel;
    attrs.event_mask = ExposureMask;

    win = XCreateWindow(dpy, root, x, y, width, height, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBorderPixel | CWBackPixel | CWEventMask,
                        &attrs);
    if (!win)
        return 0;

    XStoreName(dpy, win, "youtube-stream outline");
    make_window_clickthrough(dpy, root, win, width, height);
    XMapRaised(dpy, win);
    return win;
}

static void position_border_windows(Display *dpy, Window border[4], const struct geometry *geo, int thickness)
{
    unsigned int t = (unsigned int)thickness;
    unsigned int width = geo->w > t ? geo->w : t;
    unsigned int height = geo->h > t ? geo->h : t;
    int left_x = geo->x;
    int right_x = geo->x + (int)width - thickness;
    int top_y = geo->y;
    int bottom_y = geo->y + (int)height - thickness;

    XMoveResizeWindow(dpy, border[0], left_x, top_y, width, t);
    XMoveResizeWindow(dpy, border[1], left_x, bottom_y, width, t);
    XMoveResizeWindow(dpy, border[2], left_x, top_y, t, height);
    XMoveResizeWindow(dpy, border[3], right_x, top_y, t, height);
    for (int i = 0; i < 4; ++i)
        XMapRaised(dpy, border[i]);
}

int main(int argc, char **argv)
{
    const char *geometry_text = NULL;
    const char *color_name = "#a855f7";
    pid_t parent_pid = -1;
    int thickness = 2;
    struct geometry geo;
    Display *dpy;
    int screen;
    Window root;
    Window border[4] = {0, 0, 0, 0};
    unsigned long pixel;
    int exit_status = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--geometry") && i + 1 < argc) {
            geometry_text = argv[++i];
        } else if (!strcmp(argv[i], "--parent-pid") && i + 1 < argc) {
            char *end = NULL;
            long value;
            errno = 0;
            value = strtol(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value <= 1) {
                usage(stderr);
                return 2;
            }
            parent_pid = (pid_t)value;
        } else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
            color_name = argv[++i];
        } else if (!strcmp(argv[i], "--thickness") && i + 1 < argc) {
            if (parse_positive_int(argv[++i], &thickness) < 0) {
                usage(stderr);
                return 2;
            }
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (!geometry_text || parse_geometry(geometry_text, &geo) < 0) {
        usage(stderr);
        return 2;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "youtube-stream-outline: cannot open X display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    pixel = alloc_indicator_color(dpy, screen, color_name);

    for (int i = 0; i < 4; ++i) {
        border[i] = create_overlay_window(dpy, root, 0, 0, 2, 2, pixel);
        if (!border[i]) {
            fprintf(stderr, "youtube-stream-outline: failed to create outline window\n");
            running = 0;
            exit_status = 1;
            break;
        }
    }

    if (running) {
        position_border_windows(dpy, border, &geo, thickness);
        XFlush(dpy);
    }

    while (running) {
        if (parent_pid > 1 && kill(parent_pid, 0) < 0 && errno == ESRCH) {
            running = 0;
            break;
        }
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                XClearWindow(dpy, ev.xexpose.window);
                position_border_windows(dpy, border, &geo, thickness);
            }
        }
        usleep(50000);
    }

    for (int i = 0; i < 4; ++i) {
        if (border[i])
            XDestroyWindow(dpy, border[i]);
    }
    XCloseDisplay(dpy);
    return exit_status;
}
