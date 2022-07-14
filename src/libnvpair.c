/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012 by Delphix. All rights reserved.
 * Copyright (c) 2022 Triad National Security, LLC as operator of Los Alamos
 *     National Laboratory. All rights reserved.
 */
#include "libnvpair.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Similar to nvlist_print() but handles arrays slightly differently.
 */
void
c2_dump_nvlist (nvlist_t *list, int indent, const char *zpool_name,
                vdti_t **zpool, vdi_t *vdev)
{
  nvpair_t *elem = NULL;
  boolean_t bool_value;
  nvlist_t *nvlist_value;
  nvlist_t **nvlist_array_value;
  uint_t i, count;

  if (list == NULL)
    {
      return;
    }

  while ((elem = nvlist_next_nvpair (list, elem)) != NULL)
    {
      const char *key = nvpair_name (elem);
      const size_t key_len = strlen (key);

      switch (nvpair_type (elem))
        {
        case DATA_TYPE_UINT64:
          if (indent == 3)
            {
              uint64_t value = 0;
              nvpair_value_uint64 (elem, &value);

              if ((key_len == 7) && (strncmp (key, "nparity", 7) == 0))
                {
                  vdev->nparity = value;
                }
              else if ((key_len == 6) && (strncmp (key, "ashift", 6) == 0))
                {
                  vdev->ashift = value;
                }
            }

          break;

        case DATA_TYPE_STRING:;
          char *value = NULL;
          nvpair_value_string (elem, &value);

          /* raidz and mirror show up here - if not, the zpool is striped */
          if (indent == 3)
            {
              if (key_len == 4)
                {
                  if (strncmp (key, "type", 4) == 0)
                    {
                      const size_t value_len = strlen (value);
                      if ((value_len == 5)
                          && (strncmp (value, "raidz", 5) == 0))
                        {
                          vdev->type = RAIDZ;
                        }
                      else if ((value_len == 6)
                               && (strncmp (value, "mirror", 6) == 0))
                        {
                          vdev->type = MIRROR;
                        }
                    }
                }
            }

          /* device path in child properties */
          if ((indent == 3) || /* striped zpool */
              (indent == 4))
            { /* raidz or mirror */
              if (key_len == 4)
                {
                  if (strncmp (key, "path", 4) == 0)
                    {
                      c2list_pushback (&vdev->names, value);
                    }
                }
            }
          break;

        case DATA_TYPE_NVLIST:
          (void)nvpair_value_nvlist (elem, &nvlist_value);

          /* find zpool name */
          if (indent == 0)
            {
              if (key_len == strlen (zpool_name))
                {
                  if (strncmp (key, zpool_name, strlen (zpool_name)) == 0)
                    {
                      *zpool = malloc (sizeof (vdti_t));
                      (*zpool)->name = nvpair_name (elem);
                      c2list_init (&(*zpool)->vdevs);
                      c2_dump_nvlist (nvlist_value, indent + 1, NULL, zpool,
                                      NULL);
                    }
                }
            }
          /* find vdev_tree under zpool */
          else if (indent == 1)
            {
              if ((key_len == 9) && (strncmp (key, "vdev_tree", 9) == 0))
                {
                  c2_dump_nvlist (nvlist_value, indent + 1, NULL, zpool, NULL);
                }
            }

          break;

        case DATA_TYPE_NVLIST_ARRAY:
          (void)nvpair_value_nvlist_array (elem, &nvlist_array_value, &count);

          for (i = 0; i < count; i++)
            {
              if ((key_len == 8) && (strncmp (key, "children", 8) == 0))
                {
                  if (indent == 2)
                    {
                      vdev = calloc (1, sizeof (vdi_t));
                      vdev->type = STRIPE;
                      c2list_init (&vdev->names);
                      c2list_pushback (&(*zpool)->vdevs, vdev);
                    }
                  /* if indent == 2 || indent == 3 */
                  c2_dump_nvlist (nvlist_array_value[i], indent + 1, NULL,
                                  NULL, vdev);
                }
            }
          break;

        default:
          break;
        }
    }
}
