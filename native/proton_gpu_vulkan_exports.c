/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance,const char *);
extern PFN_vkVoidFunction vk_common_GetDeviceProcAddr(VkDevice,const char *);
extern int proton_gpu_client_bind(int);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int bound;
static void bind_broker(void)
{
    const char *text=getenv("WINE_PS5_GPU_FD");char *end;
    if(!text || !*text)return;
    long fd=strtol(text,&end,10);
    if(*end || fd<0 || fd>INT_MAX)return;
    bound=proton_gpu_client_bind((int)fd)==0;
}
/* A helper without the GPU broker (the title layout's helpers are headless): a Vulkan loader that finds no driver.
 * The global commands must still exist, since the loader's contract has them, and win32u calls them without a check
 * the first time a process reads its display configuration. With none, that call faulted inside a once-only
 * initializer that could then never finish, and the process's next display query (a window, COM's apartment)
 * waited forever: msiexec stopped at an installer's shortcuts. */
static VKAPI_ATTR VkResult VKAPI_CALL no_extensions(const char *layer,uint32_t *count,VkExtensionProperties *properties)
{
    (void)properties;
    *count=0;
    return layer ? VK_ERROR_LAYER_NOT_PRESENT : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL no_layers(uint32_t *count,VkLayerProperties *properties)
{
    (void)properties;
    *count=0;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL instance_version(uint32_t *version)
{
    *version=VK_API_VERSION_1_0;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL no_instance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *allocator,
                                                  VkInstance *instance)
{
    (void)info;(void)allocator;(void)instance;
    return VK_ERROR_INCOMPATIBLE_DRIVER;
}
static PFN_vkVoidFunction no_driver(const char *name)
{
    if(!name)return NULL;
    if(!strcmp(name,"vkEnumerateInstanceExtensionProperties"))return (PFN_vkVoidFunction)no_extensions;
    if(!strcmp(name,"vkEnumerateInstanceLayerProperties"))return (PFN_vkVoidFunction)no_layers;
    if(!strcmp(name,"vkEnumerateInstanceVersion"))return (PFN_vkVoidFunction)instance_version;
    if(!strcmp(name,"vkCreateInstance"))return (PFN_vkVoidFunction)no_instance;
    return NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name)
{
    pthread_once(&once,bind_broker);
    if(bound)return vk_icdGetInstanceProcAddr(instance,name);
    return instance ? NULL : no_driver(name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name)
{
    pthread_once(&once,bind_broker);
    return bound ? vk_common_GetDeviceProcAddr(device,name) : NULL;
}

/* Bind only a surface created by this native WSI to Wine's full HWND lifetime.
 * This export is present only in the window-capable Vulkan client PRX. */
extern int wsi_videoout_bind_window(VkSurfaceKHR,uint32_t,uint64_t);
int pw_gpu_vulkan_bind_window(VkSurfaceKHR surface,uint32_t hwnd,uint64_t generation)
{
    pthread_once(&once,bind_broker);
    return bound ? wsi_videoout_bind_window(surface,hwnd,generation) : -1;
}
