#pragma once

#include <stdbool.h>
#include <stdio.h>

int str_compare_versions(const char *a, const char *b);
void str_qsort(char **list, int count);

/* Write one literal POSIX-shell argument, including an empty argument.
 * Do not add quotes around the result or use it as shell syntax. */
int str_write_shell_arg(FILE *stream, const char *text);
