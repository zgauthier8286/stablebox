/* vi: set sw=4 ts=4: */
/*
 * Utility routines.
 *
 * Copyright (C) many different people.  If you wrote this, please
 * acknowledge your work.
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "libbb.h"



/* From <linux/kd.h> */
enum { KDGKBTYPE = 0x4B33 };  /* get keyboard type */


static int open_a_console(const char *fnam)
{
	int fd;

	/* try read-write */
	fd = open(fnam, O_RDWR);

	/* if failed, try read-only */
	if (fd < 0 && errno == EACCES)
		fd = open(fnam, O_RDONLY);

	/* if failed, try write-only */
	if (fd < 0 && errno == EACCES)
		fd = open(fnam, O_WRONLY);

	return fd;
}

/*
 * Get an fd for use with kbd/console ioctls.
 * We try several things because opening /dev/console will fail
 * if someone else used X (which does a chown on /dev/console).
 */

int get_console_fd(void)
{
	int fd;

	static const char * const choise_console_names[] = {
		CONSOLE_DEV, CURRENT_VC, CURRENT_TTY
	};

	for (fd = 2; fd >= 0; fd--) {
		int fd4name;
		int choise_fd;
		char arg;

		fd4name = open_a_console(choise_console_names[fd]);
	chk_std:
		choise_fd = fd4name >= 0 ? fd4name : fd;

		arg = 0;
		if (ioctl(choise_fd, KDGKBTYPE, &arg) == 0)
			return choise_fd;
		if(fd4name >= 0) {
			close(fd4name);
			fd4name = -1;
			goto chk_std;
		}
	}

	bb_error_msg("Couldn't get a file descriptor referring to the console");
	return fd;                      /* total failure */
}
