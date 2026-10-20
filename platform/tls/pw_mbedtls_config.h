/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mbed TLS's settings for the title's downloads (src/pw_https.c), on top of its default configuration
 * (MBEDTLS_USER_CONFIG_FILE): a TLS client and what it needs, nothing that touches the platform on its own. */
#undef MBEDTLS_NET_C                  /* its sockets ask getaddrinfo, a null import in the title; pw_https has its own */
#undef MBEDTLS_TIMING_C               /* unused */
#undef MBEDTLS_PSA_ITS_FILE_C         /* would keep key files in the working directory */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C                /* its messages would carry the build's paths into the title */
#define MBEDTLS_NO_PLATFORM_ENTROPY   /* its FreeBSD source is unproven on the console: pw_https.c gives one */
#define MBEDTLS_ENTROPY_HARDWARE_ALT
