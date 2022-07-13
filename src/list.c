#include <stdlib.h>
#include <string.h>

#include "list.h"

struct node {
    void *value;
    node_t *next;
};

void c2list_init(c2list_t *list) {
    if (list) {
        memset(list, 0, sizeof(c2list_t));
    }
}

size_t c2list_pushback(c2list_t *list, void *new_value) {
    if (!list) {
        return 0;
    }

    node_t *new_node = malloc(sizeof(node_t));
    new_node->value = new_value;
    new_node->next = NULL;

    if (!list->head) {
        list->head = new_node;
    }

    if (list->tail) {
        list->tail->next = new_node;
    }

    list->tail = new_node;
    list->count++;

    return list->count;
}

node_t *c2list_head(c2list_t *list) {
    return list?list->head:NULL;
}

node_t *c2list_next(node_t *node) {
    return node?node->next:NULL;
}

void *c2list_get(node_t *node) {
    return node?node->value:NULL;
}

void c2list_fin(c2list_t *list, void (*free_value)(void *)) {
    node_t *node = c2list_head(list);
    while (node) {
        node_t *next = c2list_next(node);
        if (free_value) {
            free_value(node->value);
        }

        free(node);
        node = next;
    }
}
