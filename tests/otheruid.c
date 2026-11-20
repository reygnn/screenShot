/*
 * Someone else's files, for the tests: with LD_PRELOAD=build/otheruid.so, geteuid()
 * names another user, so the test's own files look like a stranger's. Making a file
 * that really belongs to someone else needs root.
 */
#include <sys/types.h>
#include <unistd.h>

uid_t geteuid(void)
{
	return getuid() + 1;
}
