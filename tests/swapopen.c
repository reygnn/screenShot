/*
 * The race of a shared folder, for the tests, without racing: with
 * LD_PRELOAD=build/swapopen.so, the first open() of SWAP_PATH first renames SWAP_WITH
 * over it, as someone else could between screenShot looking at the name and opening it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void swap(const char *path)
{
	static int done;
	const char *at = getenv("SWAP_PATH"), *with = getenv("SWAP_WITH");
	if (!done && at && with && strcmp(path, at) == 0) {
		done = 1;
		rename(with, at);
	}
}

static int real_open(const char *name, const char *path, int flags, mode_t mode)
{
	int (*next)(const char *, int, ...) = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT,
		name);
	return next(path, flags, mode);
}

int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	swap(path);
	return real_open("open", path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	swap(path);
	return real_open("open64", path, flags, mode);
}
