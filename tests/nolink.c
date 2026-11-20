/*
 * A filesystem without hard links, as vfat or exFAT, for the tests: with
 * LD_PRELOAD=build/nolink.so, link() and linkat() fail with EPERM as they do there.
 * Mounting a real one needs root.
 */
#include <errno.h>

int link(const char *from, const char *to)
{
	(void)from;
	(void)to;
	errno = EPERM;
	return -1;
}

int linkat(int from_dir, const char *from, int to_dir, const char *to, int flags)
{
	(void)from_dir;
	(void)from;
	(void)to_dir;
	(void)to;
	(void)flags;
	errno = EPERM;
	return -1;
}
