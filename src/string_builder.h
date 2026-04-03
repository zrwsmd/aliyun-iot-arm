#ifndef STRING_BUILDER_H
#define STRING_BUILDER_H

#include <stddef.h>

typedef struct StringBuilder {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

int sb_init(StringBuilder *builder, size_t initial_capacity);
void sb_free(StringBuilder *builder);
int sb_append(StringBuilder *builder, const char *text);
int sb_append_n(StringBuilder *builder, const char *text, size_t length);
int sb_appendf(StringBuilder *builder, const char *fmt, ...);

#endif
