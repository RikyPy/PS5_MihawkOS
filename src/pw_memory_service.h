/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_MEMORY_SERVICE_H
#define PW_MEMORY_SERVICE_H
/* A helper process's memory, copied for the title. Every Windows program but the title's own runs in a helper, and
 * the wineserver runs in the title, so the server cannot reach a helper's memory itself: ReadProcessMemory and
 * WriteProcessMemory into another process, and into a process's own (Wine sends both to the server), were refused
 * (remote-memory-probe-2026-10-06-01). Chromium's sandbox, crash reporters, launchers and hooking libraries all
 * make those calls.
 *
 * Each helper serves its own memory on one end of a socket and gives the title the other end. A request is a
 * struct pw_memory_copy, followed for a write by its bytes; the answer a struct pw_memory_copied, followed for a
 * read by the bytes read. Copies go through a pipe, so memory not readable or not writable all the way makes the
 * kernel refuse the copy (EFAULT) instead of the helper taking the fault: the answer then says how far it got. */
#include <stdint.h>

#define PW_MEMORY_SERVICE_MAGIC 0x504d5331u   /* "PMS1" */
enum { PW_MEMORY_COPY_READ = 1, PW_MEMORY_COPY_WRITE = 2, PW_MEMORY_COPY_CHUNK = 32768 };
struct pw_memory_copy { uint32_t magic, kind; uint64_t address; uint32_t size, reserved; };
struct pw_memory_copied { uint32_t magic, status; uint32_t done, reserved; };

/* The helper: start serving this process's memory on a thread. *title_end is the socket end to give the title.
 * 0, or -1 (errno set). */
int pw_memory_service_start(int *title_end);
/* The title: copy size bytes between buffer and address in the serving process (writing: into it), over fd.
 * 0 with *done bytes copied (short when its memory was not readable or writable all the way), or -1 when the
 * service does not answer within timeout_ms. One caller at a time per fd. */
int pw_memory_service_copy(int fd, int writing, uint64_t address, void *buffer, uint32_t size, uint32_t *done,
                           int timeout_ms);
/* For the tests: copy count bytes from `from` to `to` through the pipe; the bytes copied. */
/* The caller owns an empty pipe, with a nonblocking writer. Each partial
 * write is drained before continuing; pipe capacity never limits copy size.
 * Unreadable/unwritable boundaries return the actual completed prefix. */
uint32_t pw_memory_pipe_copy(int pipe_fds[2], char *to, const char *from, uint32_t count);
#endif
