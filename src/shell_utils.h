#ifndef SHELL_UTILS_H
#define SHELL_UTILS_H

#include <stddef.h>

#include "string_builder.h"

int shell_quote(const char *input, char *out, size_t out_size);
int ensure_directory(const char *path);
int remove_tree(const char *path);
int run_command_capture(const char *command, const char *workdir, int timeout_sec, StringBuilder *output, int *exit_code);
int start_background_command(const char *command, const char *workdir, int confirm_sec, StringBuilder *output, int *started_in_background, int *exit_code, int *child_pid);

#endif
