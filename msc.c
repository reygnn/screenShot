/*
 * msc - takes a screenshot of the whole screen, into a file or the clipboard. For mwc
 * (https://github.com/reygnn/mwc) and any other Wayland compositor with the same
 * protocols.
 *
 *   msc               Screenshots/Screenshot_<date>_<time>.png in the pictures folder,
 *                     the path goes to stdout
 *   msc -c            the clipboard, as image/png
 *   msc -f FILE       that file, "-" for stdout
 *   msc -o NAME       only that monitor, -l lists them
 *
 * The screen comes through wlr-screencopy, the clipboard through ext-data-control-v1.
 * Without WAYLAND_DISPLAY, as from an ssh login or a session started elsewhere, it looks
 * for a compositor in the runtime directory by itself.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"
#include "version.h"
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
static struct output outputs[MAX_OUTPUTS];
static int n_outputs;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("msc: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static void usage(FILE *f)
{
	fputs("usage: msc [-c | -f FILE] [-o NAME] [-d SECONDS] [-p]\n"
		"       msc -l | -v\n"
		"  (none)     to Screenshots/ in the pictures folder, prints the path\n"
		"  -c         to the clipboard, as image/png\n"
		"  -f FILE    to FILE, - for stdout\n"
		"  -o NAME    only that monitor\n"
		"  -d SECONDS wait that long first\n"
		"  -p         with the mouse pointer\n"
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

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		if (!seat)
			seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
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
	return strcmp(*(char *const *)a, *(char *const *)b);
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
	char *names[64];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 64) {
		size_t len = strlen(e->d_name);
		if (strncmp(e->d_name, "wayland-", 8) != 0 ||
				(len > 5 && strcmp(e->d_name + len - 5, ".lock") == 0) ||
				!is_socket(dir, e->d_name))
			continue;
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(*names), compare_names);
	bool found = false;
	for (int i = 0; i < n; i++) {
		if (!found && try_display(names[i])) {
			found = true;
			/* For the clipboard child and anything it might start. */
			setenv("WAYLAND_DISPLAY", names[i], 1);
		}
		free(names[i]);
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
	int fd = memfd_create("msc", MFD_CLOEXEC);
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
		fprintf(stderr, "msc: %s: the compositor offers no shm format msc can read\n",
			o->name);
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
	fprintf(stderr, "msc: %s: the compositor could not copy the screen\n", o->name);
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

/* Its buffer pixels per logical pixel. */
static double output_scale(const struct output *o)
{
	int x, y, w, h, tw, th;
	logical_box(o, &x, &y, &w, &h);
	transformed_size(o, &tw, &th);
	return w > 0 ? (double)tw / w : 1;
}

static void swap_red_blue(struct output *o)
{
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
 * monitor covers stays black.
 */
static cairo_surface_t *compose(struct output **list, int n)
{
	int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
	double scale = 0;
	for (int i = 0; i < n; i++) {
		int x, y, w, h;
		logical_box(list[i], &x, &y, &w, &h);
		if (x < x0) x0 = x;
		if (y < y0) y0 = y;
		if (x + w > x1) x1 = x + w;
		if (y + h > y1) y1 = y + h;
		double s = output_scale(list[i]);
		if (s > scale)
			scale = s;
	}
	int width = (int)((x1 - x0) * scale + 0.5);
	int height = (int)((y1 - y0) * scale + 0.5);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS)
		die("cannot make a %dx%d picture", width, height);
	cairo_t *cr = cairo_create(out);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	for (int i = 0; i < n; i++) {
		struct output *o = list[i];
		if (o->format == WL_SHM_FORMAT_XBGR8888 || o->format == WL_SHM_FORMAT_ABGR8888)
			swap_red_blue(o);
		/* Alpha on a screen means nothing: x or a, it is opaque. */
		cairo_surface_t *src = cairo_image_surface_create_for_data(o->data,
			CAIRO_FORMAT_RGB24, o->width, o->height, o->stride);
		int x, y, w, h, tw, th;
		logical_box(o, &x, &y, &w, &h);
		transformed_size(o, &tw, &th);
		double s = scale / output_scale(o);

		cairo_save(cr);
		cairo_translate(cr, (x - x0) * scale, (y - y0) * scale);
		cairo_scale(cr, s, s);
		cairo_pattern_t *p = cairo_pattern_create_for_surface(src);
		cairo_matrix_t m;
		buffer_matrix(o, &m);
		cairo_pattern_set_matrix(p, &m);
		cairo_pattern_set_filter(p, s == 1 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
		cairo_set_source(cr, p);
		cairo_rectangle(cr, 0, 0, tw, th);
		cairo_fill(cr);
		cairo_pattern_destroy(p);
		cairo_restore(cr);
		cairo_surface_destroy(src);
	}
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
	snprintf(path, size, "%s/.msc-XXXXXX", dir);
	int fd = mkostemp(path, O_CLOEXEC);
	if (fd < 0)
		die("cannot write in %s: %s", dir, strerror(errno));
	fchmod(fd, 0644);
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

static void print_path(const char *path)
{
	char abs[PATH_MAX];
	printf("%s\n", realpath(path, abs) ? abs : path);
}

/* Into the pictures folder, under a name of its own: a second one in the same second
 * gets _2, and none overwrites another. */
static void save_default(const struct mem *png)
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
		/* link() fails where a file is there: nothing is overwritten. */
		if (link(tmp, path) == 0) {
			unlink(tmp);
			print_path(path);
			return;
		}
		if (errno != EEXIST) {
			int err = errno;
			unlink(tmp);
			die("cannot write %s: %s", path, strerror(err));
		}
	}
	unlink(tmp);
	die("too many screenshots named %s in %s", stamp, dir);
}

static void save_file(const char *file, const struct mem *png)
{
	if (strcmp(file, "-") == 0) {
		if (!write_all(STDOUT_FILENO, png->data, png->len))
			die("cannot write to stdout: %s", strerror(errno));
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
	write_temp(temp_file(dir, tmp, sizeof(tmp)), tmp, png);
	if (rename(tmp, file) < 0) {
		int err = errno;
		unlink(tmp);
		die("cannot write %s: %s", file, strerror(err));
	}
	print_path(file);
}

/* ---- the clipboard ------------------------------------------------------------------ */

static struct mem clip;
static bool clip_cancelled;

static void source_send(void *data, struct ext_data_control_source_v1 *source,
	const char *mime, int32_t fd)
{
	/* The reader may be slow: wait for it, it is our only job now. */
	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	if (strcmp(mime, "image/png") == 0)
		write_all(fd, clip.data, clip.len);
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
 * when someone pastes. So msc stays, in the background, until something else is copied.
 */
static void to_clipboard(struct mem *png)
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

	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid > 0)
		_exit(0); /* the child keeps the connection: no disconnect here */
	setsid();
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		if (null > STDERR_FILENO)
			close(null);
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
		printf("%s %dx%d%+d%+d scale %g\n", o->name, w, h, x, y,
			o->scale > 0 ? (double)o->scale : 1.0);
	}
}

int main(int argc, char *argv[])
{
	bool clipboard = false, pointer = false, list = false;
	const char *file = NULL, *only = NULL;
	double delay = 0;
	int opt;
	while ((opt = getopt(argc, argv, "cf:o:d:plvh")) != -1) {
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
			if (*end || delay < 0 || delay > 3600) {
				fprintf(stderr, "msc: -d wants seconds, not %s\n", optarg);
				return 2;
			}
			break;
		}
		case 'p':
			pointer = true;
			break;
		case 'l':
			list = true;
			break;
		case 'v':
			printf("msc %s\n", MSC_VERSION);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (optind < argc || (clipboard && file)) {
		usage(stderr);
		return 2;
	}
	/* A reader that leaves early must not kill the clipboard. */
	signal(SIGPIPE, SIG_IGN);

	connect_display();
	if (!shm)
		die("the compositor has no wl_shm");
	read_outputs();
	if (list) {
		list_outputs();
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
			die("no monitor named %s (msc -l lists them)", only);
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

	cairo_surface_t *picture = compose(chosen, n);
	struct mem png = { 0 };
	encode_png(picture, &png);
	cairo_surface_destroy(picture);

	if (clipboard)
		to_clipboard(&png);
	else if (file)
		save_file(file, &png);
	else
		save_default(&png);
	return 0;
}
