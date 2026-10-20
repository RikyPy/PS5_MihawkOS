/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_MEMORY_FILE_H
#define PW_MEMORY_FILE_H
#include <stdint.h>
/* Sparse shared-section backing, created only by the memory payload. Every
 * returned descriptor holds a shared flock lease. SCM_RIGHTS and dup retain
 * that lease until the last descriptor closes. Every demand-mapped view must
 * retain a descriptor until it has flushed and unmapped its cached pages.
 * Files remain linked: closing a dirty unlinked file previously hung on PS5.
 * Collection opens a separate description, takes an exclusive nonblocking
 * lease, flushes and closes while linked, then unlinks. Busy files survive
 * owner closure and are collected on a later helper request/startup.
 * The directory is private runtime storage; no supplied files are processed. */
int pw_memory_file_alloc(const char *directory, uint64_t bytes);
/* Number removed, or -1 with errno. Busy leases are normal, never waited on. */
int pw_memory_file_collect(const char *directory);
#endif
