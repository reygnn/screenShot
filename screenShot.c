/*
 * screenShot - takes a screenshot of the screen or a rectangle, into a file or the
 * clipboard. No window of its own. For mwc (https://github.com/reygnn/mwc) and any
 * other Wayland compositor with the same protocols.
 *
 *   screenShot          Screenshots/Screenshot_<date>_<time>.png in the pictures
 *                       folder, the path goes to stdout
 *   screenShot -c       the clipboard, as image/png
 *   screenShot -f FILE  that file, "-" for stdout
 *   screenShot -o NAME  only that monitor, -l lists them
 *   screenShot -r       a rectangle, drawn with the mouse on a still of the screen
 *
 * Afterwards a toast shows for three seconds what became of it, -q leaves it out.
 *
 * The screen comes through wlr-screencopy, the clipboard through ext-data-control-v1,
 * the still for -r is a wlr-layer-shell overlay.
 * Without WAYLAND_DISPLAY, as from an ssh login or a session started elsewhere, it looks
 * for a compositor in the runtime directory by itself.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "ext-data-control-v1-client-protocol.h"
#include "version.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#define MAX_OUTPUTS 16

struct output {
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	uint32_t global;
	char name[64];
	/* From wl_output, for a compositor without xdg-output. */
	int32_t geo_x, geo_y, transform, scale;
	int32_t mode_w, mode_h; /* the current mode, in buffer pixels, for -l */
	/* Logical, in the layout: from xdg-output if there is one. */
	int32_t x, y, w, h;
	bool have_logical;

	/* The capture. */
	struct zwlr_screencopy_frame_v1 *frame;
	uint32_t format, width, height, stride;
	bool have_format;
	bool y_invert;
	bool done, failed;
	struct wl_buffer *buffer;
	void *data;
	size_t size;
};

static struct wl_display *display;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_screencopy_manager_v1 *screencopy;
static struct zxdg_output_manager_v1 *xdg_output_manager;
static struct ext_data_control_manager_v1 *data_control;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wp_viewporter *viewporter;
static uint32_t seat_caps;
static struct output outputs[MAX_OUTPUTS];
static int n_outputs;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("screenShot: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static void usage(FILE *f)
{
	fputs("usage: screenShot [-c | -f FILE] [-r] [-o NAME] [-d SECONDS] [-p] [-q]\n"
		"       screenShot -l | -v\n"
		"  (none)     to Screenshots/ in the pictures folder, prints the path\n"
		"  -c         to the clipboard, as image/png\n"
		"  -f FILE    to FILE, - for stdout\n"
		"  -r         a rectangle: drag it with the mouse, a click takes the\n"
		"             monitor, Esc or another button gives up\n"
		"  -o NAME    only that monitor\n"
		"  -d SECONDS wait that long first\n"
		"  -p         with the mouse pointer\n"
		"  -q         no toast afterwards\n"
		"  -l         list the monitors\n"
		"  -v         print the version\n", f);
}

/* ---- outputs ------------------------------------------------------------------------ */

static void wl_output_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
	int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model,
	int32_t transform)
{
	struct output *o = data;
	o->geo_x = x;
	o->geo_y = y;
	o->transform = transform;
}

static void wl_output_mode(void *data, struct wl_output *wl, uint32_t flags,
	int32_t w, int32_t h, int32_t refresh)
{
	struct output *o = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		o->mode_w = w;
		o->mode_h = h;
	}
}

static void wl_output_done(void *data, struct wl_output *wl)
{
}

static void wl_output_scale(void *data, struct wl_output *wl, int32_t factor)
{
	struct output *o = data;
	o->scale = factor;
}

static void wl_output_name(void *data, struct wl_output *wl, const char *name)
{
	struct output *o = data;
	snprintf(o->name, sizeof(o->name), "%s", name);
}

static void wl_output_description(void *data, struct wl_output *wl, const char *desc)
{
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_geometry,
	.mode = wl_output_mode,
	.done = wl_output_done,
	.scale = wl_output_scale,
	.name = wl_output_name,
	.description = wl_output_description,
};

static void xdg_output_position(void *data, struct zxdg_output_v1 *xdg, int32_t x,
	int32_t y)
{
	struct output *o = data;
	o->x = x;
	o->y = y;
	o->have_logical = true;
}

static void xdg_output_size(void *data, struct zxdg_output_v1 *xdg, int32_t w, int32_t h)
{
	struct output *o = data;
	o->w = w;
	o->h = h;
}

static void xdg_output_done(void *data, struct zxdg_output_v1 *xdg)
{
}

static void xdg_output_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
	struct output *o = data;
	if (!o->name[0])
		snprintf(o->name, sizeof(o->name), "%s", name);
}

static void xdg_output_description(void *data, struct zxdg_output_v1 *xdg,
	const char *desc)
{
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_position,
	.logical_size = xdg_output_size,
	.done = xdg_output_done,
	.name = xdg_output_name,
	.description = xdg_output_description,
};

/* ---- the registry ------------------------------------------------------------------- */

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static void seat_capabilities(void *data, struct wl_seat *wl, uint32_t caps)
{
	seat_caps = caps;
}

static void seat_name(void *data, struct wl_seat *wl, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		if (!seat) {
			seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
			wl_seat_add_listener(seat, &seat_listener, NULL);
		}
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (n_outputs == MAX_OUTPUTS)
			return;
		struct output *o = &outputs[n_outputs++];
		memset(o, 0, sizeof(*o));
		o->global = name;
		o->scale = 1;
		o->wl = wl_registry_bind(registry, name, &wl_output_interface,
			min_u32(version, 4));
		wl_output_add_listener(o->wl, &wl_output_listener, o);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		xdg_output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, min_u32(version, 3));
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		if (version >= 3)
			screencopy = wl_registry_bind(registry, name,
				&zwlr_screencopy_manager_v1_interface, 3);
	} else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
		data_control = wl_registry_bind(registry, name,
			&ext_data_control_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
			min_u32(version, 6));
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
			min_u32(version, 4));
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void forget_globals(void)
{
	shm = NULL;
	seat = NULL;
	screencopy = NULL;
	xdg_output_manager = NULL;
	data_control = NULL;
	compositor = NULL;
	layer_shell = NULL;
	viewporter = NULL;
	seat_caps = 0;
	n_outputs = 0;
}

/* Connects and reads the globals. Returns false for a compositor without screencopy. */
static bool try_display(const char *name)
{
	display = wl_display_connect(name);
	if (!display)
		return false;
	forget_globals();
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 || !screencopy) {
		wl_display_disconnect(display);
		display = NULL;
		return false;
	}
	return true;
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(a, b);
}

static bool is_socket(const char *dir, const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

/*
 * WAYLAND_DISPLAY goes first. Without it, as in an ssh login, a tty or a program
 * started outside the session, the first wayland-N socket in the runtime directory whose
 * compositor offers screencopy.
 */
static void connect_display(void)
{
	const char *env = getenv("WAYLAND_DISPLAY");
	if (env && *env) {
		if (try_display(NULL))
			return;
		if (!display && !(display = wl_display_connect(NULL)))
			die("cannot connect to %s: %s", env, strerror(errno));
		die("the compositor offers no wlr-screencopy (version 3)");
	}

	char fallback[64];
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !*dir) {
		snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)getuid());
		setenv("XDG_RUNTIME_DIR", fallback, 1);
		dir = fallback;
	}
	DIR *d = opendir(dir);
	if (!d)
		die("WAYLAND_DISPLAY is not set and %s cannot be read: %s", dir,
			strerror(errno));
	/* In place, not strdup'ed: nothing here can run out of memory. */
	char names[64][sizeof(((struct dirent *)0)->d_name)];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 64) {
		/* Sockets only: that leaves out wayland-0.lock and the like. */
		if (strncmp(e->d_name, "wayland-", 8) != 0 || !is_socket(dir, e->d_name))
			continue;
		snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(names[0]), compare_names);
	bool found = false;
	for (int i = 0; i < n && !found; i++) {
		if (try_display(names[i])) {
			found = true;
			/* For the clipboard child and anything it might start. */
			setenv("WAYLAND_DISPLAY", names[i], 1);
		}
	}
	if (!found)
		die("WAYLAND_DISPLAY is not set, and no compositor with wlr-screencopy "
			"listens in %s", dir);
}

/* Reads the outputs' names and places. */
static void read_outputs(void)
{
	if (xdg_output_manager) {
		for (int i = 0; i < n_outputs; i++) {
			struct output *o = &outputs[i];
			o->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager,
				o->wl);
			zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o);
		}
	}
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	for (int i = 0; i < n_outputs; i++)
		if (!outputs[i].name[0])
			snprintf(outputs[i].name, sizeof(outputs[i].name), "output-%d", i);
}

/* ---- capture ------------------------------------------------------------------------ */

/* The formats that cairo can take as they are, or with red and blue swapped. */
static bool format_ok(uint32_t f)
{
	return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 ||
		f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888;
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
	uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
	struct output *o = data;
	/* The first usable one, but the xrgb/argb ones need no swapping. */
	if (!format_ok(format))
		return;
	if (o->have_format && (o->format == WL_SHM_FORMAT_XRGB8888 ||
			o->format == WL_SHM_FORMAT_ARGB8888))
		return;
	o->format = format;
	o->width = width;
	o->height = height;
	o->stride = stride;
	o->have_format = true;
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
	uint32_t format, uint32_t width, uint32_t height)
{
}

static int shm_file(size_t size)
{
	int fd = memfd_create("screenShot", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create: %s", strerror(errno));
	if (ftruncate(fd, size) < 0)
		die("ftruncate: %s", strerror(errno));
	return fd;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct output *o = data;
	if (!o->have_format) {
		fprintf(stderr, "screenShot: %s: the compositor offers no shm format "
			"screenShot can read\n", o->name);
		o->failed = true;
		return;
	}
	o->size = (size_t)o->stride * o->height;
	int fd = shm_file(o->size);
	o->data = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (o->data == MAP_FAILED)
		die("mmap: %s", strerror(errno));
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, o->size);
	o->buffer = wl_shm_pool_create_buffer(pool, 0, o->width, o->height, o->stride,
		o->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	zwlr_screencopy_frame_v1_copy(frame, o->buffer);
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
	uint32_t flags)
{
	struct output *o = data;
	o->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t x,
	uint32_t y, uint32_t w, uint32_t h)
{
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
	uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
	struct output *o = data;
	o->done = true;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct output *o = data;
	fprintf(stderr, "screenShot: %s: the compositor could not copy the screen\n", o->name);
	o->failed = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

static void capture(struct output *o, bool pointer)
{
	o->frame = zwlr_screencopy_manager_v1_capture_output(screencopy, pointer, o->wl);
	zwlr_screencopy_frame_v1_add_listener(o->frame, &frame_listener, o);
}

/* Once the picture is made: the clipboard process may stay for hours, with only the PNG. */
static void release_capture(struct output *o)
{
	if (o->frame)
		zwlr_screencopy_frame_v1_destroy(o->frame);
	if (o->buffer)
		wl_buffer_destroy(o->buffer);
	if (o->data)
		munmap(o->data, o->size);
	o->frame = NULL;
	o->buffer = NULL;
	o->data = NULL;
}

/* ---- the picture -------------------------------------------------------------------- */

/* The buffer's size once the output's transform is applied, as on the monitor. */
static void transformed_size(const struct output *o, int *w, int *h)
{
	if (o->transform & 1) {
		*w = o->height;
		*h = o->width;
	} else {
		*w = o->width;
		*h = o->height;
	}
}

/*
 * The matrix from a point on the monitor, in buffer pixels, to the buffer: the
 * compositor applied the output's transform when it drew, this undoes it. w and h are
 * the buffer's size.
 */
static void buffer_matrix(const struct output *o, cairo_matrix_t *m)
{
	double w = o->width, h = o->height;
	switch (o->transform) {
	case WL_OUTPUT_TRANSFORM_90:
		cairo_matrix_init(m, 0, -1, 1, 0, 0, h);
		break;
	case WL_OUTPUT_TRANSFORM_180:
		cairo_matrix_init(m, -1, 0, 0, -1, w, h);
		break;
	case WL_OUTPUT_TRANSFORM_270:
		cairo_matrix_init(m, 0, 1, -1, 0, w, 0);
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		cairo_matrix_init(m, -1, 0, 0, 1, w, 0);
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		cairo_matrix_init(m, 0, 1, 1, 0, 0, 0);
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		cairo_matrix_init(m, 1, 0, 0, -1, 0, h);
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		cairo_matrix_init(m, 0, -1, -1, 0, w, h);
		break;
	default:
		cairo_matrix_init_identity(m);
		break;
	}
	if (o->y_invert) {
		cairo_matrix_t flip;
		cairo_matrix_init(&flip, 1, 0, 0, -1, 0, h);
		cairo_matrix_multiply(m, m, &flip);
	}
}

/* Where the output lies in the layout, in logical pixels. */
static void logical_box(const struct output *o, int *x, int *y, int *w, int *h)
{
	int tw, th;
	transformed_size(o, &tw, &th);
	if (o->have_logical && o->w > 0 && o->h > 0) {
		*x = o->x;
		*y = o->y;
		*w = o->w;
		*h = o->h;
	} else {
		*x = o->geo_x;
		*y = o->geo_y;
		*w = tw / (o->scale > 0 ? o->scale : 1);
		*h = th / (o->scale > 0 ? o->scale : 1);
	}
}

/*
 * Its buffer pixels per logical pixel, one for each axis: the compositor cuts the
 * logical width and height off separately, so 1280x720 at 1.3 is 984x553, and 553 *
 * 1280 / 984 is not 720.
 */
static void output_scale(const struct output *o, double *sx, double *sy)
{
	int x, y, w, h, tw, th;
	logical_box(o, &x, &y, &w, &h);
	transformed_size(o, &tw, &th);
	*sx = w > 0 ? (double)tw / w : 1;
	*sy = h > 0 ? (double)th / h : 1;
}

/* The pixel a point in buffer pixels is on. 702 * (720 / 702.0) lands just below 720. */
static int pixel_at(double v)
{
	return (int)floor(v + 1e-6);
}

/* Once per capture: compose() may take an output more than once. */
static void swap_red_blue(struct output *o)
{
	if (o->format != WL_SHM_FORMAT_XBGR8888 && o->format != WL_SHM_FORMAT_ABGR8888)
		return;
	for (uint32_t y = 0; y < o->height; y++) {
		uint32_t *row = (uint32_t *)((char *)o->data + (size_t)y * o->stride);
		for (uint32_t x = 0; x < o->width; x++) {
			uint32_t p = row[x];
			row[x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
		}
	}
}

/*
 * All the captured outputs in one picture, as they lie in the layout. A picture of
 * monitors with different scales gets the largest one, so none loses detail. What no
 * monitor covers stays black. Where it begins in the layout and its pixels per logical
 * pixel go to *origin_x, *origin_y, *scale_x and *scale_y.
 */
/*
 * Where an output goes in the picture, in its pixels, and by how much it is scaled. One
 * off by less than a pixel over its whole size is copied as it is: the logical sizes are
 * whole numbers, so 1280x720 and 1920x1080 at 1.3 get slightly different scales.
 */
static void place(const struct output *o, int x0, int y0, double scx, double scy,
	double *px, double *py, double *rx, double *ry)
{
	int x, y, w, h, tw, th;
	logical_box(o, &x, &y, &w, &h);
	transformed_size(o, &tw, &th);
	double sx, sy;
	output_scale(o, &sx, &sy);
	*rx = scx / sx;
	*ry = scy / sy;
	*px = (x - x0) * scx;
	*py = (y - y0) * scy;
	if (fabs(*rx - 1) * tw < 1 && fabs(*ry - 1) * th < 1) {
		*rx = *ry = 1;
		*px = round(*px);
		*py = round(*py);
	}
}

static cairo_surface_t *compose(struct output **list, int n, int *origin_x, int *origin_y,
	double *scale_x, double *scale_y)
{
	int x0 = INT_MAX, y0 = INT_MAX;
	double scx = 0, scy = 0;
	for (int i = 0; i < n; i++) {
		int x, y, w, h;
		logical_box(list[i], &x, &y, &w, &h);
		if (x < x0) x0 = x;
		if (y < y0) y0 = y;
		double sx, sy;
		output_scale(list[i], &sx, &sy);
		scx = fmax(scx, sx);
		scy = fmax(scy, sy);
	}
	int width = 0, height = 0;
	for (int i = 0; i < n; i++) {
		int tw, th;
		double px, py, rx, ry;
		transformed_size(list[i], &tw, &th);
		place(list[i], x0, y0, scx, scy, &px, &py, &rx, &ry);
		width = fmax(width, (int)(px + tw * rx + 0.5));
		height = fmax(height, (int)(py + th * ry + 0.5));
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS)
		die("cannot make a %dx%d picture", width, height);
	cairo_t *cr = cairo_create(out);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	for (int i = 0; i < n; i++) {
		struct output *o = list[i];
		/* Alpha on a screen means nothing: x or a, it is opaque. */
		cairo_surface_t *src = cairo_image_surface_create_for_data(o->data,
			CAIRO_FORMAT_RGB24, o->width, o->height, o->stride);
		int tw, th;
		transformed_size(o, &tw, &th);
		double px, py, sx, sy;
		place(o, x0, y0, scx, scy, &px, &py, &sx, &sy);

		cairo_save(cr);
		cairo_translate(cr, px, py);
		cairo_scale(cr, sx, sy);
		cairo_pattern_t *p = cairo_pattern_create_for_surface(src);
		cairo_matrix_t m;
		buffer_matrix(o, &m);
		cairo_pattern_set_matrix(p, &m);
		bool exact = sx == 1 && sy == 1;
		cairo_pattern_set_filter(p, exact ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
		/* Scaled up, the edge pixels would blend with what lies outside: black. */
		cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
		cairo_set_source(cr, p);
		cairo_rectangle(cr, 0, 0, tw, th);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		cairo_restore(cr);
		cairo_surface_destroy(src);
	}
	cairo_destroy(cr);
	cairo_surface_flush(out);
	*origin_x = x0;
	*origin_y = y0;
	*scale_x = scx;
	*scale_y = scy;
	return out;
}

/* ---- -r: the rectangle -------------------------------------------------------------- */

/*
 * The screen is captured first, and the still goes up on an overlay on every monitor,
 * dimmed. The rectangle being dragged shows it as it is. So nothing moves while the
 * rectangle is drawn, and the overlay is never in the picture.
 */

struct shmbuf {
	struct wl_buffer *wl;
	void *data;
	size_t size;
	int w, h;
	bool busy;
};

struct overlay {
	struct output *o;
	cairo_surface_t *still;         /* the monitor as captured, in buffer pixels */
	double sx, sy;                  /* its pixels per logical pixel */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *viewport;
	struct wl_callback *frame;
	bool configured, dirty;
	struct shmbuf bufs[2];
};

enum sel_state { SEL_WAIT, SEL_DRAG, SEL_DONE, SEL_CANCEL };

static struct {
	struct overlay ov[MAX_OUTPUTS];
	int n;
	enum sel_state state;
	struct overlay *under;          /* the overlay the pointer is on */
	double px, py;                  /* the pointer, in the layout */
	double ax, ay;                  /* where the drag began */
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor;
	struct wl_surface *cursor_surface;
	int cursor_scale;
} sel;

static void buffer_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static struct shmbuf *free_buffer(struct overlay *ov, int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = &ov->bufs[i];
		if (b->busy)
			continue;
		if (b->wl && (b->w != w || b->h != h)) {
			wl_buffer_destroy(b->wl);
			munmap(b->data, b->size);
			b->wl = NULL;
		}
		if (!b->wl) {
			b->size = (size_t)w * 4 * h;
			int fd = shm_file(b->size);
			b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (b->data == MAP_FAILED)
				die("mmap: %s", strerror(errno));
			struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, b->size);
			b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
				WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
			wl_buffer_add_listener(b->wl, &buffer_listener, b);
			b->w = w;
			b->h = h;
		}
		return b;
	}
	return NULL;
}

/* The rectangle in the layout, from the drag. */
static void drag_box(double *x, double *y, double *w, double *h)
{
	*x = fmin(sel.ax, sel.px);
	*y = fmin(sel.ay, sel.py);
	*w = fabs(sel.px - sel.ax);
	*h = fabs(sel.py - sel.ay);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_cb_listener = {
	.done = frame_done,
};

static void draw(struct overlay *ov)
{
	int w = cairo_image_surface_get_width(ov->still);
	int h = cairo_image_surface_get_height(ov->still);
	struct shmbuf *b = free_buffer(ov, w, h);
	if (!b) {
		ov->dirty = true;
		return;
	}
	ov->dirty = false;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data,
		CAIRO_FORMAT_RGB24, w, h, w * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_set_source_surface(cr, ov->still, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
	cairo_paint(cr);

	if (sel.state == SEL_DRAG) {
		int lx, ly, lw, lh;
		logical_box(ov->o, &lx, &ly, &lw, &lh);
		double x, y, rw, rh;
		drag_box(&x, &y, &rw, &rh);
		/* On whole pixels, as cut() will: the ones the pointer is on. */
		double x0 = pixel_at((x - lx) * ov->sx), y0 = pixel_at((y - ly) * ov->sy);
		double x1 = pixel_at((x + rw - lx) * ov->sx);
		double y1 = pixel_at((y + rh - ly) * ov->sy);
		cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
		cairo_save(cr);
		cairo_clip(cr);
		cairo_set_source_surface(cr, ov->still, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
		cairo_paint(cr);
		cairo_restore(cr);
		/* The border lies outside, so the picture shows unchanged within. */
		double lw_px = fmax(1, round(ov->sx));
		cairo_set_line_width(cr, lw_px);
		cairo_rectangle(cr, x0 - lw_px / 2, y0 - lw_px / 2, x1 - x0 + lw_px,
			y1 - y0 + lw_px);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(ov->surface, b->wl, 0, 0);
	wl_surface_damage_buffer(ov->surface, 0, 0, w, h);
	ov->frame = wl_surface_frame(ov->surface);
	wl_callback_add_listener(ov->frame, &frame_cb_listener, ov);
	wl_surface_commit(ov->surface);
	b->busy = true;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct overlay *ov = data;
	wl_callback_destroy(cb);
	ov->frame = NULL;
	if (ov->dirty)
		draw(ov);
}

/* A new frame wherever the compositor is ready for one, later where it is not. */
static void redraw_all(void)
{
	for (int i = 0; i < sel.n; i++) {
		struct overlay *ov = &sel.ov[i];
		ov->dirty = true;
		if (ov->configured && !ov->frame)
			draw(ov);
	}
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h)
{
	struct overlay *ov = data;
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	if (ov->viewport && w > 0 && h > 0)
		wp_viewport_set_destination(ov->viewport, w, h);
	ov->configured = true;
	if (!ov->frame)
		draw(ov);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	sel.state = SEL_CANCEL;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static struct overlay *overlay_of(struct wl_surface *surface)
{
	for (int i = 0; i < sel.n; i++)
		if (sel.ov[i].surface == surface)
			return &sel.ov[i];
	return NULL;
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	sel.under = overlay_of(surface);
	if (!sel.under)
		return;
	int lx, ly, lw, lh;
	logical_box(sel.under->o, &lx, &ly, &lw, &lh);
	sel.px = lx + wl_fixed_to_double(sx);
	sel.py = ly + wl_fixed_to_double(sy);
	if (sel.cursor)
		wl_pointer_set_cursor(p, serial, sel.cursor_surface,
			sel.cursor->hotspot_x / sel.cursor_scale,
			sel.cursor->hotspot_y / sel.cursor_scale);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surface)
{
	/* During a drag the pointer stays with the overlay it began on. */
	if (sel.state != SEL_DRAG)
		sel.under = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t sx, wl_fixed_t sy)
{
	if (!sel.under)
		return;
	int lx, ly, lw, lh;
	logical_box(sel.under->o, &lx, &ly, &lw, &lh);
	sel.px = lx + wl_fixed_to_double(sx);
	sel.py = ly + wl_fixed_to_double(sy);
	if (sel.state == SEL_DRAG)
		redraw_all();
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (button == BTN_LEFT && down && sel.state == SEL_WAIT && sel.under) {
		sel.ax = sel.px;
		sel.ay = sel.py;
		sel.state = SEL_DRAG;
		redraw_all();
	} else if (button == BTN_LEFT && !down && sel.state == SEL_DRAG) {
		sel.state = SEL_DONE;
	} else if (button != BTN_LEFT && down) {
		sel.state = SEL_CANCEL;
	}
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *surface, struct wl_array *keys)
{
}

static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *surface)
{
}

/* Esc by its evdev code: whatever the layout, it is that key. */
static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t key, uint32_t state)
{
	if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		sel.state = SEL_CANCEL;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

/* The crosshair from the cursor theme, as large as the sharpest monitor wants. */
static void load_cursor(void)
{
	double scale = 1;
	for (int i = 0; i < sel.n; i++)
		scale = fmax(scale, fmax(sel.ov[i].sx, sel.ov[i].sy));
	sel.cursor_scale = (int)ceil(scale);
	const char *size_env = getenv("XCURSOR_SIZE");
	int size = size_env && atoi(size_env) > 0 ? atoi(size_env) : 24;
	sel.cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
		size * sel.cursor_scale, shm);
	if (!sel.cursor_theme)
		return;
	const char *names[] = { "crosshair", "cross", "tcross", "left_ptr", "default" };
	struct wl_cursor *c = NULL;
	for (size_t i = 0; !c && i < sizeof(names) / sizeof(*names); i++)
		c = wl_cursor_theme_get_cursor(sel.cursor_theme, names[i]);
	if (!c || c->image_count == 0)
		return;
	sel.cursor = c->images[0];
	/* A theme has only some sizes: Adwaita gives 64x64 for 72. A buffer scale that
	 * doesn't divide the size is a protocol error, so take the largest one up to the
	 * wanted scale that does; the compositor scales the rest. */
	int want = sel.cursor_scale;
	sel.cursor_scale = 1;
	for (int d = want; d > 1; d--)
		if (sel.cursor->width % d == 0 && sel.cursor->height % d == 0) {
			sel.cursor_scale = d;
			break;
		}
	sel.cursor_surface = wl_compositor_create_surface(compositor);
	wl_surface_set_buffer_scale(sel.cursor_surface, sel.cursor_scale);
	wl_surface_attach(sel.cursor_surface, wl_cursor_image_get_buffer(sel.cursor), 0, 0);
	wl_surface_commit(sel.cursor_surface);
}

/*
 * Shows the still and lets the mouse draw a rectangle on it. Returns false when given
 * up. A click without a drag takes the monitor it is on.
 */
static bool select_rectangle(struct output **list, int n, double *x, double *y,
	double *w, double *h)
{
	if (!compositor || !layer_shell)
		die("-r needs wlr-layer-shell, which the compositor does not offer");
	if (!seat || !(seat_caps & WL_SEAT_CAPABILITY_POINTER))
		die("-r needs a mouse, and the compositor has none");

	memset(&sel, 0, sizeof(sel));
	sel.n = n;
	for (int i = 0; i < n; i++) {
		struct overlay *ov = &sel.ov[i];
		int ox, oy;
		ov->o = list[i];
		ov->still = compose(&list[i], 1, &ox, &oy, &ov->sx, &ov->sy);
		ov->surface = wl_compositor_create_surface(compositor);
		if (viewporter)
			ov->viewport = wp_viewporter_get_viewport(viewporter, ov->surface);
		else
			wl_surface_set_buffer_scale(ov->surface, (int)round(ov->sx));
		ov->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, ov->surface,
			ov->o->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screenShot");
		zwlr_layer_surface_v1_add_listener(ov->layer, &layer_listener, ov);
		zwlr_layer_surface_v1_set_anchor(ov->layer,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_exclusive_zone(ov->layer, -1);
		/* For Esc: exclusive, or the key would go to the window under it. */
		zwlr_layer_surface_v1_set_keyboard_interactivity(ov->layer, 1);
		wl_surface_commit(ov->surface);
	}
	load_cursor();
	sel.pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(sel.pointer, &pointer_listener, NULL);
	if (seat_caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		sel.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(sel.keyboard, &keyboard_listener, NULL);
	}

	while (sel.state == SEL_WAIT || sel.state == SEL_DRAG)
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");

	bool done = sel.state == SEL_DONE;
	if (done) {
		drag_box(x, y, w, h);
		if (*w < 1 && *h < 1 && sel.under) {
			int lx, ly, lw, lh;
			logical_box(sel.under->o, &lx, &ly, &lw, &lh);
			*x = lx;
			*y = ly;
			*w = lw;
			*h = lh;
		}
	}

	/* Gone before anything else happens, so no frame of it lingers. */
	wl_pointer_destroy(sel.pointer);
	if (sel.keyboard)
		wl_keyboard_destroy(sel.keyboard);
	for (int i = 0; i < n; i++) {
		struct overlay *ov = &sel.ov[i];
		if (ov->frame)
			wl_callback_destroy(ov->frame);
		if (ov->viewport)
			wp_viewport_destroy(ov->viewport);
		zwlr_layer_surface_v1_destroy(ov->layer);
		wl_surface_destroy(ov->surface);
		for (int j = 0; j < 2; j++) {
			if (ov->bufs[j].wl) {
				wl_buffer_destroy(ov->bufs[j].wl);
				munmap(ov->bufs[j].data, ov->bufs[j].size);
			}
		}
		cairo_surface_destroy(ov->still);
	}
	if (sel.cursor_surface)
		wl_surface_destroy(sel.cursor_surface);
	if (sel.cursor_theme)
		wl_cursor_theme_destroy(sel.cursor_theme);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	return done;
}

/*
 * The piece of the picture that the rectangle covers, in the layout's coordinates. An edge
 * goes to the pixel the pointer is on: a pointer at 100.5 is on pixel 100.
 */
static cairo_surface_t *cut(cairo_surface_t *picture, int origin_x, int origin_y,
	double sx, double sy, double x, double y, double w, double h)
{
	int pw = cairo_image_surface_get_width(picture);
	int ph = cairo_image_surface_get_height(picture);
	int x0 = pixel_at((x - origin_x) * sx), y0 = pixel_at((y - origin_y) * sy);
	int x1 = pixel_at((x + w - origin_x) * sx);
	int y1 = pixel_at((y + h - origin_y) * sy);
	x0 = x0 < 0 ? 0 : x0 > pw ? pw : x0;
	x1 = x1 < 0 ? 0 : x1 > pw ? pw : x1;
	y0 = y0 < 0 ? 0 : y0 > ph ? ph : y0;
	y1 = y1 < 0 ? 0 : y1 > ph ? ph : y1;
	/* A drag of a hair still gets a pixel. */
	if (x1 == x0 && x1 < pw)
		x1++;
	if (y1 == y0 && y1 < ph)
		y1++;
	if (x1 <= x0 || y1 <= y0)
		die("the rectangle lies outside every monitor");
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, x1 - x0,
		y1 - y0);
	cairo_t *cr = cairo_create(out);
	cairo_set_source_surface(cr, picture, -x0, -y0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(out);
	return out;
}

/* ---- PNG ---------------------------------------------------------------------------- */

struct mem {
	unsigned char *data;
	size_t len, cap;
};

static cairo_status_t mem_write(void *closure, const unsigned char *data,
	unsigned int len)
{
	struct mem *m = closure;
	if (m->len + len > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 1 << 20;
		while (cap < m->len + len)
			cap *= 2;
		unsigned char *p = realloc(m->data, cap);
		if (!p)
			return CAIRO_STATUS_NO_MEMORY;
		m->data = p;
		m->cap = cap;
	}
	memcpy(m->data + m->len, data, len);
	m->len += len;
	return CAIRO_STATUS_SUCCESS;
}

static void encode_png(cairo_surface_t *s, struct mem *png)
{
	if (cairo_surface_write_to_png_stream(s, mem_write, png) != CAIRO_STATUS_SUCCESS)
		die("cannot encode the PNG");
}

static bool write_all(int fd, const unsigned char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		data += n;
		len -= n;
	}
	return true;
}

/* ---- files -------------------------------------------------------------------------- */

static void mkdirs(const char *path)
{
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) < 0 && errno != EEXIST)
			die("cannot make %s: %s", buf, strerror(errno));
		*p = '/';
	}
	if (mkdir(buf, 0755) < 0 && errno != EEXIST)
		die("cannot make %s: %s", buf, strerror(errno));
}

/*
 * The pictures folder as xdg-user-dirs has it: XDG_PICTURES_DIR in user-dirs.dirs,
 * "$HOME/Bilder" on a German system. Without one ~/Pictures.
 */
static void pictures_dir(char *out, size_t size)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		die("HOME is not set");
	char path[PATH_MAX];
	const char *config = getenv("XDG_CONFIG_HOME");
	if (config && *config)
		snprintf(path, sizeof(path), "%s/user-dirs.dirs", config);
	else
		snprintf(path, sizeof(path), "%s/.config/user-dirs.dirs", home);

	snprintf(out, size, "%s/Pictures", home);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, "XDG_PICTURES_DIR=", 17) != 0)
			continue;
		p += 17;
		if (*p != '"')
			continue;
		char *end = strrchr(++p, '"');
		if (!end || end == p)
			continue;
		*end = '\0';
		if (strncmp(p, "$HOME", 5) == 0 && (p[5] == '/' || p[5] == '\0'))
			snprintf(out, size, "%s%s", home, p + 5);
		else if (p[0] == '/')
			snprintf(out, size, "%s", p);
		/* Nothing else is allowed there; anything else keeps the one before. */
		size_t n = strlen(out);
		if (n > 1 && out[n - 1] == '/')
			out[n - 1] = '\0';
	}
	fclose(f);
}

/* A file next to where it goes, so nobody ever sees half a picture there. */
static int temp_file(const char *dir, char *path, size_t size)
{
	snprintf(path, size, "%s/.screenShot-XXXXXX", dir);
	int fd = mkostemp(path, O_CLOEXEC);
	if (fd < 0)
		die("cannot write in %s: %s", dir, strerror(errno));
	/* mkostemp gives 0600; a plain file would get 0666 less the umask. fchmod
	 * doesn't apply the umask itself, and umask 077 must keep a picture private. */
	mode_t mask = umask(0);
	umask(mask);
	fchmod(fd, 0666 & ~mask);
	return fd;
}

static void write_temp(int fd, const char *tmp, const struct mem *png)
{
	if (!write_all(fd, png->data, png->len) || fsync(fd) < 0 || close(fd) < 0) {
		int err = errno;
		unlink(tmp);
		die("cannot write %s: %s", tmp, strerror(err));
	}
}

/* printf only buffers: a full disk or a reader gone shows only here. */
static void flush_stdout(void)
{
	if (fflush(stdout) == EOF || ferror(stdout))
		die("cannot write to stdout: %s", strerror(errno));
}

/* Prints the path, absolute, and keeps it in saved for the toast. The picture stays
 * where it is when that fails, but whoever waits for the path is told. */
static void print_path(const char *path, char *saved, size_t size)
{
	char abs[PATH_MAX];
	snprintf(saved, size, "%s", realpath(path, abs) ? abs : path);
	printf("%s\n", saved);
	flush_stdout();
}

/*
 * tmp under the name path, unless a file is there: 0, else an errno. link() never
 * overwrites. vfat, exFAT and some network filesystems have no hard links; there
 * renameat2 with RENAME_NOREPLACE does the same.
 */
static int put_new(const char *tmp, const char *path)
{
	if (link(tmp, path) == 0) {
		unlink(tmp);
		return 0;
	}
	int err = errno;
	if (err != EPERM && err != EOPNOTSUPP && err != ENOSYS)
		return err;
	if (renameat2(AT_FDCWD, tmp, AT_FDCWD, path, RENAME_NOREPLACE) == 0)
		return 0;
	/* EINVAL: no RENAME_NOREPLACE either; the missing hard link is the news. */
	return errno == EINVAL ? err : errno;
}

/* Into the pictures folder, under a name of its own: a second one in the same second
 * gets _2, and none overwrites another. */
static void save_default(const struct mem *png, char *saved, size_t size)
{
	char dir[PATH_MAX - 64];
	pictures_dir(dir, sizeof(dir) - 16);
	strcat(dir, "/Screenshots");
	mkdirs(dir);

	char stamp[64];
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "Screenshot_%Y-%m-%d_%H-%M-%S", localtime(&now));

	char tmp[PATH_MAX], path[PATH_MAX];
	write_temp(temp_file(dir, tmp, sizeof(tmp)), tmp, png);
	for (int i = 1; i < 1000; i++) {
		if (i == 1)
			snprintf(path, sizeof(path), "%s/%s.png", dir, stamp);
		else
			snprintf(path, sizeof(path), "%s/%s_%d.png", dir, stamp, i);
		int err = put_new(tmp, path);
		if (err == 0) {
			print_path(path, saved, size);
			return;
		}
		if (err != EEXIST) {
			unlink(tmp);
			die("cannot write %s: %s", path, strerror(err));
		}
	}
	unlink(tmp);
	die("too many screenshots named %s in %s", stamp, dir);
}

/* One's own or root's link, or one to stdout itself, as /dev/stdout is. In a container
 * root's may show as nobody's, so stdout is told by what it is, not by who owns it. */
static bool link_to_follow(const char *file, const struct stat *link)
{
	if (link->st_uid == geteuid() || link->st_uid == 0)
		return true;
	struct stat to, out;
	return stat(file, &to) == 0 && fstat(STDOUT_FILENO, &out) == 0 &&
		to.st_dev == out.st_dev && to.st_ino == out.st_ino;
}

static void save_file(const char *file, const struct mem *png, char *saved, size_t size)
{
	if (strcmp(file, "-") == 0) {
		if (!write_all(STDOUT_FILENO, png->data, png->len))
			die("cannot write to stdout: %s", strerror(errno));
		return;
	}
	/*
	 * A plain file gets its new content all at once, through a rename, and keeps its
	 * mode: a private file stays private. Anything else is written into, as a shell's >
	 * would: a link is followed and stays a link, a pipe stays a pipe. Someone else's
	 * link is replaced, as a missing file would be: one put into a shared folder could
	 * point at ~/.bashrc, or at a pipe its owner reads.
	 */
	struct stat st;
	bool there = lstat(file, &st) == 0;
	if (there && S_ISLNK(st.st_mode) && !link_to_follow(file, &st))
		there = false;
	else if (there && !S_ISREG(st.st_mode)) {
		int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
		if (fd < 0 || !write_all(fd, png->data, png->len))
			die("cannot write %s: %s", file, strerror(errno));
		/* No path into a pipe, nor into the picture itself, as with -f /dev/stdout. */
		struct stat out;
		bool plain = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
			!(fstat(STDOUT_FILENO, &out) == 0 && out.st_dev == st.st_dev &&
				out.st_ino == st.st_ino);
		if (close(fd) < 0)
			die("cannot write %s: %s", file, strerror(errno));
		if (plain)
			print_path(file, saved, size);
		return;
	}
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", file);
	char *slash = strrchr(dir, '/');
	if (!slash)
		snprintf(dir, sizeof(dir), ".");
	else if (slash == dir)
		dir[1] = '\0';
	else
		*slash = '\0';
	char tmp[PATH_MAX];
	int fd = temp_file(dir, tmp, sizeof(tmp));
	if (there)
		fchmod(fd, st.st_mode & 0777);
	write_temp(fd, tmp, png);
	if (rename(tmp, file) < 0) {
		int err = errno;
		unlink(tmp);
		die("cannot write %s: %s", file, strerror(err));
	}
	print_path(file, saved, size);
}

/* ---- the toast ---------------------------------------------------------------------- */

/*
 * For three seconds, at the top in the middle, a little below the edge: a thumbnail and
 * what became of the screenshot.
 * Clicks and keys go through it to what is below. It runs in the background, so the
 * command line gets its prompt, and $(screenShot) its path, at once.
 */

#define TOAST_MS 3000
#define TOAST_MARGIN 32         /* below the top edge */
#define TOAST_PAD 12
#define TOAST_THUMB_W 96
#define TOAST_THUMB_H 54

static struct {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct shmbuf buf;
	cairo_surface_t *picture;
	char title[64], detail[256];
	int w, h;                       /* logical */
	int scale;
	bool configured;
} toast;

static void toast_line_font(cairo_t *cr, bool title)
{
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		title ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, title ? 14 : 12);
}

/* The thumbnail's size, within its box and in the picture's shape. */
static void toast_thumb_size(double *tw, double *th)
{
	double pw = cairo_image_surface_get_width(toast.picture);
	double ph = cairo_image_surface_get_height(toast.picture);
	double f = fmin(TOAST_THUMB_W / pw, TOAST_THUMB_H / ph);
	*tw = fmax(1, round(pw * f));
	*th = fmax(1, round(ph * f));
}

static void toast_measure(void)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	cairo_t *cr = cairo_create(s);
	cairo_text_extents_t a, b;
	toast_line_font(cr, true);
	cairo_text_extents(cr, toast.title, &a);
	toast_line_font(cr, false);
	cairo_text_extents(cr, toast.detail, &b);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	double text = fmin(fmax(a.x_advance, b.x_advance), 480);
	toast.w = (int)ceil(TOAST_PAD + TOAST_THUMB_W + TOAST_PAD + text + TOAST_PAD);
	toast.h = TOAST_PAD + TOAST_THUMB_H + TOAST_PAD;
}

static void toast_draw(void)
{
	int s = toast.scale, w = toast.w * s, h = toast.h * s;
	if (toast.buf.busy)
		return;
	if (toast.buf.wl && (toast.buf.w != w || toast.buf.h != h)) {
		wl_buffer_destroy(toast.buf.wl);
		munmap(toast.buf.data, toast.buf.size);
		toast.buf.wl = NULL;
	}
	if (!toast.buf.wl) {
		struct shmbuf *b = &toast.buf;
		b->size = (size_t)w * 4 * h;
		int fd = shm_file(b->size);
		b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (b->data == MAP_FAILED)
			die("mmap: %s", strerror(errno));
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, b->size);
		b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
		close(fd);
		wl_buffer_add_listener(b->wl, &buffer_listener, b);
		b->w = w;
		b->h = h;
	}
	cairo_surface_t *cs = cairo_image_surface_create_for_data(toast.buf.data,
		CAIRO_FORMAT_RGB24, w, h, w * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, s, s);
	cairo_set_source_rgb(cr, 0x2b / 255.0, 0x2b / 255.0, 0x2b / 255.0);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0x80 / 255.0, 0x80 / 255.0, 0x80 / 255.0);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, toast.w - 1, toast.h - 1);
	cairo_stroke(cr);

	double tw, th;
	toast_thumb_size(&tw, &th);
	double tx = TOAST_PAD + (TOAST_THUMB_W - tw) / 2;
	double ty = TOAST_PAD + (TOAST_THUMB_H - th) / 2;
	cairo_save(cr);
	cairo_rectangle(cr, tx, ty, tw, th);
	cairo_clip(cr);
	cairo_translate(cr, tx, ty);
	cairo_scale(cr, tw / cairo_image_surface_get_width(toast.picture),
		th / cairo_image_surface_get_height(toast.picture));
	cairo_set_source_surface(cr, toast.picture, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, 0x80 / 255.0, 0x80 / 255.0, 0x80 / 255.0);
	cairo_rectangle(cr, tx - 0.5, ty - 0.5, tw + 1, th + 1);
	cairo_stroke(cr);

	double x = TOAST_PAD + TOAST_THUMB_W + TOAST_PAD;
	cairo_rectangle(cr, x, 0, toast.w - x - TOAST_PAD, toast.h);
	cairo_clip(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	toast_line_font(cr, true);
	cairo_move_to(cr, x, toast.h / 2.0 - 6);
	cairo_show_text(cr, toast.title);
	cairo_set_source_rgb(cr, 0xd0 / 255.0, 0xd0 / 255.0, 0xd0 / 255.0);
	toast_line_font(cr, false);
	cairo_move_to(cr, x, toast.h / 2.0 + 14);
	cairo_show_text(cr, toast.detail);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_set_buffer_scale(toast.surface, s);
	wl_surface_attach(toast.surface, toast.buf.wl, 0, 0);
	wl_surface_damage_buffer(toast.surface, 0, 0, w, h);
	wl_surface_commit(toast.surface);
	toast.buf.busy = true;
}

static void toast_configure(void *data, struct zwlr_layer_surface_v1 *layer,
	uint32_t serial, uint32_t w, uint32_t h)
{
	zwlr_layer_surface_v1_ack_configure(layer, serial);
	toast.configured = true;
	toast_draw();
}

static void toast_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
}

static const struct zwlr_layer_surface_v1_listener toast_layer_listener = {
	.configure = toast_configure,
	.closed = toast_closed,
};

static void toast_surface_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void toast_surface_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
}

/* Sharp on the monitor it ends up on. */
static void toast_preferred_scale(void *data, struct wl_surface *s, int32_t scale)
{
	if (scale < 1 || scale == toast.scale)
		return;
	toast.scale = scale;
	if (toast.configured)
		toast_draw();
}

static void toast_preferred_transform(void *data, struct wl_surface *s,
	uint32_t transform)
{
}

static const struct wl_surface_listener toast_surface_listener = {
	.enter = toast_surface_enter,
	.leave = toast_surface_leave,
	.preferred_buffer_scale = toast_preferred_scale,
	.preferred_buffer_transform = toast_preferred_transform,
};

/* Into the background, away from the terminal: stdout must close, or $(...) waits. */
static void go_background(void)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid > 0)
		_exit(0); /* the child keeps the connection: no disconnect here */
	setsid();
	/* It may live for hours: it must not keep a directory busy, a stick from unmounting. */
	if (chdir("/") < 0) {
		/* Then it stays where it is; the clipboard matters more. */
	}
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		if (null > STDERR_FILENO)
			close(null);
	}
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* Handles events until the time is up, or until the compositor is gone. */
static bool dispatch_until(long deadline)
{
	for (;;) {
		long left = deadline - now_ms();
		if (left <= 0)
			return true;
		while (wl_display_prepare_read(display) != 0)
			if (wl_display_dispatch_pending(display) < 0)
				return false;
		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(display);
			return false;
		}
		struct pollfd p = { .fd = wl_display_get_fd(display), .events = POLLIN };
		int r = poll(&p, 1, (int)left);
		if (r > 0) {
			if (wl_display_read_events(display) < 0)
				return false;
		} else {
			wl_display_cancel_read(display);
			if (r < 0 && errno != EINTR)
				return false;
		}
		if (wl_display_dispatch_pending(display) < 0)
			return false;
	}
}

/* Up for TOAST_MS, then gone. Without layer-shell there is none. */
static void show_toast(cairo_surface_t *picture, const char *title, const char *detail)
{
	if (!compositor || !layer_shell || !picture)
		return;
	memset(&toast, 0, sizeof(toast));
	toast.picture = picture;
	snprintf(toast.title, sizeof(toast.title), "%s", title);
	snprintf(toast.detail, sizeof(toast.detail), "%s", detail);
	toast.scale = 1;
	for (int i = 0; i < n_outputs; i++)
		if (outputs[i].scale > toast.scale)
			toast.scale = outputs[i].scale;
	toast_measure();

	toast.surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(toast.surface, &toast_surface_listener, NULL);
	struct wl_region *none = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(toast.surface, none);
	wl_region_destroy(none);
	/* No output named: the compositor puts it where the user is. */
	toast.layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, toast.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screenShot-toast");
	zwlr_layer_surface_v1_add_listener(toast.layer, &toast_layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(toast.layer, toast.w, toast.h);
	/* Anchored to the top alone, it is centred across. */
	zwlr_layer_surface_v1_set_anchor(toast.layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
	zwlr_layer_surface_v1_set_margin(toast.layer, TOAST_MARGIN, 0, 0, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(toast.layer, 0);
	wl_surface_commit(toast.surface);

	dispatch_until(now_ms() + TOAST_MS);

	zwlr_layer_surface_v1_destroy(toast.layer);
	wl_surface_destroy(toast.surface);
	if (toast.buf.wl) {
		wl_buffer_destroy(toast.buf.wl);
		munmap(toast.buf.data, toast.buf.size);
	}
	wl_display_roundtrip(display);
}

/* ---- the clipboard ------------------------------------------------------------------ */

static struct mem clip;
static bool clip_cancelled;

/* A reader that stops reading would block us for good, and with us every later paste
 * and the end of the toast. A slow one is fine as long as it makes progress. */
#define PASTE_STALL_MS 5000

static void paste_to(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	const unsigned char *data = clip.data;
	size_t len = clip.len;
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n > 0) {
			data += n;
			len -= n;
		} else if (n < 0 && errno == EAGAIN) {
			struct pollfd p = { .fd = fd, .events = POLLOUT };
			int r = poll(&p, 1, PASTE_STALL_MS);
			if (r == 0 || (r < 0 && errno != EINTR))
				return;
		} else if (n < 0 && errno != EINTR) {
			return;
		}
	}
}

static void source_send(void *data, struct ext_data_control_source_v1 *source,
	const char *mime, int32_t fd)
{
	if (strcmp(mime, "image/png") == 0)
		paste_to(fd);
	close(fd);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
	clip_cancelled = true;
}

static const struct ext_data_control_source_v1_listener source_listener = {
	.send = source_send,
	.cancelled = source_cancelled,
};

/* What others put into the clipboard: we never read it, but must let go of it. */
static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer)
{
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer)
{
	if (offer)
		ext_data_control_offer_v1_destroy(offer);
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device)
{
	clip_cancelled = true;
}

static void device_primary_selection(void *data,
	struct ext_data_control_device_v1 *device, struct ext_data_control_offer_v1 *offer)
{
	if (offer)
		ext_data_control_offer_v1_destroy(offer);
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_primary_selection,
};

/*
 * On Wayland the clipboard holds no data, only a promise: whoever copied hands it out
 * when someone pastes. So screenShot stays, in the background, until something else is
 * copied.
 */
static void to_clipboard(struct mem *png, cairo_surface_t *picture)
{
	if (!seat)
		die("the compositor has no seat");
	clip = *png;
	struct ext_data_control_device_v1 *device =
		ext_data_control_manager_v1_get_data_device(data_control, seat);
	ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
	struct ext_data_control_source_v1 *source =
		ext_data_control_manager_v1_create_data_source(data_control);
	ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
	ext_data_control_source_v1_offer(source, "image/png");
	ext_data_control_device_v1_set_selection(device, source);
	if (wl_display_roundtrip(display) < 0)
		die("lost the compositor");
	if (clip_cancelled)
		die("the compositor did not take the selection");

	go_background();
	if (picture) {
		char detail[64];
		snprintf(detail, sizeof(detail), "%d x %d, in the clipboard",
			cairo_image_surface_get_width(picture),
			cairo_image_surface_get_height(picture));
		show_toast(picture, "Screenshot copied", detail);
		cairo_surface_destroy(picture);
	}
	while (!clip_cancelled && wl_display_dispatch(display) >= 0)
		;
	exit(0);
}

/* ---- main --------------------------------------------------------------------------- */

static void list_outputs(void)
{
	for (int i = 0; i < n_outputs; i++) {
		const struct output *o = &outputs[i];
		int x, y, w, h;
		logical_box(o, &x, &y, &w, &h);
		/* The real scale, also a fractional one; wl_output's is rounded up. */
		int mw = o->transform & 1 ? o->mode_h : o->mode_w;
		double scale = mw > 0 && w > 0 ? (double)mw / w : o->scale > 0 ? o->scale : 1;
		printf("%s %dx%d%+d%+d scale %g\n", o->name, w, h, x, y,
			round(scale * 100) / 100);
	}
}

int main(int argc, char *argv[])
{
	bool clipboard = false, pointer = false, list = false, region = false, quiet = false;
	bool version = false, help = false, delayed = false;
	const char *file = NULL, *only = NULL;
	double delay = 0;
	int opt;
	while ((opt = getopt(argc, argv, "cf:o:d:plqrvh")) != -1) {
		switch (opt) {
		case 'c':
			clipboard = true;
			break;
		case 'f':
			file = optarg;
			break;
		case 'o':
			only = optarg;
			break;
		case 'd': {
			char *end;
			delay = strtod(optarg, &end);
			/* !(0 <= d <= 3600) also catches nan; end == optarg an empty one. */
			if (end == optarg || *end || !(delay >= 0 && delay <= 3600) ||
				strpbrk(optarg, "xX")) {
				fprintf(stderr, "screenShot: -d wants seconds, not %s\n", optarg);
				return 2;
			}
			delayed = true;
			break;
		}
		case 'p':
			pointer = true;
			break;
		case 'l':
			list = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'r':
			region = true;
			break;
		case 'v':
			version = true;
			break;
		case 'h':
			help = true;
			break;
		default:
			usage(stderr);
			return 2;
		}
	}
	/* -l, -v and -h stand alone: anything else with them would be ignored. -q, no
	 * toast, holds for them anyway. */
	bool shot = clipboard || file || only || region || pointer || delayed;
	if (optind < argc || (clipboard && file) || (list && (shot || version || help)) ||
			(version && (shot || help)) || (help && shot)) {
		usage(stderr);
		return 2;
	}
	if (help) {
		usage(stdout);
		flush_stdout();
		return 0;
	}
	if (version) {
		printf("screenShot %s\n", SCREENSHOT_VERSION);
		flush_stdout();
		return 0;
	}
	/* A reader that leaves early must not kill the clipboard. */
	signal(SIGPIPE, SIG_IGN);

	connect_display();
	if (!shm)
		die("the compositor has no wl_shm");
	read_outputs();
	if (list) {
		list_outputs();
		flush_stdout();
		return 0;
	}
	if (clipboard && !data_control)
		die("the compositor offers no ext-data-control-v1, so no clipboard");

	struct output *chosen[MAX_OUTPUTS];
	int n = 0;
	for (int i = 0; i < n_outputs; i++)
		if (!only || strcmp(outputs[i].name, only) == 0)
			chosen[n++] = &outputs[i];
	if (n == 0) {
		if (only)
			die("no monitor named %s (screenShot -l lists them)", only);
		die("the compositor has no monitors");
	}

	if (delay > 0) {
		struct timespec ts = { (time_t)delay, (long)((delay - (time_t)delay) * 1e9) };
		while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
			;
	}
	for (int i = 0; i < n; i++)
		capture(chosen[i], pointer);
	for (;;) {
		int pending = 0;
		for (int i = 0; i < n; i++)
			if (!chosen[i]->done && !chosen[i]->failed)
				pending++;
		if (!pending)
			break;
		if (wl_display_dispatch(display) < 0)
			die("lost the compositor");
	}
	for (int i = 0; i < n; i++)
		if (chosen[i]->failed)
			return 1;
	for (int i = 0; i < n; i++)
		swap_red_blue(chosen[i]);

	int origin_x, origin_y;
	double sx, sy;
	cairo_surface_t *picture = compose(chosen, n, &origin_x, &origin_y, &sx, &sy);
	if (region) {
		double x, y, w, h;
		if (!select_rectangle(chosen, n, &x, &y, &w, &h))
			die("given up, no screenshot");
		cairo_surface_t *piece = cut(picture, origin_x, origin_y, sx, sy, x, y, w, h);
		cairo_surface_destroy(picture);
		picture = piece;
	}
	for (int i = 0; i < n; i++)
		release_capture(chosen[i]);
	struct mem png = { 0 };
	encode_png(picture, &png);

	char saved[PATH_MAX] = "";
	if (clipboard) {
		if (quiet) {
			cairo_surface_destroy(picture);
			picture = NULL;
		}
		to_clipboard(&png, picture);
	} else if (file) {
		save_file(file, &png, saved, sizeof(saved));
		/* Into a pipe, no file: someone else takes it from there. */
		if (!saved[0])
			quiet = true;
	} else {
		save_default(&png, saved, sizeof(saved));
	}
	if (!quiet && compositor && layer_shell) {
		go_background();
		const char *name = strrchr(saved, '/');
		show_toast(picture, "Screenshot saved", name ? name + 1 : saved);
	}
	return 0;
}
