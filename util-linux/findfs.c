/* vi: set sw=4 ts=4: */
/*
 * Support functions for mounting devices by label/uuid
 *
 * Copyright (C) 2006 by Jason Schoon <floydpink@gmail.com>
 * Some portions cribbed from e2fsprogs, util-linux, dosfstools
 *
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <linux/ext2_fs.h>
#include "findfs.h"
#include "busybox.h"

/* TODO - There is lots of duplicated code between this and e2fsprogs that should
	  be consolidated somewhere common.
*/

/* borrowed from swapheader.h in util-linux */
struct swap_header_v1_2 {
	char	      bootbits[1024];    /* Space for disklabel etc. */
	unsigned int  version;
	unsigned int  last_page;
	unsigned int  nr_badpages;
	unsigned char uuid[16];
	char	      volume_name[16];
	unsigned int  padding[117];
	unsigned int  badpages[1];
};

/* borrowed from messy header <linux/reiserfs_fs.h> */
struct reiserfs_super_block
{
	u_char		s_block_count[4];
	u_char		s_free_blocks[4];
	u_char		s_root_block[4];
	u_char		s_journal_block[4];
	u_char		s_journal_dev[4];
	u_char		s_orig_journal_size[4];
	u_char		s_journal_trans_max[4];
	u_char		s_journal_block_count[4];
	u_char		s_journal_max_batch[4];
	u_char		s_journal_max_commit_age[4];
	u_char		s_journal_max_trans_age[4];
	u_char		s_blocksize[2];
	u_char		s_oid_maxsize[2];
	u_char		s_oid_cursize[2];
	u_char		s_state[2];
	u_char		s_magic[10];
	u_char		s_dummy1[10];
	u_char		s_version[2]; /* only valid with relocated journal */

	/* only valid in 3.6.x format --mason@suse.com */
	u_char		s_dummy2[10];
	u_char		s_uuid[16];
	u_char		s_label[16];
};
#define REISERFS_SUPER_MAGIC_STRING "ReIsErFs"
#define REISER2FS_SUPER_MAGIC_STRING "ReIsEr2Fs"
#define REISER3FS_SUPER_MAGIC_STRING "ReIsEr3Fs"
#define REISERFS_DISK_OFFSET_IN_BYTES (64 * 1024)

/* borrowed from dosfstools */
#define BOOT_SIGN 0xAA55	/* Boot sector magic number */
#define SWAP_BOOT_SIGN 0x55AA	/* Endian-swapped magic number */ 

#define ATTR_VOLUME 8

#define BOOTCODE_SIZE		448
#define BOOTCODE_FAT32_SIZE	420
#define DEF_FAT_BLKSIZE		512

struct msdos_volume_info {
  u_char		drive_number;	/* BIOS drive number */
  u_char		RESERVED;	/* Unused */
  u_char		ext_boot_sign;	/* 0x29 if fields below exist (DOS 3.3+) */
  u_char		volume_id[4];	/* Volume ID number */
  u_char		volume_label[11];/* Volume label */
  u_char		fs_type[8];	/* Typically FAT12 or FAT16 */
} __attribute__ ((packed));
struct msdos_boot_sector
{
  u_char	        boot_jump[3];	/* Boot strap short or near jump */
  u_char          system_id[8];	/* Name - can be used to special case
				   partition manager volumes */
  u_char          sector_size[2];	/* bytes per logical sector */
  u_char          cluster_size;	/* sectors/cluster */
  u_short         reserved;	/* reserved sectors */
  u_char          fats;		/* number of FATs */
  u_char          dir_entries[2];	/* root directory entries */
  u_char          sectors[2];	/* number of sectors */
  u_char          media;		/* media code (unused) */
  u_short         fat_length;	/* sectors/FAT */
  u_short         secs_track;	/* sectors per track */
  u_short         heads;		/* number of heads */
  u_long         hidden;		/* hidden sectors (unused) */
  u_long         total_sect;	/* number of sectors (if sectors == 0) */
  union {
    struct {
      struct msdos_volume_info vi;
      u_char	boot_code[BOOTCODE_SIZE];
    } __attribute__ ((packed)) _oldfat;
    struct {
      u_long	fat32_length;	/* sectors/FAT */
      u_short	flags;		/* bit 8: fat mirroring, low 4: active fat */
      u_char	version[2];	/* major, minor filesystem version */
      u_long	root_cluster;	/* first cluster in root directory */
      u_short	info_sector;	/* filesystem info sector */
      u_short	backup_boot;	/* backup boot sector */
      u_short	reserved2[6];	/* Unused */
      struct msdos_volume_info vi;
      u_char	boot_code[BOOTCODE_FAT32_SIZE];
    } __attribute__ ((packed)) _fat32;
  } __attribute__ ((packed)) fstype;
  u_short		boot_sign;
} __attribute__ ((packed));
#define fat32   fstype._fat32
#define oldfat  fstype._oldfat

struct msdos_dir_entry
{
    char        name[8], ext[3];        /* name and extension */
    u_char      attr;                   /* attribute bits */
    u_char      lcase;                  /* Case for base and extension */
    u_char      ctime_ms;               /* Creation time, milliseconds */
    u_short     ctime;                  /* Creation time */
    u_short     cdate;                  /* Creation date */
    u_short     adate;                  /* Last access date */
    u_short     starthi;                /* high 16 bits of first cl. (FAT32) */
    u_short     time, date, start;      /* time, date and first cluster */
    u_long      size;                   /* file size (in bytes) */
}__attribute__ ((packed)); 
/*****************************************************************************/

#define PROC_PARTITIONS "/proc/partitions"
#define DEVLABELDIR	"/dev"

static struct uuidCache_s {
	struct uuidCache_s *next;
	char uuid[16];
	char *label;
	char *device;
} *uuidCache = NULL;

int get_label_uuid(const char *device, char **label, char *uuid); 

static void uuidcache_addentry(char *device, char *label, char *uuid) 
{
	struct uuidCache_s *last;

	if (!uuidCache)
		last = uuidCache = xmalloc(sizeof(*uuidCache));
	else 
        {
		for (last = uuidCache; last->next; last = last->next);
		last->next = xmalloc(sizeof(*uuidCache));
		last = last->next;
	}
	last->next = NULL;
	last->device = device;
	last->label = label;
	memcpy(last->uuid, uuid, sizeof(last->uuid));
}

static void uuidcache_init(void) 
{
	char line[100], ptname[100], device[110], *tmp;
	int ma, mi, sz;
	FILE *procpt;

	if (uuidCache)
		return;

	procpt = fopen(PROC_PARTITIONS, "r");
	if (!procpt) 
		return;

	while (fgets(line, sizeof(line), procpt)) 
	{
		char uuid[16], *label = NULL;

		if (!index(line, '\n'))
			break;

		if (sscanf (line, " %d %d %d %[^\n ]",
			    &ma, &mi, &sz, ptname) != 4)
			continue;
		/* skip extended partitions (heuristic: size 1) */
		if (sz == 1)
			continue;

		/* skip entire disk (minor 0, 64, ... on ide;
		   0, 16, ... on sd) */
		/* heuristic: partition name ends in a digit */
		/* devfs has .../disc and .../part1 etc. */
		for (tmp = ptname; *tmp; tmp++);
		if (isdigit(tmp[-1])) 
                {
			sprintf(device, "%s/%s", DEVLABELDIR, ptname);
			if (!get_label_uuid(device, &label, uuid))
				uuidcache_addentry(strdup(device), label, uuid);
		}
	}
	fclose(procpt);
}

/*****************************************************************************/
static void store_uuid(char *udest, char *usrc) 
{
	if (usrc)
		memcpy(udest, usrc, 16);
	else
		memset(udest, 0, 16);
}

static void store_label(char **ldest, char *lsrc, int len) 
{
    	*ldest = bb_xstrndup(lsrc, len);
}

static int is_v1_swap_partition(int fd, char **label, char *uuid) 
{
	long sz = sysconf(_SC_PAGESIZE);
	char *buf = (char *)xmalloc(sz);
	struct swap_header_v1_2 *p = (struct swap_header_v1_2 *)buf;
	int ret = 0;

	if (lseek(fd, 0, SEEK_SET) == 0
	    && read(fd, buf, sz) == sz 
	    && !strncmp(buf+sz-10, "SWAPSPACE2", 10)
	    && p->version == 1) 
	{
		store_uuid(uuid, p->uuid);
		store_label(label, p->volume_name, 16);
		ret = 1;
	}
	free(buf);
	return ret;
}

static int is_ext_partition(int fd, char **label, char *uuid) 
{
	struct ext2_super_block e2sb;

	if (lseek(fd, 1024, SEEK_SET) == 1024
	    && read(fd, (char *) &e2sb, sizeof(e2sb)) == sizeof(e2sb)
	    && (e2sb.s_magic == EXT2_SUPER_MAGIC)) 
	{
		store_uuid(uuid, e2sb.s_uuid);
		store_label(label, e2sb.s_volume_name, sizeof(e2sb.s_volume_name));
		return 1;
	}
	return 0;
}

static int reiserfs_magic_version(const char *magic) 
{
	if (!strncmp(magic, REISERFS_SUPER_MAGIC_STRING,
		     strlen(REISERFS_SUPER_MAGIC_STRING)))
	        return 1;	
        else if (!strncmp(magic, REISER2FS_SUPER_MAGIC_STRING, 
		     strlen(REISER2FS_SUPER_MAGIC_STRING)))
		return 2;
        else if (!strncmp(magic, REISER3FS_SUPER_MAGIC_STRING, 
		     strlen(REISER3FS_SUPER_MAGIC_STRING)))
		return 3;
	
        return 0;
}

static int is_reiserfs_partition(int fd, char **label, char *uuid) 
{
	struct reiserfs_super_block reiserfssb;

	if (lseek(fd, REISERFS_DISK_OFFSET_IN_BYTES, SEEK_SET) == REISERFS_DISK_OFFSET_IN_BYTES
	    && read(fd, (char *) &reiserfssb, sizeof(reiserfssb)) == sizeof(reiserfssb)
	    && reiserfs_magic_version(reiserfssb.s_magic) > 1) 
	{
		/* Only 3.6.x format supers have labels or uuids.
	   	   Label and UUID can be set by reiserfstune -l/-u. */

		store_uuid(uuid, reiserfssb.s_uuid);
		store_label(label, reiserfssb.s_label, sizeof(reiserfssb.s_label));
		return 1;
	}
	return 0;
}

static int is_fat_partition(int fd, char **label, char *uuid)
{
	struct msdos_boot_sector fatbs;
	struct msdos_volume_info *vi;
	
	if (lseek(fd, 0, SEEK_SET) == 0 &&
            read(fd, (char *)&fatbs, sizeof(fatbs)) == sizeof(fatbs) &&
	    (fatbs.boot_sign == BOOT_SIGN || fatbs.boot_sign == SWAP_BOOT_SIGN))
	{
	 	/* Sectors per FAT will be 0 for FAT32 */
		vi = fatbs.fat_length ? &fatbs.oldfat.vi : &fatbs.fat32.vi;
		if (strncmp("NO NAME", vi->volume_label, 7))
		{
			store_uuid(uuid, 0);
			store_label(label, vi->volume_label, sizeof(vi->volume_label)); 
			return 1;
		}
		else
		{
			struct msdos_dir_entry de;

			/* A Redmond, WA company now puts the label in the root directory instead */
			lseek(fd, (DEF_FAT_BLKSIZE * (fatbs.reserved + fatbs.fat_length * fatbs.fats)), SEEK_SET);
			for (;;)
			{
				if (read(fd, (char *)&de, sizeof(de)) != sizeof(de) || de.name == 0)
					break;

				if (de.attr & ATTR_VOLUME)
				{
					store_uuid(uuid, 0);
					store_label(label, de.name, sizeof(de.name) + sizeof(de.ext));
					return 1;
				}
			}
		}
	}
	return 0;
}
	    
int get_label_uuid(const char *device, char **label, char *uuid) 
{
	int rv = 1;
	int fd;

	fd = open(device, O_RDONLY);
	if (fd < 0)
		return -1;

	if (is_reiserfs_partition(fd, label, uuid) || 
	    is_ext_partition(fd, label, uuid) || 
	    is_v1_swap_partition(fd, label, uuid) ||
	    is_fat_partition(fd, label, uuid))
	{
		/* success */
		rv = 0;
	}

	close(fd);
	return rv;
}

/*****************************************************************************/
char *mount_get_devname(char *spec, enum mount_specs m) 
{
	struct uuidCache_s *uc;
        char *tmp = NULL;

	uuidcache_init();
	uc = uuidCache;

	while (uc) 
        {
		switch (m) 
                {
		        case MOUNT_SPEC_UUID:
			        if (!memcmp(spec, uc->uuid, sizeof(uc->uuid)))
                                    tmp = uc->device;
			        break;
		        case MOUNT_SPEC_LABEL:
			        if (uc->label && !strncmp(spec, uc->label, strlen(spec)))
                                    tmp = uc->device;
			        break;
		}
		uc = uc->next;
	}

        return tmp ? bb_xstrdup(tmp) : tmp; 
}

int findfs_main(int argc, char **argv)
{
	char *tmp = NULL;

	if (argc != 2)
                bb_show_usage();                                                                                                             

	if (!strncmp(argv[1], "LABEL=", 6))
		tmp = mount_get_devname(argv[1] + 6, MOUNT_SPEC_LABEL);
	else if (!strncmp(argv[1], "UUID=", 5))
		tmp = mount_get_devname(argv[1] + 5, MOUNT_SPEC_UUID);
	else if (!strncmp(argv[1], "/dev/", 5)) {
		/* Just pass a device name right through.  This might aid in some scripts
	   	being able to call this unconditionally */

		tmp = argv[1];
	}
	else
		bb_show_usage();
		
	if (tmp) { 
        	puts(tmp);                                                                                                                   
		return 0;
	}
	return 1;
}    
