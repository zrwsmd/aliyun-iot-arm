#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <stddef.h>

int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_get_raw_value(const char *json, const char *key, char *out, size_t out_size);
int json_get_object(const char *json, const char *key, char *out, size_t out_size);
int json_get_array(const char *json, const char *key, char *out, size_t out_size);
int json_array_size(const char *array_json);
int json_array_get_item(const char *array_json, size_t index, char *out, size_t out_size);
int json_escape_string(const char *input, char *out, size_t out_size);

#endif
