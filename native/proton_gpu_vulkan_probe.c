/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic offscreen images only; optional triangle, no display or timing queries. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radv_ps5_platform.h"
#ifdef PROTON_GPU_DRAW
#include "proton_gpu_probe_spv.h"
#endif
extern PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance,const char *);
extern int proton_gpu_client_bind(int);
extern int ps5_gpu_client_image(unsigned,unsigned,uint64_t);
extern int ps5_gpu_client_coherence(unsigned,unsigned,uint64_t);
extern int ps5_gpu_client_timing(unsigned,unsigned,uint64_t);
extern int ps5_gpu_client_done(int,uint64_t,uint64_t);
int proton_gpu_vulkan_probe(int fd,unsigned client);
#define V(name) ((PFN_vk##name)vk_icdGetInstanceProcAddr(instance,"vk" #name))
#define CHECK(call) do { VkResult result=(call); if(result!=VK_SUCCESS) { \
    fprintf(stderr,"gpu-vulkan client=%u cycle=%u stage=%s status=%d\n",client,cycle,#call,result);goto cleanup; } } while(0)
static uint32_t memory_type(VkPhysicalDevice physical,uint32_t bits,VkMemoryPropertyFlags required,VkInstance instance)
{
    VkPhysicalDeviceMemoryProperties properties;
    V(GetPhysicalDeviceMemoryProperties)(physical,&properties);
    for(uint32_t i=0;i<properties.memoryTypeCount;++i)
        if((bits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&required)==required)return i;
    return UINT32_MAX;
}
/* Coherence (risk 5 of the broker review): no flush, no invalidate, no cache maintenance of any kind. The CPU writes a
 * pattern into a host-visible buffer with plain stores, the GPU uploads it into the image and copies the image into a
 * second host-visible buffer whose lines the CPU dirtied first (0xa5a5a5a5), and the CPU reads that. Every word must be
 * the pattern: the GPU saw the CPU's unflushed writes, and the CPU sees the GPU's writes over its own dirty lines. */
static uint32_t coherence_word(unsigned client,unsigned cycle,unsigned kind,unsigned i)
{
    uint32_t x=0x9e3779b9u*(i+1)^(client<<28)^(cycle<<24)^(kind<<20);
    x^=x>>16;x*=0x7feb352du;x^=x>>15;return x;
}
static int coherence(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,VkCommandPool pool,
                     VkImage image,unsigned client,unsigned cycle,unsigned kind,unsigned *matched,uint64_t *hash)
{
    VkBuffer buffers[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};VkDeviceMemory memory[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};
    void *maps[2]={NULL,NULL};VkFence fence=VK_NULL_HANDLE;VkCommandBuffer command=VK_NULL_HANDLE;int okay=0;
    *matched=0;*hash=1469598103934665603ull;
    VkPhysicalDeviceMemoryProperties properties;V(GetPhysicalDeviceMemoryProperties)(physical,&properties);
    const VkMemoryPropertyFlags coherent=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(unsigned n=0;n<2;++n) {
        VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*64*4,
            .usage=n ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(V(CreateBuffer)(device,&info,NULL,&buffers[n]));
        VkMemoryRequirements requirements;V(GetBufferMemoryRequirements)(device,buffers[n],&requirements);
        /* kind 0: host-visible and coherent but not cached (RADV's write-combined type); kind 1: cached as well. */
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<properties.memoryTypeCount && type==UINT32_MAX;++i) {
            const VkMemoryPropertyFlags flags=properties.memoryTypes[i].propertyFlags;
            if((requirements.memoryTypeBits&(1u<<i)) && (flags&coherent)==coherent &&
               !(flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)==!kind)type=i;
        }
        if(type==UINT32_MAX)goto cleanup;
        VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=type};
        CHECK(V(AllocateMemory)(device,&allocation,NULL,&memory[n]));
        CHECK(V(BindBufferMemory)(device,buffers[n],memory[n],0));
        CHECK(V(MapMemory)(device,memory[n],0,VK_WHOLE_SIZE,0,&maps[n]));
    }
    volatile uint32_t *upload=maps[0],*readback=maps[1];uint32_t primed=0;
    for(unsigned i=0;i<64*64;++i) { readback[i]=0xa5a5a5a5u;primed+=readback[i]; }
    for(unsigned i=0;i<64*64;++i)upload[i]=coherence_word(client,cycle,kind,i);
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(V(AllocateCommandBuffers)(device,&command_info,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(V(BeginCommandBuffer)(command,&begin));
    VkMemoryBarrier host_write={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT};
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier to_dst={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
        1,&host_write,0,NULL,1,&to_dst);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
    V(CmdCopyBufferToImage)(command,buffers[0],image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    VkImageMemoryBarrier to_src=to_dst;
    to_src.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;to_src.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;to_src.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_src);
    V(CmdCopyImageToBuffer)(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[1],1,&copy);
    VkBufferMemoryBarrier host_read={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffers[1],.size=VK_WHOLE_SIZE};
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host_read,0,NULL);
    CHECK(V(EndCommandBuffer)(command));
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(V(CreateFence)(device,&fence_info,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(V(QueueSubmit)(queue,1,&submit,fence));
    CHECK(V(WaitForFences)(device,1,&fence,VK_TRUE,5000000000ull));
    unsigned first_bad=UINT32_MAX;
    for(unsigned i=0;i<64*64;++i) {
        const uint32_t value=readback[i];*hash=(*hash^value)*1099511628211ull;
        if(value==coherence_word(client,cycle,kind,i))++*matched;else if(first_bad==UINT32_MAX)first_bad=i;
    }
    if(first_bad!=UINT32_MAX)
        fprintf(stderr,"gpu-vulkan coherence_mismatch client=%u cycle=%u kind=%u word=%u actual=%#x expected=%#x primed=%#x\n",client,cycle,
            kind,first_bad,readback[first_bad],coherence_word(client,cycle,kind,first_bad),primed);
    okay=1;
cleanup:
    if(fence)V(DestroyFence)(device,fence,NULL);
    if(command)V(FreeCommandBuffers)(device,pool,1,&command);
    for(unsigned n=0;n<2;++n) {
        if(maps[n])V(UnmapMemory)(device,memory[n]);
        if(buffers[n])V(DestroyBuffer)(device,buffers[n],NULL);
        if(memory[n])V(FreeMemory)(device,memory[n],NULL);
    }
    return okay;
}
/* Throughput (risk 2): small submissions, each a 16 KiB fill of a device buffer. Serial waits for every one; pipelined
 * keeps two in flight. Reported as total nanoseconds for the count, so the owner sees both clients' numbers side by side. */
static int throughput(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,VkCommandPool pool,
                      unsigned client,unsigned cycle)
{
    enum { count=30 };
    VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkFence fences[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};
    VkCommandBuffer commands[2]={VK_NULL_HANDLE,VK_NULL_HANDLE};int okay=0;
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16384,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(V(CreateBuffer)(device,&info,NULL,&buffer));
    VkMemoryRequirements requirements;V(GetBufferMemoryRequirements)(device,buffer,&requirements);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,
        .memoryTypeIndex=memory_type(physical,requirements.memoryTypeBits,0,instance)};
    CHECK(V(AllocateMemory)(device,&allocation,NULL,&memory));
    CHECK(V(BindBufferMemory)(device,buffer,memory,0));
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=2};
    CHECK(V(AllocateCommandBuffers)(device,&command_info,commands));
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT};
    for(unsigned n=0;n<2;++n) {
        CHECK(V(CreateFence)(device,&fence_info,NULL,&fences[n]));
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(V(BeginCommandBuffer)(commands[n],&begin));
        V(CmdFillBuffer)(commands[n],buffer,0,VK_WHOLE_SIZE,0x01010101u*(n+1));
        CHECK(V(EndCommandBuffer)(commands[n]));
    }
    for(unsigned phase=0;phase<2;++phase) {
        const unsigned depth=phase ? 2 : 1;
        const uint64_t start=radv_ps5_now_ns();
        for(unsigned i=0;i<count;++i) {
            const unsigned slot=i%depth;
            CHECK(V(WaitForFences)(device,1,&fences[slot],VK_TRUE,5000000000ull));
            CHECK(V(ResetFences)(device,1,&fences[slot]));
            VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&commands[slot]};
            CHECK(V(QueueSubmit)(queue,1,&submit,fences[slot]));
        }
        CHECK(V(WaitForFences)(device,2,fences,VK_TRUE,5000000000ull));
        if(ps5_gpu_client_timing(cycle*8+phase,count,radv_ps5_now_ns()-start))goto cleanup;
    }
    /* Where a serial submission's time goes, in this process: the submit call, the fence wait, a 1 ms sleep, and a
     * no-op round trip to the owner. */
    extern void ps5_gpu_client_submit_stats(uint64_t *,uint64_t *);
    uint64_t rpc_ns0=0,rpc_count0=0,rpc_ns1=0,rpc_count1=0;
    ps5_gpu_client_submit_stats(&rpc_ns0,&rpc_count0);
    extern uint64_t ps5_gpu_client_reply_transit(void);
    const uint64_t transit0=ps5_gpu_client_reply_transit();
    extern void ps5_gpu_client_rpc_split(uint64_t[3]);
    uint64_t split0[3],split1[3];ps5_gpu_client_rpc_split(split0);
    uint64_t submit_ns=0,wait_ns=0;
    for(unsigned i=0;i<count;++i) {
        CHECK(V(ResetFences)(device,1,&fences[0]));
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&commands[0]};
        const uint64_t t0=radv_ps5_now_ns();
        CHECK(V(QueueSubmit)(queue,1,&submit,fences[0]));
        const uint64_t t1=radv_ps5_now_ns();
        CHECK(V(WaitForFences)(device,1,&fences[0],VK_TRUE,5000000000ull));
        submit_ns+=t1-t0;wait_ns+=radv_ps5_now_ns()-t1;
    }
    ps5_gpu_client_submit_stats(&rpc_ns1,&rpc_count1);
    const uint64_t transit1=ps5_gpu_client_reply_transit();
    ps5_gpu_client_rpc_split(split1);
    extern void radv_ps5_sleep_us(unsigned);
    extern int ps5_gpu_client_ping(void);
    uint64_t t=radv_ps5_now_ns();
    for(unsigned i=0;i<20;++i)radv_ps5_sleep_us(1000);
    const uint64_t sleep_ns=radv_ps5_now_ns()-t;
    t=radv_ps5_now_ns();
    for(unsigned i=0;i<50;++i)if(ps5_gpu_client_ping())goto cleanup;
    const uint64_t ping_ns=radv_ps5_now_ns()-t;
    /* Where this process may run: the kernel's CPU set (tried at the sizes the title found, proton_wine.cpp) and the
     * CPUs this thread was seen on over 200 samples. */
    extern int sceKernelGetCurrentCpu(void);
    extern int cpuset_getaffinity(int,int,long long,size_t,void *);
    uint64_t allowed=0,seen=0;
    for(size_t bytes=8;bytes<=64 && !allowed;bytes+=8) {
        _Alignas(8) unsigned char set[64]={0};
        if(!cpuset_getaffinity(3,2,-1,bytes,set))for(unsigned i=0;i<64 && i<bytes*8;++i)if(set[i/8]&(1u<<(i%8)))allowed|=1ull<<i;
    }
    for(unsigned i=0;i<200;++i) {
        const int cpu=sceKernelGetCurrentCpu();if(cpu>=0 && cpu<64)seen|=1ull<<cpu;
        radv_ps5_sleep_us(100);
    }
    if(ps5_gpu_client_timing(cycle*8+2,count,submit_ns) || ps5_gpu_client_timing(cycle*8+3,count,wait_ns) ||
       ps5_gpu_client_timing(cycle*8+4,20,sleep_ns) || ps5_gpu_client_timing(cycle*8+5,50,ping_ns) ||
       ps5_gpu_client_timing(cycle*8+6,(unsigned)(allowed ? allowed : 1u<<31),seen) ||
       ps5_gpu_client_timing(cycle*8+7,(unsigned)(rpc_count1-rpc_count0),rpc_ns1-rpc_ns0) ||
       ps5_gpu_client_timing(16+cycle,(unsigned)(rpc_count1-rpc_count0),transit1-transit0) ||
       ps5_gpu_client_timing(18+cycle*3,(unsigned)(rpc_count1-rpc_count0),split1[0]-split0[0]) ||
       ps5_gpu_client_timing(19+cycle*3,(unsigned)(rpc_count1-rpc_count0),split1[1]-split0[1]) ||
       ps5_gpu_client_timing(20+cycle*3,(unsigned)(rpc_count1-rpc_count0),split1[2]-split0[2]))goto cleanup;
    okay=1;
cleanup:
    for(unsigned n=0;n<2;++n)if(fences[n])V(DestroyFence)(device,fences[n],NULL);
    if(commands[0])V(FreeCommandBuffers)(device,pool,2,commands);
    if(buffer)V(DestroyBuffer)(device,buffer,NULL);
    if(memory)V(FreeMemory)(device,memory,NULL);
    return okay;
}
static int render(unsigned client,unsigned cycle,uint64_t *hash)
{
    VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory image_memory=VK_NULL_HANDLE,buffer_memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    void *pixels=NULL;int okay=0;
#ifdef PROTON_GPU_DRAW
    VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE;VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
#endif
    VkApplicationInfo application={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="GPU broker offscreen probe",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&application};
    CHECK(V(CreateInstance)(&instance_info,NULL,&instance));
    VkPhysicalDevice physical=VK_NULL_HANDLE;uint32_t count=1;
    CHECK(V(EnumeratePhysicalDevices)(instance,&count,&physical));if(count!=1)goto cleanup;
    VkQueueFamilyProperties families[8];count=8;
    V(GetPhysicalDeviceQueueFamilyProperties)(physical,&count,families);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<count && i<8;++i)if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){family=i;break;}
    if(family==UINT32_MAX)goto cleanup;
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info};
    CHECK(V(CreateDevice)(physical,&device_info,NULL,&device));
    fprintf(stderr,"gpu-vulkan client=%u cycle=%u device_created=1\n",client,cycle);
    VkQueue queue;V(GetDeviceQueue)(device,family,0,&queue);
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={64,64,1},.mipLevels=1,.arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(V(CreateImage)(device,&image_info,NULL,&image));
    VkMemoryRequirements requirements;V(GetImageMemoryRequirements)(device,image,&requirements);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,
        .memoryTypeIndex=memory_type(physical,requirements.memoryTypeBits,0,instance)};
    if(allocation.memoryTypeIndex==UINT32_MAX)goto cleanup;
    CHECK(V(AllocateMemory)(device,&allocation,NULL,&image_memory));
    CHECK(V(BindImageMemory)(device,image,image_memory,0));
    VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*64*4,
        .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(V(CreateBuffer)(device,&buffer_info,NULL,&buffer));
    V(GetBufferMemoryRequirements)(device,buffer,&requirements);
    allocation.allocationSize=requirements.size;
    allocation.memoryTypeIndex=memory_type(physical,requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,instance);
    if(allocation.memoryTypeIndex==UINT32_MAX)goto cleanup;
    CHECK(V(AllocateMemory)(device,&allocation,NULL,&buffer_memory));
    CHECK(V(BindBufferMemory)(device,buffer,buffer_memory,0));
    CHECK(V(MapMemory)(device,buffer_memory,0,VK_WHOLE_SIZE,0,&pixels));
    memset(pixels,0,64*64*4);radv_ps5_cpu_flush(pixels,64*64*4);
    VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(V(CreateCommandPool)(device,&pool_info,NULL,&pool));
    VkCommandBuffer command;
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    CHECK(V(AllocateCommandBuffers)(device,&command_info,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(V(BeginCommandBuffer)(command,&begin));
#ifdef PROTON_GPU_DRAW
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference colour_attachment={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&colour_attachment};
    VkSubpassDependency dependencies[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo pass_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,
        .pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,.dependencyCount=2,.pDependencies=dependencies};
    CHECK(V(CreateRenderPass)(device,&pass_info,NULL,&pass));
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,
        .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    CHECK(V(CreateImageView)(device,&view_info,NULL,&view));
    VkFramebufferCreateInfo framebuffer_info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,
        .attachmentCount=1,.pAttachments=&view,.width=64,.height=64,.layers=1};
    CHECK(V(CreateFramebuffer)(device,&framebuffer_info,NULL,&framebuffer));
    VkPushConstantRange constants={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&constants};
    CHECK(V(CreatePipelineLayout)(device,&layout_info,NULL,&layout));
    VkShaderModuleCreateInfo shader_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof(gpu_probe_vert),.pCode=gpu_probe_vert};
    CHECK(V(CreateShaderModule)(device,&shader_info,NULL,&vertex));
    shader_info.codeSize=sizeof(gpu_probe_frag);shader_info.pCode=gpu_probe_frag;
    CHECK(V(CreateShaderModule)(device,&shader_info,NULL,&fragment));
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo input={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,64,64,0,1};VkRect2D scissor={{0,0},{64,64}};
    VkPipelineViewportStateCreateInfo viewport_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo samples={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount=2,.pStages=stages,.pVertexInputState=&input,.pInputAssemblyState=&assembly,
        .pViewportState=&viewport_info,.pRasterizationState=&raster,.pMultisampleState=&samples,
        .pColorBlendState=&blend,.layout=layout,.renderPass=pass,.subpass=0};
    CHECK(V(CreateGraphicsPipelines)(device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&pipeline));
    VkRenderPassBeginInfo pass_begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,
        .framebuffer=framebuffer,.renderArea={{0,0},{64,64}}};
    float rgba[4]={client==0,client==1,cycle==1,1};
    V(CmdBeginRenderPass)(command,&pass_begin,VK_SUBPASS_CONTENTS_INLINE);
    V(CmdBindPipeline)(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    V(CmdPushConstants)(command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(rgba),rgba);
    V(CmdDraw)(command,3,1,0,0);
    V(CmdEndRenderPass)(command);
#else
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier image_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
    VkClearColorValue colour={.float32={client==0,client==1,cycle==1,1}};
    V(CmdClearColorImage)(command,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&colour,1,&range);
    image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image_barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image_barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&image_barrier);
#endif
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={64,64,1}};
    V(CmdCopyImageToBuffer)(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    VkBufferMemoryBarrier host_barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=buffer,.size=VK_WHOLE_SIZE};
    V(CmdPipelineBarrier)(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host_barrier,0,NULL);
    CHECK(V(EndCommandBuffer)(command));
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(V(CreateFence)(device,&fence_info,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(V(QueueSubmit)(queue,1,&submit,fence));
    CHECK(V(WaitForFences)(device,1,&fence,VK_TRUE,5000000000ull));
    VkMappedMemoryRange invalidate={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=buffer_memory,.size=VK_WHOLE_SIZE};
    CHECK(V(InvalidateMappedMemoryRanges)(device,1,&invalidate));radv_ps5_cpu_flush(pixels,64*64*4);
    const uint32_t expected=0xff000000u | (client==0 ? 0xffu : 0xff00u) | (cycle==1 ? 0xff0000u : 0);
    *hash=1469598103934665603ull;
    for(unsigned i=0;i<64*64;++i) {
        uint32_t value=((uint32_t *)pixels)[i];*hash=(*hash^value)*1099511628211ull;
        if(value!=expected) { fprintf(stderr,"gpu-vulkan pixel_mismatch client=%u cycle=%u pixel=%u actual=%#x expected=%#x\n",client,cycle,i,value,expected);goto cleanup; }
    }
#ifdef PROTON_GPU_PRESENT
    extern int ps5_gpu_client_present(const void *,unsigned,unsigned,unsigned,unsigned);
    if(ps5_gpu_client_present(pixels,64,64,256,cycle))goto cleanup;
#endif
    for(unsigned kind=0;kind<2;++kind) {
        unsigned matched=0;uint64_t coherent_hash=0;
        if(!coherence(instance,physical,device,queue,pool,image,client,cycle,kind,&matched,&coherent_hash))goto cleanup;
        fprintf(stderr,"gpu-vulkan client=%u cycle=%u coherence kind=%u words=%u\n",client,cycle,kind,matched);
        /* Reported, not judged here: the owner records every check, so one failure does not hide the rest. */
        if(ps5_gpu_client_coherence(cycle*2+kind,matched,coherent_hash))goto cleanup;
    }
    if(!throughput(instance,physical,device,queue,pool,client,cycle))goto cleanup;
    okay=1;
cleanup:
    if(device) {
        V(DeviceWaitIdle)(device);
        if(pixels)V(UnmapMemory)(device,buffer_memory);
        if(fence)V(DestroyFence)(device,fence,NULL);
        if(pool)V(DestroyCommandPool)(device,pool,NULL);
#ifdef PROTON_GPU_DRAW
        if(pipeline)V(DestroyPipeline)(device,pipeline,NULL);
        if(vertex)V(DestroyShaderModule)(device,vertex,NULL);
        if(fragment)V(DestroyShaderModule)(device,fragment,NULL);
        if(layout)V(DestroyPipelineLayout)(device,layout,NULL);
        if(framebuffer)V(DestroyFramebuffer)(device,framebuffer,NULL);
        if(view)V(DestroyImageView)(device,view,NULL);
        if(pass)V(DestroyRenderPass)(device,pass,NULL);
#endif
        if(buffer)V(DestroyBuffer)(device,buffer,NULL);
        if(image)V(DestroyImage)(device,image,NULL);
        if(buffer_memory)V(FreeMemory)(device,buffer_memory,NULL);
        if(image_memory)V(FreeMemory)(device,image_memory,NULL);
        V(DestroyDevice)(device,NULL);
    }
    if(instance)V(DestroyInstance)(instance,NULL);
    fprintf(stderr,"gpu-vulkan client=%u cycle=%u pixels=%u verdict=%d\n",client,cycle,okay ? 4096u : 0u,okay);
    return !okay;
}
int proton_gpu_vulkan_probe(int fd,unsigned client)
{
    if(client>1 || proton_gpu_client_bind(fd))return -1;
    uint64_t hash=0,images=0;int status=0;
    for(unsigned cycle=0;cycle<2 && !status;++cycle) { status=render(client,cycle,&hash);
        if(!status)status=ps5_gpu_client_image(cycle,4096,hash);
        if(!status)++images; }
    return ps5_gpu_client_done(status,images,hash) ? -1 : status;
}
