#ifndef C2_LIBZDB_LIBNVPAIR_H
#define C2_LIBZDB_LIBNVPAIR_H

#include "list.h"

#include <sys/nvpair.h>

typedef enum {
	STRIPE,
	RAIDZ,
	MIRROR,
} zpool_type_t;

/* single vdev */
typedef struct vdev_info {
	c2list_t names;
	zpool_type_t type;
	size_t nparity;
	size_t ashift;
} vdi_t;

/* entire zpool */
typedef struct vdev_tree_info {
	char *name;
	c2list_t vdevs;
} vdti_t;

void c2_dump_nvlist(nvlist_t *list, int indent, const char *zpool_name,
    vdti_t **zpool, vdi_t *vdev);

#endif
