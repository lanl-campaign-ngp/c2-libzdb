#ifndef C2_LIBZDB_LIST_H
#define C2_LIBZDB_LIST_H

#include <stddef.h>

typedef struct node node_t;

/* zfs defines list at sys/list_impl.h */
typedef struct c2list
{
  node_t *head;
  node_t *tail;
  size_t count;
} c2list_t;

void c2list_init (c2list_t *list);
size_t c2list_pushback (c2list_t *list, void *new_value);
node_t *c2list_head (c2list_t *list);
node_t *c2list_next (node_t *node);
void *c2list_get (node_t *node);
void c2list_fin (c2list_t *list, void (*free_value) (void *));

#endif
