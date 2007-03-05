/* vi: set sw=4 ts=4: */
/*
 * Mini ps implementation(s) for busybox
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *
 * Licensed under the GPL v2, see the file LICENSE in this tarball.
 */

#include "busybox.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>

int ps_main(int argc, char **argv)
{
	procps_status_t * p;
	int i, len;

#if ENABLE_FEATURE_PS_WIDE
	int terminal_width;
	int w_count = 0;

	bb_opt_complementally="-:ww";
#else
# define terminal_width 79
#endif

#if ENABLE_FEATURE_PS_WIDE
	/* handle arguments */
#if ENABLE_FEATURE_PS_WIDE
	bb_getopt_ulflags(argc, argv, "w", &w_count);
#else
	i = bb_getopt_ulflags(argc, argv, "c");
#endif
#if ENABLE_FEATURE_PS_WIDE
	/* if w is given once, GNU ps sets the width to 132,
	 * if w is given more than once, it is "unlimited"
	 */
	if(w_count) {
		terminal_width = (w_count==1) ? 132 : INT_MAX;
	} else {
		get_terminal_width_height(1, &terminal_width, NULL);
		/* Go one less... */
		terminal_width--;
	}
#endif
#endif  /* ENABLE_FEATURE_PS_WIDE */

	printf("  PID  Uid     VmSize Stat Command\n");

	while ((p = procps_scan(1)) != 0)  {
		char *namecmd = p->cmd;
		
		if(p->rss == 0)
		    len = printf("%5d %-8s        %s ", p->pid, p->user, p->state);
		else
		    len = printf("%5d %-8s %6ld %s ", p->pid, p->user, p->rss, p->state);

		i = terminal_width-len;

		if(namecmd && namecmd[0]) {
			if(i < 0)
				i = 0;
			if(strlen(namecmd) > (size_t)i)
				namecmd[i] = 0;
			printf("%s\n", namecmd);
		} else {
			namecmd = p->short_cmd;
			if(i < 2)
				i = 2;
			if(strlen(namecmd) > ((size_t)i-2))
				namecmd[i-2] = 0;
			printf("[%s]\n", namecmd);
		}
		/* no check needed, but to make valgrind happy..  */
		if (ENABLE_FEATURE_CLEAN_UP && p->cmd)
			free(p->cmd);
	}
	return EXIT_SUCCESS;
}
