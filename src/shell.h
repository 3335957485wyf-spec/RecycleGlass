/* Recycle Bin shell operations: query / empty / open / send-to-bin. */
#ifndef RG_SHELL_H
#define RG_SHELL_H

#include "common.h"

void shell_query(long long *count, long long *size);
BOOL shell_empty(BOOL confirm);
void shell_open_bin(void);

/* Send a list of filesystem paths to the recycle bin.
 * On success *message receives the user-facing toast text. */
BOOL shell_recycle_paths(wchar_t **paths, int n, wchar_t *message, size_t msgcap);

void shell_format_size(long long n, wchar_t *out, size_t cap);
void shell_format_count(long long n, wchar_t *out, size_t cap);

#endif /* RG_SHELL_H */
