/*
 * PNG helper for the tests.
 *
 *   pngcmp gen OUT W H     a picture whose quarters differ, so a turned or mirrored
 *                          copy of it is never the same
 *   pngcmp same A B        exit 0 if both have the same size and pixels
 *   pngcmp size FILE       prints WxH
 */
#include <cairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cairo_surface_t *load(const char *path)
{
	cairo_surface_t *s = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "pngcmp: cannot read %s\n", path);
		exit(1);
	}
	return s;
}

static int gen(const char *out, int w, int h)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(s);
	const double c[4][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 1 } };
	for (int i = 0; i < 4; i++) {
		cairo_set_source_rgb(cr, c[i][0], c[i][1], c[i][2]);
		cairo_rectangle(cr, (i % 2) * w / 2, (i / 2) * h / 2, w / 2 + 1, h / 2 + 1);
		cairo_fill(cr);
	}
	/* A mark off the middle in the red quarter: a mirror along the diagonal moves it. */
	cairo_set_source_rgb(cr, 1, 1, 0);
	cairo_rectangle(cr, w / 16, h / 4, w / 4, h / 16);
	cairo_fill(cr);
	cairo_destroy(cr);
	int ret = cairo_surface_write_to_png(s, out) == CAIRO_STATUS_SUCCESS ? 0 : 1;
	cairo_surface_destroy(s);
	return ret;
}

static int same(const char *a, const char *b)
{
	cairo_surface_t *sa = load(a), *sb = load(b);
	int w = cairo_image_surface_get_width(sa), h = cairo_image_surface_get_height(sa);
	if (w != cairo_image_surface_get_width(sb) || h != cairo_image_surface_get_height(sb)) {
		printf("sizes differ: %dx%d and %dx%d\n", w, h,
			cairo_image_surface_get_width(sb), cairo_image_surface_get_height(sb));
		return 1;
	}
	int stride_a = cairo_image_surface_get_stride(sa);
	int stride_b = cairo_image_surface_get_stride(sb);
	unsigned char *da = cairo_image_surface_get_data(sa);
	unsigned char *db = cairo_image_surface_get_data(sb);
	long diff = 0;
	int fx = -1, fy = -1;
	for (int y = 0; y < h; y++) {
		uint32_t *ra = (uint32_t *)(da + (size_t)y * stride_a);
		uint32_t *rb = (uint32_t *)(db + (size_t)y * stride_b);
		for (int x = 0; x < w; x++) {
			if ((ra[x] & 0xffffff) != (rb[x] & 0xffffff)) {
				if (!diff++) {
					fx = x;
					fy = y;
				}
			}
		}
	}
	if (diff) {
		printf("%ld pixels differ, the first at %d,%d\n", diff, fx, fy);
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc == 5 && strcmp(argv[1], "gen") == 0)
		return gen(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 4 && strcmp(argv[1], "same") == 0)
		return same(argv[2], argv[3]);
	if (argc == 3 && strcmp(argv[1], "size") == 0) {
		cairo_surface_t *s = load(argv[2]);
		printf("%dx%d\n", cairo_image_surface_get_width(s),
			cairo_image_surface_get_height(s));
		return 0;
	}
	fprintf(stderr, "usage: pngcmp gen OUT W H | same A B | size FILE\n");
	return 2;
}
