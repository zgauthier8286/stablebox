/* dhcpc.h */
#ifndef _DHCPC_H
#define _DHCPC_H

/* grab define DEFAULT_SCRIPT */
#include "libbb_udhcp.h"

#define INIT_SELECTING	0
#define REQUESTING	1
#define BOUND		2
#define RENEWING	3
#define REBINDING	4
#define INIT_REBOOT	5
#define RENEW_REQUESTED 6
#define RELEASED	7


struct client_config_t {
	char foreground;		/* Do not fork */
	char quit_after_lease;		/* Quit after obtaining lease */
	char abort_if_no_lease;		/* Abort if no lease */
	char background_if_no_lease;	/* Fork to background if no lease */
	char *interface;		/* The name of the interface to use */
	char *pidfile;			/* Optionally store the process ID */
	char *script;			/* User script to run at dhcp events */
	char *vendor_options;		/* File to store any vendor-specific options */ 
	uint8_t *clientid;		/* Optional client id to use */
	uint8_t *vendorclass;		/* Optional vendor class-id to use */
	uint8_t *hostname;		/* Optional hostname to use */
	uint8_t *fqdn;			/* Optional fully qualified domain name to use */
	char servername[64 + 1];	/* Server name to compare against sname -- specify the server we like */
	int ifindex;			/* Index number of the interface to use */
	int retries;			/* Max number of request packets */        
	int timeout;			/* Number of seconds to try to get a lease */
	uint8_t arp[6];			/* Our arp address */
};

extern struct client_config_t client_config;


#endif
