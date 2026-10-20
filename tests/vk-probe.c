/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vulkan from a Windows program, the ground under Zink and DXVK (docs/IA32_DESIGN.md, M4 step 3). Built as
 * vk32-probe.exe (i386, under WoW64) and vk64-probe.exe: the device the GPU broker gives the helper, host-visible
 * memory mapped into the program (below 4 GiB for a 32-bit one, which winevulkan's WoW64 side has to arrange),
 * and a fill the GPU writes into that memory, read back through the mapping. One line per step; the last is
 * "PS5_PROTON_VK_PROBE PASS" or "... FAIL <step>", which tools/run-console.py reads.
 * Built by tools/build-wine-probe.sh with clang, no C runtime. */
#include <windows.h>
#include "wine/vulkan.h"

enum { BYTES = 65536, FILL = 0xa5a5a5a5u };
int _fltused;   /* no C runtime: floating point needs the marker it would define */

static HANDLE out;
static char line[512];
static unsigned used;

static void add(const char *text)
{
    while (*text && used < sizeof(line) - 1) line[used++] = *text++;
    line[used] = 0;
}

static void add_hex(unsigned long long value)
{
    char text[19] = "0x";
    int n = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned digit = (value >> shift) & 15;
        if (digit || n || !shift) text[2 + n++] = "0123456789abcdef"[digit];
    }
    text[2 + n] = 0;
    add(text);
}

/* One line to standard output and to the debugger channel: a helper's verdict reaches the run's log through
 * +debugstr (pe32-probe.c). */
static void flush_line(void)
{
    DWORD written;
    add("\r\n");
    WriteFile(out, line, used, &written, NULL);
    OutputDebugStringA(line);
    used = 0;
    line[0] = 0;
}

static int fail(const char *step, VkResult result)
{
    add("PS5_PROTON_VK_PROBE FAIL "); add(step); add(" result="); add_hex((unsigned)result);
    flush_line();
    ExitProcess(1);
    return 1;
}

#define CHECK(step, call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) return fail(step, r_); } while (0)
#define INSTANCE_FN(name) PFN_##name name = (PFN_##name)get_proc(instance, #name)

int mainCRTStartup(void)
{
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
    PFN_vkGetInstanceProcAddr get_proc = vulkan ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr") : NULL;
    if (!get_proc) return fail("load", VK_ERROR_INITIALIZATION_FAILED);

    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)get_proc(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vk-probe", 1, NULL, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
    CHECK("instance", vkCreateInstance(&instance_info, NULL, &instance));

    INSTANCE_FN(vkEnumeratePhysicalDevices);
    INSTANCE_FN(vkGetPhysicalDeviceProperties);
    INSTANCE_FN(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE_FN(vkCreateDevice);
    INSTANCE_FN(vkGetDeviceProcAddr);
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || !count) return fail("devices", enumerated);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    add("vk bits="); add(sizeof(void *) == 4 ? "32" : "64"); add(" device="); add(properties.deviceName);
    add(" api="); add_hex(properties.apiVersion);
    flush_line();

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkQueueFamilyProperties families[16];
    uint32_t family_count = 16, family = ~0u;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (uint32_t i = 0; i < family_count && family == ~0u; i++)
        if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) family = i;
    if (family == ~0u) return fail("queue_family", VK_ERROR_FEATURE_NOT_PRESENT);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &priority };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info };
    VkDevice device = VK_NULL_HANDLE;
    CHECK("device", vkCreateDevice(physical, &device_info, NULL, &device));
#define DEVICE_FN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
    DEVICE_FN(vkGetDeviceQueue); DEVICE_FN(vkCreateBuffer); DEVICE_FN(vkGetBufferMemoryRequirements);
    DEVICE_FN(vkAllocateMemory); DEVICE_FN(vkBindBufferMemory); DEVICE_FN(vkMapMemory); DEVICE_FN(vkUnmapMemory);
    DEVICE_FN(vkCreateCommandPool); DEVICE_FN(vkAllocateCommandBuffers); DEVICE_FN(vkBeginCommandBuffer);
    DEVICE_FN(vkCmdFillBuffer); DEVICE_FN(vkEndCommandBuffer); DEVICE_FN(vkCreateFence); DEVICE_FN(vkQueueSubmit);
    DEVICE_FN(vkWaitForFences); DEVICE_FN(vkDestroyFence); DEVICE_FN(vkDestroyCommandPool);
    DEVICE_FN(vkDestroyBuffer); DEVICE_FN(vkFreeMemory); DEVICE_FN(vkDestroyDevice);
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)get_proc(instance, "vkDestroyInstance");
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, BYTES,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buffer;
    CHECK("buffer", vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == ~0u; i++)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) type = i;
    if (type == ~0u) return fail("memory_type", VK_ERROR_FEATURE_NOT_PRESENT);
    VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, requirements.size, type };
    VkDeviceMemory backing;
    CHECK("allocate", vkAllocateMemory(device, &allocate, NULL, &backing));
    CHECK("bind", vkBindBufferMemory(device, buffer, backing, 0));
    void *mapped = NULL;
    CHECK("map", vkMapMemory(device, backing, 0, BYTES, 0, &mapped));
    volatile unsigned *words = mapped;
    for (unsigned i = 0; i < BYTES / 4; i++) words[i] = 0x11111111u;

    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family };
    VkCommandPool pool;
    CHECK("pool", vkCreateCommandPool(device, &pool_info, NULL, &pool));
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer commands;
    CHECK("commands", vkAllocateCommandBuffers(device, &command_info, &commands));
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK("begin", vkBeginCommandBuffer(commands, &begin));
    vkCmdFillBuffer(commands, buffer, 0, BYTES, FILL);
    CHECK("end", vkEndCommandBuffer(commands));
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK("fence", vkCreateFence(device, &fence_info, NULL, &fence));
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &commands };
    CHECK("submit", vkQueueSubmit(queue, 1, &submit, fence));
    CHECK("wait", vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));

    unsigned filled = 0;
    for (unsigned i = 0; i < BYTES / 4; i++) filled += words[i] == FILL;
    add("vk mapped="); add_hex((unsigned long long)(ULONG_PTR)mapped); add(" filled_words="); add_hex(filled);
    add(" of "); add_hex(BYTES / 4);
    flush_line();

    vkUnmapMemory(device, backing);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, backing, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    if (filled != BYTES / 4) return fail("readback", VK_ERROR_UNKNOWN);
    add("PS5_PROTON_VK_PROBE PASS: device, mapped memory, a GPU fill read back through the mapping");
    flush_line();
    ExitProcess(0);
    return 0;
}
