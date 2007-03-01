#ifndef __MOUNT_SPEC_H
#define __MOUNT_SPEC_H

enum mount_specs {
    MOUNT_SPEC_UUID,
    MOUNT_SPEC_LABEL
};

char *mount_get_devname(char *spec, enum mount_specs m);

#endif
