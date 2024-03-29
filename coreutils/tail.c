/* vi: set sw=4 ts=4: */
/*
 * Mini tail implementation for busybox
 *
 * Copyright (C) 2001 by Matt Kraai <kraai@alumni.carnegiemellon.edu>
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 */

/* BB_AUDIT SUSv3 compliant (need fancy for -c) */
/* BB_AUDIT GNU compatible -c, -q, and -v options in 'fancy' configuration. */
/* http://www.opengroup.org/onlinepubs/007904975/utilities/tail.html */

/* Mar 16, 2003      Manuel Novoa III   (mjn3@codepoet.org)
 *
 * Pretty much rewritten to fix numerous bugs and reduce realloc() calls.
 * Bugs fixed (although I may have forgotten one or two... it was pretty bad)
 * 1) mixing printf/write without fflush()ing stdout
 * 2) no check that any open files are present
 * 3) optstring had -q taking an arg
 * 4) no error checking on write in some cases, and a warning even then
 * 5) q and s interaction bug
 * 6) no check for lseek error
 * 7) lseek attempted when count==0 even if arg was +0 (from top)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "busybox.h"

static const struct suffix_mult tail_suffixes[] = {
	{ "b", 512 },
	{ "k", 1024 },
	{ "m", 1048576 },
	{ NULL, 0 }
};

static int status;

static void tail_xprint_header(const char *fmt, const char *filename)
{
	/* If we get an output error, there is really no sense in continuing. */
	if (dprintf(STDOUT_FILENO, fmt, filename) < 0) {
		bb_perror_nomsg_and_die();
	}
}

/* len should probably be size_t */
static void tail_xbb_full_write(const char *buf, size_t len)
{
	/* If we get a write error, there is really no sense in continuing. */
	if (bb_full_write(STDOUT_FILENO, buf, len) < 0) {
		bb_perror_nomsg_and_die();
	}
}

static ssize_t tail_read(int fd, char *buf, size_t count)
{
	ssize_t r;
	off_t current,end;
	struct stat sbuf;

	end = current = lseek(fd, 0, SEEK_CUR);
	if (!fstat(fd, &sbuf) && sbuf.st_size)
		end = sbuf.st_size;
	lseek(fd, end < current ? 0 : current, SEEK_SET);
	if ((r = safe_read(fd, buf, count)) < 0) {
		bb_perror_msg(bb_msg_read_error);
		status = EXIT_FAILURE;
	}

	return r;
}

static const char tail_opts[] =
	"fn:c:"
#if ENABLE_FEATURE_FANCY_TAIL
	"qs:v"
#endif
	;

static const char header_fmt[] = "\n==> %s <==\n";

int tail_main(int argc, char **argv)
{
	long count = 10;
	unsigned int sleep_period = 1;
	int from_top = 0;
	int follow = 0;
	int header_threshhold = 1;
	int count_bytes = 0;

	char *tailbuf;
	size_t tailbufsize;
	int taillen = 0;
	int newline = 0;

	int *fds, nfiles, nread, nwrite, seen, i, opt;
	char *s, *buf;
	const char *fmt;

#if !ENABLE_DEBUG_YANK_SUSv2 || ENABLE_FEATURE_FANCY_TAIL
	/* Allow legacy syntax of an initial numeric option without -n. */
	if (argc >=2 && ((argv[1][0] == '+') || ((argv[1][0] == '-')
			/* && (isdigit)(argv[1][1]) */
			&& (((unsigned int)(argv[1][1] - '0')) <= 9))))
	{
		optind = 2;
		optarg = argv[1];
		goto GET_COUNT;
	}
#endif

	while ((opt = getopt(argc, argv, tail_opts)) > 0) {
		switch (opt) {
			case 'f':
				follow = 1;
				break;
			case 'c':
				count_bytes = 1;
				/* FALLS THROUGH */
			case 'n':
#if !ENABLE_DEBUG_YANK_SUSv2 || ENABLE_FEATURE_FANCY_TAIL
			GET_COUNT:
#endif
				count = bb_xgetlarg10_sfx(optarg, tail_suffixes);
				/* Note: Leading whitespace is an error trapped above. */
				if (*optarg == '+') {
					from_top = 1;
				} else {
					from_top = 0;
				}
				if (count < 0) {
					count = -count;
				}
				break;
#if ENABLE_FEATURE_FANCY_TAIL
			case 'q':
				header_threshhold = INT_MAX;
				break;
			case 's':
				sleep_period =bb_xgetularg10_bnd(optarg, 0, UINT_MAX);
				break;
			case 'v':
				header_threshhold = 0;
				break;
#endif
			default:
				bb_show_usage();
		}
	}

	/* open all the files */
	fds = (int *)xmalloc(sizeof(int) * (argc - optind + 1));

	argv += optind;
	nfiles = i = 0;

	if ((argc -= optind) == 0) {
		struct stat statbuf;

		if (!fstat(STDIN_FILENO, &statbuf) && S_ISFIFO(statbuf.st_mode)) {
			follow = 0;
		}
		/* --argv; */
		*argv = (char *) bb_msg_standard_input;
		goto DO_STDIN;
	}

	do {
		if ((argv[i][0] == '-') && !argv[i][1]) {
		DO_STDIN:
			fds[nfiles] = STDIN_FILENO;
		} else if ((fds[nfiles] = open(argv[i], O_RDONLY)) < 0) {
			bb_perror_msg("%s", argv[i]);
			status = EXIT_FAILURE;
			continue;
		}
		argv[nfiles] = argv[i];
		++nfiles;
	} while (++i < argc);

	if (!nfiles) {
		bb_error_msg_and_die("no files");
	}

	tailbufsize = BUFSIZ;

	/* tail the files */
	if (from_top < count_bytes) {	/* Each is 0 or 1, so true iff 0 < 1. */
		/* Hence, !from_top && count_bytes */
		if (tailbufsize < count) {
			tailbufsize = count + BUFSIZ;
		}
	}

	buf = tailbuf = xmalloc(tailbufsize);

	fmt = header_fmt + 1;	/* Skip header leading newline on first output. */
	i = 0;
	do {
		/* Be careful.  It would be possible to optimize the count-bytes
		 * case if the file is seekable.  If you do though, remember that
		 * starting file position may not be the beginning of the file.
		 * Beware of backing up too far.  See example in wc.c.
		 */
		if ((!(count|from_top)) && (lseek(fds[i], 0, SEEK_END) >= 0)) {
			continue;
		}

		if (nfiles > header_threshhold) {
			tail_xprint_header(fmt, argv[i]);
			fmt = header_fmt;
		}

		buf = tailbuf;
		taillen = 0;
		seen = 1;
		newline = 0;

		while ((nread = tail_read(fds[i], buf, tailbufsize-taillen)) > 0) {
			if (from_top) {
				nwrite = nread;
				if (seen < count) {
					if (count_bytes) {
						nwrite -= (count - seen);
						seen = count;
					} else {
						s = buf;
						do {
							--nwrite;
							if ((*s++ == '\n') && (++seen == count)) {
								break;
							}
						} while (nwrite);
					}
				}
				tail_xbb_full_write(buf + nread - nwrite, nwrite);
			} else if (count) {
				if (count_bytes) {
					taillen += nread;
					if (taillen > count) {
						memmove(tailbuf, tailbuf + taillen - count, count);
						taillen = count;
					}
				} else {
					int k = nread;
					int nbuf = 0;

					while (k) {
						--k;
						if (buf[k] == '\n') {
							++nbuf;
						}
					}

					if (newline + nbuf < count) {
						newline += nbuf;
						taillen += nread;

					} else {
						int extra = 0;
						if (buf[nread-1] != '\n') {
							extra = 1;
						}

						k = newline + nbuf + extra - count;
						s = tailbuf;
						while (k) {
							if (*s == '\n') {
								--k;
							}
							++s;
						}

						taillen += nread - (s - tailbuf);
						memmove(tailbuf, s, taillen);
						newline = count - extra;
					}
					if (tailbufsize < taillen + BUFSIZ) {
						tailbufsize = taillen + BUFSIZ;
						tailbuf = xrealloc(tailbuf, tailbufsize);
					}
				}
				buf = tailbuf + taillen;
			}
		}

		if (!from_top) {
			tail_xbb_full_write(tailbuf, taillen);
		}

		taillen = 0;
	} while (++i < nfiles);

	buf = xrealloc(tailbuf, BUFSIZ);

	fmt = NULL;

	while (follow) {
		sleep(sleep_period);
		i = 0;
		do {
			if (nfiles > header_threshhold) {
				fmt = header_fmt;
			}
			while ((nread = tail_read(fds[i], buf, sizeof(buf))) > 0) {
				if (fmt) {
					tail_xprint_header(fmt, argv[i]);
					fmt = NULL;
				}
				tail_xbb_full_write(buf, nread);
			}
		} while (++i < nfiles);
	}

	return status;
}
