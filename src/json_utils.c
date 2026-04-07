#include "json_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *json_skip_ws(const char *cursor) {
    while (cursor != NULL && *cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static const char *json_find_after_key(const char *json, const char *key) {
    char pattern[128];
    const char *cursor;
    size_t pattern_len;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) < 0) {
        return NULL;
    }

    pattern_len = strlen(pattern);
    cursor = json;
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        const char *after = json_skip_ws(cursor + pattern_len);
        if (after != NULL && *after == ':') {
            return json_skip_ws(after + 1);
        }
        cursor += pattern_len;
    }

    return NULL;
}

static const char *json_find_string_end(const char *cursor) {
    int escaped = 0;

    while (cursor != NULL && *cursor != '\0') {
        if (escaped) {
            escaped = 0;
        } else if (*cursor == '\\') {
            escaped = 1;
        } else if (*cursor == '"') {
            return cursor;
        }
        cursor++;
    }

    return NULL;
}

static const char *json_find_compound_end(const char *cursor, char open_char, char close_char) {
    int depth = 0;
    int in_string = 0;
    int escaped = 0;

    while (cursor != NULL && *cursor != '\0') {
        char ch = *cursor;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (ch == '\\') {
                escaped = 1;
            } else if (ch == '"') {
                in_string = 0;
            }
            cursor++;
            continue;
        }

        if (ch == '"') {
            in_string = 1;
        } else if (ch == open_char) {
            depth++;
        } else if (ch == close_char) {
            depth--;
            if (depth == 0) {
                return cursor;
            }
        }

        cursor++;
    }

    return NULL;
}

static int json_copy_trimmed(const char *start, const char *end, char *out, size_t out_size) {
    size_t length;

    if (start == NULL || end == NULL || out == NULL || out_size == 0 || end < start) {
        return -1;
    }

    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    length = (size_t)(end - start);
    if (length >= out_size) {
        return -1;
    }

    memcpy(out, start, length);
    out[length] = '\0';
    return 0;
}

static int json_copy_string_value(const char *start, const char *end, char *out, size_t out_size) {
    size_t idx = 0;
    const char *cursor = start;

    if (start == NULL || end == NULL || out == NULL || out_size == 0 || end < start) {
        return -1;
    }

    while (cursor < end) {
        char ch = *cursor++;
        if (ch == '\\' && cursor < end) {
            char esc = *cursor++;
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default: ch = esc; break;
            }
        }

        if (idx + 1 >= out_size) {
            return -1;
        }
        out[idx++] = ch;
    }

    out[idx] = '\0';
    return 0;
}

int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *cursor = json_find_after_key(json, key);
    const char *end;

    if (cursor == NULL || *cursor != '"') {
        return -1;
    }

    end = json_find_string_end(cursor + 1);
    if (end == NULL) {
        return -1;
    }

    return json_copy_string_value(cursor + 1, end, out, out_size);
}

int json_get_raw_value(const char *json, const char *key, char *out, size_t out_size) {
    const char *cursor = json_find_after_key(json, key);
    const char *end;

    if (cursor == NULL) {
        return -1;
    }

    if (*cursor == '"') {
        end = json_find_string_end(cursor + 1);
        if (end == NULL) {
            return -1;
        }
        return json_copy_string_value(cursor + 1, end, out, out_size);
    }

    if (*cursor == '{') {
        end = json_find_compound_end(cursor, '{', '}');
        if (end == NULL) {
            return -1;
        }
        return json_copy_trimmed(cursor, end + 1, out, out_size);
    }

    if (*cursor == '[') {
        end = json_find_compound_end(cursor, '[', ']');
        if (end == NULL) {
            return -1;
        }
        return json_copy_trimmed(cursor, end + 1, out, out_size);
    }

    end = cursor;
    while (*end != '\0' && *end != ',' && *end != '}' && *end != ']') {
        end++;
    }

    return json_copy_trimmed(cursor, end, out, out_size);
}

int json_get_object(const char *json, const char *key, char *out, size_t out_size) {
    const char *cursor = json_find_after_key(json, key);
    const char *end;

    if (cursor == NULL || *cursor != '{') {
        return -1;
    }

    end = json_find_compound_end(cursor, '{', '}');
    if (end == NULL) {
        return -1;
    }

    return json_copy_trimmed(cursor, end + 1, out, out_size);
}

int json_get_array(const char *json, const char *key, char *out, size_t out_size) {
    const char *cursor = json_find_after_key(json, key);
    const char *end;

    if (cursor == NULL || *cursor != '[') {
        return -1;
    }

    end = json_find_compound_end(cursor, '[', ']');
    if (end == NULL) {
        return -1;
    }

    return json_copy_trimmed(cursor, end + 1, out, out_size);
}

int json_array_size(const char *array_json) {
    const char *cursor;
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    int count = 0;
    int has_item = 0;

    if (array_json == NULL) {
        return -1;
    }

    cursor = json_skip_ws(array_json);
    if (cursor == NULL || *cursor != '[') {
        return -1;
    }

    cursor = json_skip_ws(cursor + 1);
    if (cursor == NULL || *cursor == '\0') {
        return -1;
    }
    if (*cursor == ']') {
        return 0;
    }

    while (*cursor != '\0') {
        char ch = *cursor;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (ch == '\\') {
                escaped = 1;
            } else if (ch == '"') {
                in_string = 0;
            }
            cursor++;
            continue;
        }

        if (isspace((unsigned char)ch)) {
            cursor++;
            continue;
        }

        if (ch == '"') {
            in_string = 1;
            has_item = 1;
        } else if (ch == '{' || ch == '[') {
            depth++;
            has_item = 1;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) {
                depth--;
            } else if (ch == ']') {
                if (has_item) {
                    count++;
                }
                return count;
            }
        } else if (ch == ',' && depth == 0) {
            if (has_item) {
                count++;
                has_item = 0;
            }
        } else {
            has_item = 1;
        }

        cursor++;
    }

    return -1;
}

int json_array_get_item(const char *array_json, size_t index, char *out, size_t out_size) {
    const char *cursor;
    const char *item_start;
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    size_t current_index = 0;

    if (array_json == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    cursor = json_skip_ws(array_json);
    if (cursor == NULL || *cursor != '[') {
        return -1;
    }

    cursor = json_skip_ws(cursor + 1);
    if (cursor == NULL || *cursor == '\0' || *cursor == ']') {
        return -1;
    }

    item_start = cursor;
    while (*cursor != '\0') {
        char ch = *cursor;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (ch == '\\') {
                escaped = 1;
            } else if (ch == '"') {
                in_string = 0;
            }
            cursor++;
            continue;
        }

        if (ch == '"') {
            in_string = 1;
        } else if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            if (depth > 0) {
                depth--;
            } else if (ch == ']') {
                if (current_index == index) {
                    return json_copy_trimmed(item_start, cursor, out, out_size);
                }
                return -1;
            }
        } else if (ch == ',' && depth == 0) {
            if (current_index == index) {
                return json_copy_trimmed(item_start, cursor, out, out_size);
            }
            current_index++;
            item_start = json_skip_ws(cursor + 1);
            cursor++;
            continue;
        }

        cursor++;
    }

    return -1;
}

int json_escape_string(const char *input, char *out, size_t out_size) {
    size_t idx = 0;

    if (input == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    while (*input != '\0') {
        const char *replacement = NULL;
        char ch = *input++;

        switch (ch) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default: break;
        }

        if (replacement != NULL) {
            size_t rep_len = strlen(replacement);
            if (idx + rep_len + 1 >= out_size) {
                return -1;
            }
            memcpy(out + idx, replacement, rep_len);
            idx += rep_len;
        } else {
            if (idx + 2 >= out_size) {
                return -1;
            }
            out[idx++] = ch;
        }
    }

    out[idx] = '\0';
    return 0;
}
