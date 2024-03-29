/* vi: set sw=4 ts=4: */
/*  nc: mini-netcat - built from the ground up for LRP
    Copyright (C) 1998  Charles P. Wright

    0.0.1   6K      It works.
    0.0.2   5K      Smaller and you can also check the exit condition if you wish.
    0.0.3           Uses select()

    19980918 Busy Boxed! Dave Cinege
    19990512 Uses Select. Charles P. Wright
    19990513 Fixes stdin stupidity and uses buffers.  Charles P. Wright

    Licensed under GPLv2, see file LICENSE in this tarball for details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include "busybox.h"

static void timeout(int signum)
{
	bb_error_msg_and_die("Timed out");
}

int nc_main(int argc, char **argv)
{
	int do_listen = 0, lport = 0, delay = 0, wsecs = 0, tmpfd, opt, sfd, x;

#ifdef CONFIG_NC_GAPING_SECURITY_HOLE
	char *pr00gie = NULL;
#endif

	struct sockaddr_in address;
	struct hostent *hostinfo;

	fd_set readfds, testfds;

	while ((opt = getopt(argc, argv, "lp:i:e:w:")) > 0) {
		switch (opt) {
			case 'l':
				do_listen++;
				break;
			case 'p':
				lport = bb_lookup_port(optarg, "tcp", 0);
				break;
			case 'i':
				delay = atoi(optarg);
				break;
#ifdef CONFIG_NC_GAPING_SECURITY_HOLE
			case 'e':
				pr00gie = optarg;
				break;
#endif
			case 'w':
				wsecs = atoi(optarg);
				break;
			default:
				bb_show_usage();
		}
	}

	if ((do_listen && optind != argc) || (!do_listen && optind + 2 != argc))
		bb_show_usage();

	sfd = bb_xsocket(AF_INET, SOCK_STREAM, 0);
	x = 1;
	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof (x)) == -1)
		bb_perror_msg_and_die("reuseaddr");
	address.sin_family = AF_INET;

	if (wsecs) {
		signal(SIGALRM, timeout);
		alarm(wsecs);
	}

	if (lport != 0) {
		memset(&address.sin_addr, 0, sizeof(address.sin_addr));
		address.sin_port = lport;

		bb_xbind(sfd, (struct sockaddr *) &address, sizeof(address));
	}

	if (do_listen) {
		socklen_t addrlen = sizeof(address);

		bb_xlisten(sfd, 1);
		if ((tmpfd = accept(sfd, (struct sockaddr *) &address, &addrlen)) < 0)
			bb_perror_msg_and_die("accept");

		close(sfd);
		sfd = tmpfd;
	} else {
		hostinfo = xgethostbyname(argv[optind]);

		address.sin_addr = *(struct in_addr *) *hostinfo->h_addr_list;
		address.sin_port = bb_lookup_port(argv[optind+1], "tcp", 0);

		if (connect(sfd, (struct sockaddr *) &address, sizeof(address)) < 0)
			bb_perror_msg_and_die("connect");
	}

	if (wsecs) {
		alarm(0);
		signal(SIGALRM, SIG_DFL);
	}

#ifdef CONFIG_NC_GAPING_SECURITY_HOLE
	/* -e given? */
	if (pr00gie) {
		dup2(sfd, 0);
		close(sfd);
		dup2(0, 1);
		dup2(0, 2);
		execl(pr00gie, pr00gie, NULL);
		/* Don't print stuff or it will go over the wire.... */
		_exit(-1);
	}
#endif /* CONFIG_NC_GAPING_SECURITY_HOLE */

	FD_ZERO(&readfds);
	FD_SET(sfd, &readfds);
	FD_SET(STDIN_FILENO, &readfds);

	while (1) {
		int fd;
		int ofd;
		int nread;

		testfds = readfds;

		if (select(FD_SETSIZE, &testfds, NULL, NULL, NULL) < 0)
			bb_perror_msg_and_die("select");

		for (fd = 0; fd < FD_SETSIZE; fd++) {
			if (FD_ISSET(fd, &testfds)) {
				if ((nread = safe_read(fd, bb_common_bufsiz1,
					sizeof(bb_common_bufsiz1))) < 0)
				{
					bb_perror_msg_and_die(bb_msg_read_error);
				}

				if (fd == sfd) {
					if (nread == 0)
						exit(0);
					ofd = STDOUT_FILENO;
				} else {
					if (nread <= 0) {
						shutdown(sfd, 1 /* send */ );
						close(STDIN_FILENO);
						FD_CLR(STDIN_FILENO, &readfds);
					}
					ofd = sfd;
				}

				if (bb_full_write(ofd, bb_common_bufsiz1, nread) < 0)
					bb_perror_msg_and_die(bb_msg_write_error);
				if (delay > 0) {
					sleep(delay);
				}
			}
		}
	}
}
