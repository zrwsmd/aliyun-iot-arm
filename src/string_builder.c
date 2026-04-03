#include "string_builder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sb_reserve(StringBuilder *builder, size_t needed) {
    char *next_data;
    size_t next_capacity;

    if (builder == NULL) {
        return -1;
    }

    if (builder->capacity >= needed) {
        return 0;
    }

    next_capacity = (builder->capacity == 0) ? 256 : builder->capacity;
    while (next_capacity < needed) {
        next_capacity *= 2;
    }

    next_data = (char *)realloc(builder->data, next_capacity);
    if (next_data == NULL) {
        return -1;
    }

    builder->data = next_data;
    builder->capacity = next_capacity;
    return 0;
}

int sb_init(StringBuilder *builder, size_t initial_capacity) {
    if (builder == NULL) {
        return -1;
    }

    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;

    if (initial_capacity == 0) {
        initial_capacity = 256;
    }

    if (sb_reserve(builder, initial_capacity) != 0) {
        return -1;
    }

    builder->data[0] = '\0';
    return 0;
}

void sb_free(StringBuilder *builder) {
    if (builder == NULL) {
        return;
    }

    free(builder->data);
    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
}

int sb_append_n(StringBuilder *builder, const char *text, size_t length) {
    if (builder == NULL || text == NULL) {
        return -1;
    }

    if (sb_reserve(builder, builder->length + length + 1) != 0) {
        return -1;
    }

    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 0;
}

int sb_append(StringBuilder *builder, const char *text) {
    if (text == NULL) {
        return -1;
    }

    return sb_append_n(builder, text, strlen(text));
}

int sb_appendf(StringBuilder *builder, const char *fmt, ...) {
    va_list args;
    va_list copy;
    int needed;

    if (builder == NULL || fmt == NULL) {
        return -1;
    }

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return -1;
    }

    if (sb_reserve(builder, builder->length + (size_t)needed + 1) != 0) {
        va_end(args);
        return -1;
    }

    vsnprintf(builder->data + builder->length, builder->capacity - builder->length, fmt, args);
    builder->length += (size_t)needed;
    va_end(args);
    return 0;
}
