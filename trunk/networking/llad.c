/*
 * RFC3927 ZeroConf IPv4 Link-Local addressing
 *
 * Copyright (C) 2003 by Arthur van Hoff (avh@strangeberry.com)
 * Copyright (C) 2004 by David Brownell
 * Copyright (C) 2007 by Jason Schoon (jason.schoon@intermec.com)
 *
 * Licensed under the GPLv2.
 *
 * This code was borrowed directly from the Busybox zcip applet and reduced in
 * size and complexity, becoming more robust and specific along the way.
 *
 * Here are some primary differences:
 *  - This daemon doesn't exit when the interface goes down.
 *    However, it can hang around in the poll command if an interface goes down
 *    in the middle of probing.
 *  - No script files are used.
 *  - Removal of the address is handled on exit. 
 *  - Signals are provided to restart the state machine. 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "busybox.h"

struct arp_packet {
	struct ether_header hdr;
	struct ether_arp arp;
} __attribute__((__packed__));

/* Protocol Constants */
enum {
	/* protocol timeout parameters, specified in seconds */
	PROBE_WAIT = 1,
	PROBE_MIN = 1,
	PROBE_MAX = 2,
	PROBE_NUM = 3,
	MAX_CONFLICTS = 10,
	RATE_LIMIT_INTERVAL = 60,
	/* TODO ANNOUNCE_WAIT = 2, */
	ANNOUNCE_NUM = 2,
	ANNOUNCE_INTERVAL = 2,
	DEFEND_INTERVAL = 10
};
#define LINKLOCAL_ADDR 	0xa9fe0000	/* 169.254.0.0 */
#define	IN_LINKLOCAL(a)	((((in_addr_t)(a)) & IN_CLASSB_NET) == LINKLOCAL_ADDR)

/* States during the configuration process. */
enum {
	INIT = 0,
	PROBE,
	RATE_LIMIT_PROBE,
	ANNOUNCE,
	MONITOR,
	DEFEND
};

static const struct ether_addr null_addr = {{0}};
static char *intf;
static int state = INIT;

/**
 * Broadcast an ARP packet.
 */
static int arp(int fd, struct sockaddr *saddr, unsigned short op,
	const struct ether_addr *source_addr, struct in_addr source_ip,
	const struct ether_addr *target_addr, struct in_addr target_ip)
{
	struct arp_packet p;
	memset(&p, 0, sizeof(p));

	// ether header
	p.hdr.ether_type = htons(ETHERTYPE_ARP);
	memcpy(p.hdr.ether_shost, source_addr, ETH_ALEN);
	memset(p.hdr.ether_dhost, 0xff, ETH_ALEN);

	// arp request
	p.arp.arp_hrd = htons(ARPHRD_ETHER);
	p.arp.arp_pro = htons(ETHERTYPE_IP);
	p.arp.arp_hln = ETH_ALEN;
	p.arp.arp_pln = sizeof(struct in_addr);
	p.arp.arp_op = htons(op);
	memcpy(p.arp.arp_sha, source_addr, ETH_ALEN);
	memcpy(p.arp.arp_spa, (void *)&source_ip, sizeof(source_ip));
	memcpy(p.arp.arp_tha, target_addr, ETH_ALEN);
	memcpy(p.arp.arp_tpa, (void *)&target_ip, sizeof(target_ip));

	if (sendto(fd, &p, sizeof (p), 0, saddr, sizeof (*saddr)) < 0)
		return -errno;
	return 0;
}

/**
 * Pick a random link local IP address on 169.254/16, except that
 * the first and last 256 addresses are reserved.
 */
static void pick(struct in_addr *ip)
{
    	unsigned long tmp;

	/* use cheaper math than lrand48() mod N */
	do 
	{
	    	tmp = ((unsigned long)lrand48() >> 16) & IN_CLASSB_HOST; 
	} while (tmp > (IN_CLASSB_HOST - 0x0200));
	ip->s_addr = htonl((LINKLOCAL_ADDR + 0x0100) + tmp);
}

/**
 * Return milliseconds of random delay, up to "secs" seconds.
 */
static unsigned ms_rdelay(unsigned secs)
{
	return (unsigned)(lrand48() % (long int)(secs * 1000));
}

static void addr_add(struct in_addr ip)
{
	// TODO - Do this the right way, without system()
	char buf[128];

	sprintf(buf, "ifconfig %s %s 2>/dev/null", intf, inet_ntoa(ip));
	(void)system(buf);
}

static void addr_del(void)
{
	// TODO - Do this the right way, without system()
	char buf[128];

	sprintf(buf, "ifconfig %s down 2>/dev/null", intf);
	(void)system(buf);
}

static void sighandler(int sig)
{
	if (sig == SIGTERM ||
	    sig == SIGINT ||
	    sig == SIGQUIT)
	{
		// Cleanup before exiting
		addr_del();
		exit(0);
	}
	else if (sig == SIGHUP ||
		 sig == SIGUSR1 ||
	 	 sig == SIGUSR2)
	{
		// Restart the process
		addr_del();
		state = INIT;
	}
}

static void setup_signals(void)
{
	struct sigaction action;

	(void)memset((void *) &action, 0, sizeof(action));
	action.sa_handler = sighandler;

	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGQUIT, &action, NULL);
	(void)sigaction(SIGHUP, &action, NULL);
	(void)sigaction(SIGUSR1, &action, NULL);
	(void)sigaction(SIGUSR2, &action, NULL);
}

int llad_main(int argc, char **argv)
{
	struct sockaddr saddr;
	struct ether_addr addr;
	int fd;
	suseconds_t timeout = 0;	// milliseconds
	unsigned conflicts = 0;
	unsigned nprobes = 0;
	unsigned nclaims = 0;
	int t;
	struct ifreq ifr;
	unsigned short seed[3];
	struct in_addr ip = {0};
	char physical_intf[IFNAMSIZ] = {0}, *tmp;

	while ((t = getopt(argc, argv, "r:")) != EOF) 
	{
		switch (t) 
		{
			case 'r':
				if (inet_pton(AF_INET, optarg, &ip) <= 0 || 
				    !IN_LINKLOCAL(ntohl(ip.s_addr))) 
				{
					fprintf(stderr, "invalid link address\n");
					bb_show_usage();
				}
				break;
			default:
				fprintf(stderr, "bad option specified\n");
				bb_show_usage();
				break;
		}
	}
	if (optind < argc)
		intf = argv[optind++];
	if (optind != argc || !intf)
		bb_show_usage();

	setup_signals();

	fd = bb_xsocket(PF_PACKET, SOCK_PACKET, htons(ETH_P_ARP));

	tmp = strrchr(intf, ':');
	if (tmp)
		safe_strncpy(physical_intf, intf, tmp - intf + 1);
	else
		safe_strncpy(physical_intf, intf, IFNAMSIZ);
	memset(&saddr, 0, sizeof (saddr));
	safe_strncpy(saddr.sa_data, physical_intf, sizeof (saddr.sa_data));
	bb_xbind(fd, &saddr, sizeof (saddr));

	// get the interface's ethernet address
	memset(&ifr, 0, sizeof (ifr));
	safe_strncpy(ifr.ifr_name, physical_intf, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) 
	{
		syslog(LOG_ERR, "%s - %s", intf, strerror(errno));
		return EXIT_FAILURE;
	}
	memcpy(addr.ether_addr_octet, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

	/* start with some stable ip address, either a function of
		the hardware address or else the last address we used.
		NOTE: the sequence of addresses we try changes only
		depending on when we detect conflicts.
	*/
	memcpy(seed, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
	(void)seed48(seed);
	if (ip.s_addr == 0)
		pick(&ip);

	/* run the dynamic address negotiation protocol,
	   restarting after address conflicts:
	  - start with some address we want to try
	  - short random delay
	  - arp probes to see if another host else uses it
	  - arp announcements that we're claiming it
	  - use it
	  - defend it, within limits
	*/
	for (;;)
	{
		struct pollfd fds[1];
		struct timeval tv1;
		struct arp_packet p;
		int source_ip_conflict = 0;
		int target_ip_conflict = 0;

		fds[0].fd = fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;

		// poll, being ready to adjust current timeout
		if (!timeout) 
		{
			timeout = (suseconds_t)ms_rdelay(PROBE_WAIT);

			// TODO setsockopt(fd, SO_ATTACH_FILTER, ...) to
			// make the kernel filter out all packets except
			// ones we'd care about.
		}

		// set tv1 to the point in time when we timeout
		(void)gettimeofday(&tv1, NULL);
		tv1.tv_usec += (timeout % 1000) * 1000;
		while (tv1.tv_usec > 1000000) 
		{
			tv1.tv_usec -= 1000000;
			tv1.tv_sec++;
		}
		tv1.tv_sec += timeout / 1000;
	
		switch (poll(fds, 1, timeout)) 
		{
			case 0:	// timeout
				switch (state) 
				{
					case PROBE:
						// timeouts in the PROBE state means no conflicting ARP packets
						// have been received, so we can progress through the states
						if (nprobes < PROBE_NUM) 
						{
						    	struct in_addr null_ip = {INADDR_ANY};

							nprobes++;
							(void)arp(fd, &saddr, ARPOP_REQUEST, &addr, null_ip, &null_addr, ip);
							timeout = PROBE_MIN * 1000;
							timeout += (suseconds_t)ms_rdelay(PROBE_MAX - PROBE_MIN);
						}
						else 
						{
							// Switch to announce state.
							state = ANNOUNCE;
							nclaims = 0;
							(void)arp(fd, &saddr, ARPOP_REQUEST, &addr, ip, &addr, ip);
							timeout = ANNOUNCE_INTERVAL * 1000;
						}
						break;
					case RATE_LIMIT_PROBE:
						// timeouts in the RATE_LIMIT_PROBE state means no conflicting ARP packets
						// have been received, so we can move immediately to the announce state
						state = ANNOUNCE;
						nclaims = 0;
						(void)arp(fd, &saddr, ARPOP_REQUEST, &addr, ip, &addr, ip);
						timeout = ANNOUNCE_INTERVAL * 1000;
						break;
					case ANNOUNCE:
						// timeouts in the ANNOUNCE state means no conflicting ARP packets
						// have been received, so we can progress through the states
						if (nclaims < ANNOUNCE_NUM) 
						{
							nclaims++;
							(void)arp(fd, &saddr, ARPOP_REQUEST, &addr, ip, &addr, ip);
							timeout = ANNOUNCE_INTERVAL * 1000;
						}
						else 
						{
							// Switch to monitor state.
							state = MONITOR;
		
							// TODO update filters
							addr_add(ip);

							conflicts = 0;
							timeout = -1; // Never timeout in the monitor state.
						}
						break;
					case DEFEND:
						// We won!  No ARP replies, so just go back to monitor.
						state = MONITOR;
						timeout = -1;
						conflicts = 0;
						break;
					default:
						// Invalid, should never happen.  Restart the whole protocol.
						pick(&ip);
						/* FALLTHRU */
					case INIT:
						timeout = 0;
						nprobes = 0;
						nclaims = 0;
						state = PROBE;
						break;
				}
				break;
			case 1:	// packets arriving
				// We need to adjust the timeout in case we didn't receive
				// a conflicting packet.
				if (timeout > 0) 
				{
					struct timeval tv2;
	
					(void)gettimeofday(&tv2, NULL);
					if (timercmp(&tv1, &tv2, <)) 
					{
						// Current time is greater than the expected timeout time.
						// Should never happen.
						timeout = 0;
					} 
					else 
					{
					    	/*lint -e717*/
						timersub(&tv1, &tv2, &tv1);
						/*lint +e717*/
						timeout = 1000 * tv1.tv_sec + tv1.tv_usec / 1000;
					}
				}

				if ((fds[0].revents & POLLIN) == 0) 
				{
					if (fds[0].revents & POLLERR) 
					{
						// link down
						(void)usleep(100000);
					}
					continue;
				}
	
				// read ARP packet
				if (recv(fd, &p, 0, 0) < 0 ||
				    p.hdr.ether_type != htons(ETHERTYPE_ARP) ||
				    (p.arp.arp_op != htons(ARPOP_REQUEST) && 
 				     p.arp.arp_op != htons(ARPOP_REPLY)))
				{
					continue;
				}
	
				if (memcmp(p.arp.arp_spa, &ip.s_addr, sizeof(struct in_addr)) == 0 &&
				    memcmp(&addr, p.arp.arp_sha, ETH_ALEN) != 0) 
				{
					source_ip_conflict = 1;
				}
				if (memcmp(p.arp.arp_tpa, &ip.s_addr, sizeof(struct in_addr)) == 0 &&
				    p.arp.arp_op == htons(ARPOP_REQUEST) &&
				    memcmp(&addr, p.arp.arp_tha, ETH_ALEN) != 0) 
				{
					target_ip_conflict = 1;
				}
	
				switch (state) 
				{
					case PROBE:
					case ANNOUNCE:
						// When probing or announcing, check for source IP conflicts
						// and other hosts doing ARP probes (target IP conflicts).
						if (source_ip_conflict || target_ip_conflict) 
						{
							conflicts++;
							if (conflicts >= MAX_CONFLICTS) 
							{
								timeout = RATE_LIMIT_INTERVAL * 1000;
								state = RATE_LIMIT_PROBE;
							}

							pick(&ip);
							state = INIT;
						}
						break;
					case MONITOR:
						// If a conflict, we try to defend with a single ARP probe.
						if (source_ip_conflict) 
						{
							state = DEFEND;
							timeout = DEFEND_INTERVAL * 1000;
							(void)arp(fd, &saddr, ARPOP_REQUEST, &addr, ip, &addr, ip);
						}
						break;
					case DEFEND:
						// Well, we tried.  Start over (on conflict).
						if (source_ip_conflict) 
						{
							addr_del();

							pick(&ip);
							state = INIT;
						}
						break;
					default:
						// Invalid, should never happen.  Restart the whole protocol.
						pick(&ip);
						/* FALLTHRU */
					case INIT:
						timeout = 0;
						nprobes = 0;
						nclaims = 0;
						state = PROBE;
						break;
				}
				break;
			default:
			case -1:	// error
				(void)usleep(100000);
				break;
		}
	}
	return EXIT_SUCCESS;
}
