/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Preserve Wine's symbols using the canonical graphics packet implementation. */
#define ps5_gpu_packet_socketpair pw_child_socketpair
#define ps5_gpu_packet_send pw_child_send
#define ps5_gpu_packet_recv pw_child_recv
#include "../../.deps/graphics-foundation/tooling/gpu-broker/packet_io.c"
