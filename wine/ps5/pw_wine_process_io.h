/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PROCESS_IO_H
#define PW_WINE_PROCESS_IO_H
#include <stddef.h>
#include <sys/types.h>
int pw_child_socketpair(int pair[2], size_t packet_size);
int pw_child_send(int fd, const void *data, size_t size, const int *fds, unsigned count);
ssize_t pw_child_recv(int fd, void *data, size_t size, int fds[2], unsigned *count);
#endif
