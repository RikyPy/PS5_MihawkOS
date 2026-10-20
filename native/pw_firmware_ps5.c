/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_firmware.h"
#include <ps5platform/kernel.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>

unsigned pw_firmware_installed(unsigned *reported)
{
    struct ps5_kernel_sw_version version;
    unsigned sdk = 0, said = 0;
    size_t size = sizeof(sdk);

    memset(&version, 0, sizeof(version));
    version.size = sizeof(version);
    if (sceKernelGetSystemSwVersion(&version) == 0) said = version.version;
    if (sysctlbyname("kern.sdk_version", &sdk, &size, NULL, 0) || size != sizeof(sdk)) sdk = 0;
    if (reported) *reported = said;
    return pw_firmware_choose(sdk, said);
}
