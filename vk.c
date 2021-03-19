#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk.h"

#define EXPORTED_VK_FUNC(name)          static PFN_##name name;
#define GLOBAL_VK_FUNC(name)            static PFN_##name name;
#define INSTANCE_VK_FUNC(name)          static PFN_##name name;
#define DEVICE_VK_FUNC(name)            static PFN_##name name;

#include "vkfuncs.h"

/* For xmalloc, xrealloc */
#include "st.h"

#define ATLASSIZ                        (1024)
#define ATLASPAD                        (1)

#define APPNAME                         "stvk"
#define APPVER                          VK_MAKE_VERSION(0, 1, 0)
#define APIVER                          VK_MAKE_VERSION(1, 0, 0)

#define SSBUFSIZ                        (1024*1024*2)
#define STGBUFSIZ                       (SSBUFSIZ + ATLASSIZ*ATLASSIZ)

#define makerect(x, y, w, h)            (Rect){(x), (y), (w), (h)}
#define makearr(s, n)                   (s) = xmalloc((n)*sizeof(*(s)))
#define makequad(x, y, uv, fg, bg)      (VKQUAD){(x), (y), (uv), (fg), (bg)}

static const char *instext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
static const char *devext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

extern const char vssrc[];
extern const char fssrc[];
extern const int vssrc_size;
extern const int fssrc_size;

#pragma pack(push, 1)
typedef struct {
        float vw, vh;
        float tw, th;
} VKPC;

typedef struct {
        uint16_t x, y;
        Rect uv;
        Color fg;
        Color bg;
} VKQUAD;
#pragma pack(pop)

typedef struct {
        uint32_t w;
        uint32_t h;
        uint32_t nimg;
        VkImage *imgs;
        Rect *dirty;
        VkSwapchainKHR handle;
} VKSC;

typedef struct {
        VkImage img;
        VkDeviceMemory mem;
        VkImageView view;
        VkFramebuffer fb;
} VKRT;

typedef struct {
        VkDescriptorSetLayout desc;
        VkPipelineLayout layout;
        VkPipeline handle;
} VKPIPE;

typedef struct {
        VkBuffer handle;
        VkDeviceMemory mem;
        VkDeviceSize size;
} VKBUF;

typedef struct {
        VkImage handle;
        VkDeviceMemory mem;
        VkImageView view;
        VkSampler sampler;
} VKIMG;

typedef struct {
        void *lib;
        VkInstance instance;
        VkSurfaceKHR surface;
        VkPhysicalDevice pdev;
        VkDevice dev;
        VKSC swapchain;
        uint32_t nqidx;
        uint32_t qidx[2];
        VkQueue gfxq;
        VkQueue presq;
        VKRT rt;
        VkRenderPass pass;
        VKPIPE pipeline;
        VkCommandPool cmdpool;
        VkCommandBuffer cmdbuf;
        VkDescriptorPool descpool;
        VkDescriptorSet descset;
        VkSemaphore acquire, release;
        Rect dirty;
} VKCTX;

typedef struct {
        uint16_t x;
        uint16_t y;
        uint16_t maxy;
        uint16_t dirty;
        uint8_t data[ATLASSIZ*ATLASSIZ];
} VKATLAS;

typedef struct {
        uint32_t sz;
        uint32_t cap;
        VKQUAD *data;
} VKARR;

static VKCTX ctx;
static VKIMG fontimg;
static VKBUF ssbuf;
static VKBUF stgbuf;
static VKATLAS fontatlas = { .x = ATLASPAD, .y = ATLASPAD };
static VKARR quadarr;

static int load_exported_vk_func(void);
static int load_global_vk_funcs(void);
static int load_instance_vk_funcs(void);
static int load_device_vk_funcs(void);

static inline void addrect(Rect *, Rect);
static int initswapchain(VKSC *, uint32_t, uint32_t);
static void freeswapchain(VKSC *);
static inline uint32_t getmemidx(uint32_t, VkMemoryPropertyFlags);
static int initrt(VKRT *);
static void freert(VKRT *);
static int initpipe(VKPIPE *);
static void freepipe(VKPIPE *);
static int initbuf(VKBUF *, VkDeviceSize, VkBufferUsageFlags, VkMemoryPropertyFlags);
static void freebuf(VKBUF *);
static int initimg(VKIMG *, uint32_t, uint32_t, VkFormat);
static void freeimg(VKIMG *);
static void imgbarrier(VkImage, VkAccessFlags, VkAccessFlags, VkImageLayout, VkImageLayout,
                VkPipelineStageFlags, VkPipelineStageFlags);
static void bufbarrier(VkBuffer, VkDeviceSize,
                VkAccessFlags, VkAccessFlags,
                VkPipelineStageFlags, VkPipelineStageFlags);

int
load_exported_vk_func(void)
{
#define EXPORTED_VK_FUNC(name)                                          \
        name = (PFN_##name)dlsym(ctx.lib, #name);                       \
        if (!name) {                                                    \
                fprintf(stderr, "failed to load vk proc: " #name "\n"); \
                return 1;                                               \
        }
#include "vkfuncs.h"

    return 0;
}

int
load_global_vk_funcs(void)
{
#define GLOBAL_VK_FUNC(name)                                            \
        name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);          \
                if (!name) {                                            \
                fprintf(stderr, "failed to load vk proc: " #name "\n"); \
                return 1;                                               \
        }
#include "vkfuncs.h"

    return 0;
}

int
load_instance_vk_funcs(void)
{
#define INSTANCE_VK_FUNC(name)                                          \
    name = (PFN_##name)vkGetInstanceProcAddr(ctx.instance, #name);      \
    if (!name) {                                                        \
            fprintf(stderr, "failed to load vk proc: " #name "\n");     \
            return 1;                                                   \
    }
#include "vkfuncs.h"

    return 0;
}

int
load_device_vk_funcs(void)
{
#define DEVICE_VK_FUNC(name)                                            \
    name = (PFN_##name)vkGetDeviceProcAddr(ctx.dev, #name);             \
    if (!name) {                                                        \
            fprintf(stderr, "failed to load vk proc: " #name "\n");     \
            return 1;                                                   \
    }
#include "vkfuncs.h"

    return 0;
}

void
addrect(Rect *a, Rect b)
{
        uint16_t x1 = b.x + b.w;
        uint16_t y1 = b.y + b.h;

        if (b.x < a->x)
                a->x = b.x;
        if (x1 > a->x + a->w)
                a->w = x1 - a->x;

        if (b.y < a->y)
                a->y = b.y;
        if (y1 > a->y + a->h)
                a->h = y1 - a->y;
}

int
initswapchain(VKSC *sc, uint32_t w, uint32_t h)
{
        uint32_t count, i;
        VkSurfaceCapabilitiesKHR caps;
        VkSurfaceFormatKHR *fmts;
        VkPresentModeKHR *modes;
        VkSurfaceFormatKHR fmt;
        VkPresentModeKHR mode;

        /* Pick a surface format */
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.pdev, ctx.surface, &count, NULL);
        makearr(fmts, count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.pdev, ctx.surface, &count, fmts);
        fmt = fmts[0];
        for (i = 0; i < count; i++) {
                if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                        fmt = fmts[i];
                        break;
                }
        }
        free(fmts);

        /* Pick a present mode, or use FIFO as fallback */
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.pdev, ctx.surface, &count, NULL);
        makearr(modes, count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.pdev, ctx.surface, &count, modes);
        mode = VK_PRESENT_MODE_FIFO_KHR;
        for (i = 0; i < count; i++) {
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                        mode = modes[i];
                        break;
                }
        }
        free(modes);

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.pdev, ctx.surface, &caps);
        sc->w = caps.currentExtent.width;
        sc->h = caps.currentExtent.height;
        if (sc->w == UINT32_MAX) {
                sc->w = MAX(caps.minImageExtent.width, MIN(caps.maxImageExtent.width, w));
                sc->h = MAX(caps.minImageExtent.height, MIN(caps.maxImageExtent.height, h));
        }

        /* Create the swapchain */
        {
                VkSwapchainCreateInfoKHR info = {0};
                info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                info.surface = ctx.surface;
                info.minImageCount = caps.minImageCount;
                info.imageFormat = fmt.format;
                info.imageColorSpace = fmt.colorSpace;
                info.imageExtent.width = sc->w;
                info.imageExtent.height = sc->h;
                info.imageArrayLayers = 1;
                info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                info.preTransform = caps.currentTransform;
                info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                info.presentMode = mode;
                info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                info.clipped = VK_TRUE;
                info.oldSwapchain = VK_NULL_HANDLE;
                info.queueFamilyIndexCount = ctx.nqidx;
                info.pQueueFamilyIndices = ctx.qidx;
                info.imageSharingMode = (ctx.nqidx == 2) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
                if (vkCreateSwapchainKHR(ctx.dev, &info, NULL, &sc->handle) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateSwapchainKHR()\n");
                        return 1;
                }
        }

        /* Get the swapchain images */
        vkGetSwapchainImagesKHR(ctx.dev, sc->handle, &sc->nimg, NULL);
        makearr(sc->imgs, sc->nimg);
        vkGetSwapchainImagesKHR(ctx.dev, sc->handle, &sc->nimg, sc->imgs);

        /* Create the dirty rectangles */
        makearr(sc->dirty, sc->nimg);
        for (i = 0; i < sc->nimg; i++)
                sc->dirty[i] = makerect(0, 0, (uint16_t)sc->w, (uint16_t)sc->h);

        /* Zero out the frame dirty rect, to make sure the copy op
         * stays within the boundaries of the newly resized images */
        ctx.dirty = makerect(0, 0, 0, 0);

        return 0;
}

void
freeswapchain(VKSC *sc)
{
        vkDestroySwapchainKHR(ctx.dev, sc->handle, NULL);
        free(sc->imgs);
        free(sc->dirty);
}

uint32_t
getmemidx(uint32_t type, VkMemoryPropertyFlags flags)
{
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(ctx.pdev, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
                if ((type & (1<<i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                        return i;
        }
        return UINT32_MAX;
}

int
initrt(VKRT *rt)
{
        VkImageCreateInfo imginfo = {0};
        VkMemoryAllocateInfo allocinfo = {0};
        VkImageViewCreateInfo viewinfo = {0};
        VkMemoryRequirements memreq;
        uint32_t memidx;
        uint32_t w = ctx.swapchain.w;
        uint32_t h = ctx.swapchain.h;

        /* Create the image */
        imginfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imginfo.imageType = VK_IMAGE_TYPE_2D;
        imginfo.extent.width = w;
        imginfo.extent.height = h;
        imginfo.extent.depth = 1;
        imginfo.mipLevels = 1;
        imginfo.arrayLayers = 1;
        imginfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imginfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(ctx.dev, &imginfo, NULL, &rt->img) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateImage()\n");
                return 1;
        }

        /* Allocate memory */
        vkGetImageMemoryRequirements(ctx.dev, rt->img, &memreq);
        memidx = getmemidx(memreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memidx == UINT32_MAX) {
                fprintf(stderr, "FATAL: Could not find a suitable memory type\n");
                vkDestroyImage(ctx.dev, rt->img, NULL);
                return 1;
        }

        allocinfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocinfo.allocationSize = memreq.size;
        allocinfo.memoryTypeIndex = memidx;
        if (vkAllocateMemory(ctx.dev, &allocinfo, NULL, &rt->mem) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkAllocateMemory()\n");
                vkDestroyImage(ctx.dev, rt->img, NULL);
                return 1;
        }
        vkBindImageMemory(ctx.dev, rt->img, rt->mem, 0);

        /* Create the image view */
        viewinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewinfo.image = rt->img;
        viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewinfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewinfo.subresourceRange.levelCount = 1;
        viewinfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx.dev, &viewinfo, NULL, &rt->view) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkAllocateMemory()\n");
                vkFreeMemory(ctx.dev, rt->mem, NULL);
                vkDestroyImage(ctx.dev, rt->img, NULL);
                return 1;
        }

        /* Create the framebuffer */
        VkFramebufferCreateInfo fbinfo = {0};
        fbinfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbinfo.renderPass = ctx.pass;
        fbinfo.attachmentCount = 1;
        fbinfo.pAttachments = &rt->view;
        fbinfo.width = w;
        fbinfo.height = h;
        fbinfo.layers = 1;
        if (vkCreateFramebuffer(ctx.dev, &fbinfo, NULL, &rt->fb) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateFramebuffer()\n");
                vkDestroyImageView(ctx.dev, rt->view, NULL);
                vkFreeMemory(ctx.dev, rt->mem, NULL);
                vkDestroyImage(ctx.dev, rt->img, NULL);
                return 1;
        }

        /* Perform the initial clear and layout transition */
        /* TODO: Is the clear really necessary? */
        vkResetCommandPool(ctx.dev, ctx.cmdpool, 0);
        VkCommandBufferBeginInfo begininfo = {0};
        begininfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begininfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(ctx.cmdbuf, &begininfo) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkBeginCommandBuffer()\n");
                return 1;
        }

        VkClearColorValue color = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {0};
        range.layerCount = 1;
        range.levelCount = 1;
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        imgbarrier(rt->img, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(ctx.cmdbuf, rt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        imgbarrier(rt->img, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        vkEndCommandBuffer(ctx.cmdbuf);
        VkSubmitInfo info = {0};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &ctx.cmdbuf;
        vkQueueSubmit(ctx.gfxq, 1, &info, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.gfxq);

        return 0;
}

void
freert(VKRT *rt)
{
        vkFreeMemory(ctx.dev, rt->mem, NULL);
        vkDestroyFramebuffer(ctx.dev, rt->fb, NULL);
        vkDestroyImageView(ctx.dev, rt->view, NULL);
        vkDestroyImage(ctx.dev, rt->img, NULL);
}

int
initpipe(VKPIPE *pipe)
{
        VkShaderModule vs, fs;

        VkShaderModuleCreateInfo shaderinfo = {0};
        shaderinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderinfo.codeSize = (size_t)vssrc_size;
        shaderinfo.pCode = (const void *)vssrc;
        if (vkCreateShaderModule(ctx.dev, &shaderinfo, NULL, &vs) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateShaderModule()\n");
                return 1;
        }
        shaderinfo.codeSize = (size_t)fssrc_size;
        shaderinfo.pCode = (const void *)fssrc;
        if (vkCreateShaderModule(ctx.dev, &shaderinfo, NULL, &fs) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateShaderModule()\n");
                return 1;
        }

        VkPipelineShaderStageCreateInfo stages[2] = {0};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo inputstate = {0};
        inputstate.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputassy = {0};
        inputassy.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputassy.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkPipelineRasterizationStateCreateInfo rasterizer = {0};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo ms = {0};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendatt = {0};
        blendatt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        blendatt.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo blend = {0};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendatt;

        VkPipelineViewportStateCreateInfo viewport = {0};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkDynamicState ds[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynstate = {0};
        dynstate.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynstate.dynamicStateCount = 2;
        dynstate.pDynamicStates = ds;

        /* Descriptor set layout */
        VkPushConstantRange range = {0};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(VKPC);

        VkDescriptorSetLayoutBinding bindings[2] = {0};
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].binding = 0;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].binding = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo descinfo = {0};
        descinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descinfo.bindingCount = 2;
        descinfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(ctx.dev, &descinfo, NULL, &pipe->desc) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateDescriptorSetLayout()\n");
                vkDestroyShaderModule(ctx.dev, vs, NULL);
                vkDestroyShaderModule(ctx.dev, fs, NULL);
                return 1;
        }

        /* Pipeline layout */
        VkPipelineLayoutCreateInfo layoutinfo = {0};
        layoutinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutinfo.pushConstantRangeCount = 1;
        layoutinfo.pPushConstantRanges = &range;
        layoutinfo.setLayoutCount = 1;
        layoutinfo.pSetLayouts = &pipe->desc;
        if (vkCreatePipelineLayout(ctx.dev, &layoutinfo, NULL, &pipe->layout) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreatePipelineLayout()\n");
                vkDestroyDescriptorSetLayout(ctx.dev, pipe->desc, NULL);
                vkDestroyShaderModule(ctx.dev, vs, NULL);
                vkDestroyShaderModule(ctx.dev, fs, NULL);
                return 1;
        }

        /* Graphics pipeline */
        VkGraphicsPipelineCreateInfo gfxinfo = {0};
        gfxinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gfxinfo.stageCount = 2;
        gfxinfo.pStages = stages;
        gfxinfo.pVertexInputState = &inputstate;
        gfxinfo.pInputAssemblyState = &inputassy;
        gfxinfo.pRasterizationState = &rasterizer;
        gfxinfo.pMultisampleState = &ms;
        gfxinfo.pColorBlendState = &blend;
        gfxinfo.pViewportState = &viewport;
        gfxinfo.pDynamicState = &dynstate;
        gfxinfo.layout = pipe->layout;
        gfxinfo.renderPass = ctx.pass;
        if (vkCreateGraphicsPipelines(ctx.dev, VK_NULL_HANDLE, 1, &gfxinfo, NULL, &pipe->handle) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateGraphicsPipelines()\n");
                vkDestroyDescriptorSetLayout(ctx.dev, pipe->desc, NULL);
                vkDestroyPipelineLayout(ctx.dev, pipe->layout, NULL);
                vkDestroyShaderModule(ctx.dev, vs, NULL);
                vkDestroyShaderModule(ctx.dev, fs, NULL);
                return 1;
        }

        vkDestroyShaderModule(ctx.dev, vs, NULL);
        vkDestroyShaderModule(ctx.dev, fs, NULL);

        return 0;
}

void
freepipe(VKPIPE *pipe)
{
        vkDestroyDescriptorSetLayout(ctx.dev, pipe->desc, NULL);
        vkDestroyPipelineLayout(ctx.dev, pipe->layout, NULL);
        vkDestroyPipeline(ctx.dev, pipe->handle, NULL);
}

int
initbuf(VKBUF *buf, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
{
        VkBufferCreateInfo info = {0};
        VkMemoryAllocateInfo allocinfo = {0};
        VkMemoryRequirements memreq;
        uint32_t memidx;

        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(ctx.dev, &info, NULL, &buf->handle) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateBuffer()\n");
                return 1;
        }

        vkGetBufferMemoryRequirements(ctx.dev, buf->handle, &memreq);
        memidx = getmemidx(memreq.memoryTypeBits, flags);
        if (memidx == UINT32_MAX) {
                fprintf(stderr, "FATAL: Could not find a suitable memory type\n");
                vkDestroyBuffer(ctx.dev, buf->handle, NULL);
                return 1;
        }

        allocinfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocinfo.allocationSize = memreq.size;
        allocinfo.memoryTypeIndex = memidx;
        if (vkAllocateMemory(ctx.dev, &allocinfo, NULL, &buf->mem) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkAllocateMemory()\n");
                vkDestroyBuffer(ctx.dev, buf->handle, NULL);
                return 1;
        }
        vkBindBufferMemory(ctx.dev, buf->handle, buf->mem, 0);
        buf->size = memreq.size;

        return 0;
}

void
freebuf(VKBUF *buf)
{
        vkFreeMemory(ctx.dev, buf->mem, NULL);
        vkDestroyBuffer(ctx.dev, buf->handle, NULL);
}

int
initimg(VKIMG *img, uint32_t w, uint32_t h, VkFormat fmt)
{
        VkImageCreateInfo info = {0};
        VkMemoryAllocateInfo allocinfo = {0};
        VkImageViewCreateInfo viewinfo = {0};
        VkSamplerCreateInfo samplerinfo = {0};
        VkMemoryRequirements memreq;
        uint32_t memidx;

        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent.width = w;
        info.extent.height = h;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.format = fmt;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(ctx.dev, &info, NULL, &img->handle) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateImage()\n");
                return 1;
        }

        vkGetImageMemoryRequirements(ctx.dev, img->handle, &memreq);
        memidx = getmemidx(memreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memidx == UINT32_MAX) {
                fprintf(stderr, "FATAL: Could not find a suitable memory type\n");
                vkDestroyImage(ctx.dev, img->handle, NULL);
                return 1;
        }

        allocinfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocinfo.allocationSize = memreq.size;
        allocinfo.memoryTypeIndex = memidx;
        if (vkAllocateMemory(ctx.dev, &allocinfo, NULL, &img->mem) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkAllocateMemory()\n");
                vkDestroyImage(ctx.dev, img->handle, NULL);
                return 1;
        }
        vkBindImageMemory(ctx.dev, img->handle, img->mem, 0);

        viewinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewinfo.image = img->handle;
        viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewinfo.format = fmt;
        viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewinfo.subresourceRange.levelCount = 1;
        viewinfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx.dev, &viewinfo, NULL, &img->view) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateImageView()\n");
                vkFreeMemory(ctx.dev, img->mem, NULL);
                vkDestroyImage(ctx.dev, img->handle, NULL);
                return 1;
        }

        samplerinfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerinfo.magFilter = VK_FILTER_LINEAR;
        samplerinfo.minFilter = VK_FILTER_LINEAR;
        samplerinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        if (vkCreateSampler(ctx.dev, &samplerinfo, NULL, &img->sampler) != VK_SUCCESS) {
                fprintf(stderr, "FATAL: vkCreateSampler()\n");
                vkDestroyImageView(ctx.dev, img->view, NULL);
                vkFreeMemory(ctx.dev, img->mem, NULL);
                vkDestroyImage(ctx.dev, img->handle, NULL);
                return 1;
        }

        return 0;
}

void
freeimg(VKIMG *img)
{
        vkDestroySampler(ctx.dev, img->sampler, NULL);
        vkDestroyImageView(ctx.dev, img->view, NULL);
        vkFreeMemory(ctx.dev, img->mem, NULL);
        vkDestroyImage(ctx.dev, img->handle, NULL);
}

void
imgbarrier(VkImage img, VkAccessFlags srcacc, VkAccessFlags dstacc,
           VkImageLayout ol, VkImageLayout nl,
           VkPipelineStageFlags srcstage, VkPipelineStageFlags dststage)
{
        VkImageMemoryBarrier barrier = {0};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcacc;
        barrier.dstAccessMask = dstacc;
        barrier.oldLayout = ol;
        barrier.newLayout = nl;
        barrier.image = img;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(ctx.cmdbuf, srcstage, dststage, VK_DEPENDENCY_BY_REGION_BIT,
                        0, NULL, 0, NULL, 1, &barrier);
}

void
bufbarrier(VkBuffer buf, VkDeviceSize size,
                VkAccessFlags srcacc, VkAccessFlags dstacc,
                VkPipelineStageFlags srcstage, VkPipelineStageFlags dststage)
{
        VkBufferMemoryBarrier barrier = {0};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = srcacc;
        barrier.dstAccessMask = dstacc;
        barrier.buffer = buf;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.size = size;
        vkCmdPipelineBarrier(ctx.cmdbuf, srcstage, dststage, VK_DEPENDENCY_BY_REGION_BIT,
                        0, NULL, 1, &barrier, 0, NULL);
}

int
blitatlas(uint16_t *x, uint16_t *y, uint16_t w, uint16_t h, uint16_t cw, uint16_t ch,
                uint16_t ox, uint16_t oy, uint16_t pitch, const uint8_t *data)
{
        uint8_t *dst;
        uint16_t i;

        if (fontatlas.x + cw + ATLASPAD > ATLASSIZ) {
                if (fontatlas.y + ch + ATLASPAD > ATLASSIZ) {
                        /* Atlas is full. TODO: Resize? */
                        return 1;
                }

                fontatlas.x = ATLASPAD;
                fontatlas.y += fontatlas.maxy + ATLASPAD;
                fontatlas.maxy = 0;
        }

        if (ch > fontatlas.maxy)
                fontatlas.maxy = ch;

        *x = fontatlas.x;
        *y = fontatlas.y;

        dst = fontatlas.data + (fontatlas.y+oy)*ATLASSIZ + fontatlas.x+ox;
        for (i = 0; i < h; i++) {
                memcpy(dst, data, (size_t)w);
                dst += ATLASSIZ;
                data += pitch;
        }

        fontatlas.x += cw + ATLASPAD;
        fontatlas.dirty = 1;

        return 0;
}

int
vkinit(Display *dpy, Window win, int w, int h)
{
        ctx.lib = dlopen("libvulkan.so.1", RTLD_NOW);
        if (!ctx.lib) {
                perror("dlopen");
                return 1;
        }
        if (load_exported_vk_func())
                return 1;
        if (load_global_vk_funcs())
                return 1;

        /* Create the instance */
        {
                VkApplicationInfo appinfo = {0};
                appinfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                appinfo.pApplicationName = APPNAME;
                appinfo.applicationVersion = APPVER;
                appinfo.apiVersion = APIVER;
                VkInstanceCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
                info.pApplicationInfo = &appinfo;
                info.enabledExtensionCount = LEN(instext);
                info.ppEnabledExtensionNames = instext;
#ifdef DEBUG
                const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
                info.enabledLayerCount = 1;
                info.ppEnabledLayerNames = layers;
#endif
                if (vkCreateInstance(&info, NULL, &ctx.instance) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateInstance()\n");
                        return 1;
                }
        }

        if (load_instance_vk_funcs())
                return 1;

        /* Create the surface, WSI */
        {
                VkXlibSurfaceCreateInfoKHR info = {0};
                info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
                info.dpy = dpy;
                info.window = win;
                if (vkCreateXlibSurfaceKHR(ctx.instance, &info, NULL, &ctx.surface) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateXlibSurfaceKHR()\n");
                        return 1;
                }
        }

        /* Choose a physical device */
        {
                VkPhysicalDevice devs[16];
                uint32_t count = 16;
                vkEnumeratePhysicalDevices(ctx.instance, &count, devs);
                if (count == 0) {
                        fprintf(stderr, "FATAL: Did not find any GPUs with vulkan support\n");
                        return 1;
                }
                ctx.pdev = devs[0];
                /* TODO: Validation */
        }

        /* Get the graphics and presentation queue indices */
        ctx.qidx[0] = UINT32_MAX;
        ctx.qidx[1] = UINT32_MAX;
        {
                uint32_t count;
                VkQueueFamilyProperties *props;
                VkBool32 ret;

                vkGetPhysicalDeviceQueueFamilyProperties(ctx.pdev, &count, NULL);
                makearr(props, count);
                vkGetPhysicalDeviceQueueFamilyProperties(ctx.pdev, &count, props);
                for (uint32_t i = 0; i < count; i++) {
                        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                                ctx.qidx[0] = i;

                        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.pdev, i, ctx.surface, &ret);
                        if (ret)
                                ctx.qidx[1] = i;
                }
                free(props);
                if (ctx.qidx[0] == UINT32_MAX || ctx.qidx[1] == UINT32_MAX) {
                        fprintf(stderr, "FATAL: Insufficient queue support\n");
                        return 1;
                }
                ctx.nqidx = ctx.qidx[0] == ctx.qidx[1] ? 1 : 2;
        }

        /* Create the logical device */
        {
                VkDeviceQueueCreateInfo qinfo[2] = {0};
                for (uint32_t i = 0; i < ctx.nqidx; i++) {
                        qinfo[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                        qinfo[i].queueFamilyIndex = ctx.qidx[i];
                        qinfo[i].queueCount = 1;
                        qinfo[i].pQueuePriorities = &(float){1.0f};
                }

                VkDeviceCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                info.pQueueCreateInfos = qinfo;
                info.queueCreateInfoCount = ctx.nqidx;
                info.pEnabledFeatures = &(VkPhysicalDeviceFeatures){0};
                info.ppEnabledExtensionNames = devext;
                info.enabledExtensionCount = 1;
                if (vkCreateDevice(ctx.pdev, &info, NULL, &ctx.dev) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateDevice()\n");
                        return 1;
                }
        }

        if (load_device_vk_funcs())
                return 1;

        /* Get the device queues */
        vkGetDeviceQueue(ctx.dev, ctx.qidx[0], 0, &ctx.gfxq);
        vkGetDeviceQueue(ctx.dev, ctx.qidx[1], 0, &ctx.presq);

        /* Create the swapchain */
        if (initswapchain(&ctx.swapchain, (uint32_t)w, (uint32_t)h))
                return 1;

         /* Create the render pass */
        {
                VkAttachmentDescription att = {0};
                att.format = VK_FORMAT_B8G8R8A8_UNORM;
                att.samples = VK_SAMPLE_COUNT_1_BIT;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                att.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

                VkAttachmentReference ref = {0};
                ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                VkSubpassDescription subpass = {0};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = 1;
                subpass.pColorAttachments = &ref;

                VkRenderPassCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
                info.attachmentCount = 1;
                info.pAttachments = &att;
                info.subpassCount = 1;
                info.pSubpasses = &subpass;
                if (vkCreateRenderPass(ctx.dev, &info, NULL, &ctx.pass) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateRenderPass()\n");
                        return 1;
                }
        }

        /* Create the command pool, allocate a command buffer */
        {
                VkCommandPoolCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                info.queueFamilyIndex = ctx.qidx[0];
                if (vkCreateCommandPool(ctx.dev, &info, NULL, &ctx.cmdpool) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateCommandPool()\n");
                        return 1;
                }

                VkCommandBufferAllocateInfo allocinfo = {0};
                allocinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocinfo.commandPool = ctx.cmdpool;
                allocinfo.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(ctx.dev, &allocinfo, &ctx.cmdbuf) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkAllocateCommandBuffers()\n");
                        return 1;
                }
        }

        /* Create the render target */
        if (initrt(&ctx.rt))
                return 1;

        /* Create the graphics pipeline */
        if (initpipe(&ctx.pipeline))
                return 1;

        /* Font texture, buffers */
        if (initimg(&fontimg, ATLASSIZ, ATLASSIZ, VK_FORMAT_R8_UNORM))
                return 1;
        if (initbuf(&stgbuf, STGBUFSIZ, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                return 1;
        if (initbuf(&ssbuf, SSBUFSIZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return 1;

        /* Create the descriptor pool, allocate a descriptor set */
        {
                VkDescriptorPoolSize sizes[2] = {0};
                sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                sizes[0].descriptorCount = 1;
                sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                sizes[1].descriptorCount = 1;

                VkDescriptorPoolCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                info.poolSizeCount = 2;
                info.pPoolSizes = sizes;
                info.maxSets = 1;
                if (vkCreateDescriptorPool(ctx.dev, &info, NULL, &ctx.descpool) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkCreateDescriptorPool()\n");
                        return 1;
                }

                VkDescriptorSetAllocateInfo alloc = {0};
                alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                alloc.descriptorPool = ctx.descpool;
                alloc.descriptorSetCount = 1;
                alloc.pSetLayouts = &ctx.pipeline.desc;
                if (vkAllocateDescriptorSets(ctx.dev, &alloc, &ctx.descset) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkAllocateDescriptorSets()\n");
                        return 1;
                }

                VkDescriptorImageInfo imginfo = {0};
                imginfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imginfo.imageView = fontimg.view;
                imginfo.sampler = fontimg.sampler;

                VkDescriptorBufferInfo bufinfo = {0};
                bufinfo.buffer = ssbuf.handle;
                bufinfo.offset = 0;
                bufinfo.range = VK_WHOLE_SIZE;

                VkWriteDescriptorSet writes[2] = {0};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = ctx.descset;
                writes[0].dstBinding = 0;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[0].descriptorCount = 1;
                writes[0].pBufferInfo = &bufinfo;
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = ctx.descset;
                writes[1].dstBinding = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].descriptorCount = 1;
                writes[1].pImageInfo = &imginfo;
                vkUpdateDescriptorSets(ctx.dev, 2, writes, 0, NULL);
        }


        /* Create the semaphores */
        {
                VkSemaphoreCreateInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                if (vkCreateSemaphore(ctx.dev, &info, NULL, &ctx.acquire) != VK_SUCCESS ||
                    vkCreateSemaphore(ctx.dev, &info, NULL, &ctx.release) != VK_SUCCESS)
                        return 1;
        }

        /* Update the fontatlas on the first render regardless of its actual state */
        fontatlas.dirty = 1;

        return 0;
}

void
vkfree(void)
{
        free(quadarr.data);

        vkDeviceWaitIdle(ctx.dev);
        vkDestroySemaphore(ctx.dev, ctx.acquire, NULL);
        vkDestroySemaphore(ctx.dev, ctx.release, NULL);
        freebuf(&ssbuf);
        freebuf(&stgbuf);
        freeimg(&fontimg);
        vkDestroyDescriptorPool(ctx.dev, ctx.descpool, NULL);
        vkDestroyCommandPool(ctx.dev, ctx.cmdpool, NULL);
        freepipe(&ctx.pipeline);
        vkDestroyRenderPass(ctx.dev, ctx.pass, NULL);
        freert(&ctx.rt);
        freeswapchain(&ctx.swapchain);
        vkDestroyDevice(ctx.dev, NULL);
        vkDestroySurfaceKHR(ctx.instance, ctx.surface, NULL);
        vkDestroyInstance(ctx.instance, NULL);
        dlclose(ctx.lib);
}

int
vkresize(int w, int h)
{
        VKSC *sc = &ctx.swapchain;
        VKRT *rt = &ctx.rt;

        vkDeviceWaitIdle(ctx.dev);
        freeswapchain(sc);
        freert(rt);

        if (initswapchain(sc, (uint32_t)w, (uint32_t)h))
                return 1;
        if (initrt(rt))
                return 1;

        return 0;
}

void
vkpushquad(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t uvx, uint16_t uvy, Color fg, Color bg)
{
        uint32_t cap;

        if (quadarr.sz == quadarr.cap) {
                cap = quadarr.cap ? quadarr.cap*2 : 1024;
                quadarr.data = xrealloc(quadarr.data, sizeof *quadarr.data * cap);
                quadarr.cap = cap;
        }
        quadarr.data[quadarr.sz++] = makequad(x, y, makerect(uvx, uvy, w, h), fg, bg);
        addrect(&ctx.dirty, makerect(x, y, w, h));
}

int
vkflush(void)
{
        VKSC *sc;
        void *stgp;
        uint32_t nquad, imgidx, i;
        size_t datasz;

        sc = &ctx.swapchain;
        nquad = quadarr.sz;
        if (nquad == 0)
                return 0;

        vkAcquireNextImageKHR(ctx.dev, sc->handle, UINT64_MAX, ctx.acquire, VK_NULL_HANDLE, &imgidx);

        datasz = nquad * sizeof(VKQUAD);
        if (datasz > SSBUFSIZ) {
                /* TODO: Resize the shader storage buffer */
                fputs("warning: shader storage buffer overflow.", stderr);
        }

        /* Begin command buffer */
        {
                vkResetCommandPool(ctx.dev, ctx.cmdpool, 0);
                VkCommandBufferBeginInfo begininfo = {0};
                begininfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begininfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                if (vkBeginCommandBuffer(ctx.cmdbuf, &begininfo) != VK_SUCCESS) {
                        fprintf(stderr, "FATAL: vkBeginCommandBuffer()\n");
                        return 1;
                }
        }

        /* SSBO upload, TODO: benchmark against not using the staging buffer for simplicity */
        {
                vkMapMemory(ctx.dev, stgbuf.mem, 0, datasz, 0, &stgp);
                memcpy(stgp, quadarr.data, datasz);
                vkUnmapMemory(ctx.dev, stgbuf.mem);
                quadarr.sz = 0;

                VkBufferCopy region = {0};
                region.size = datasz;
                vkCmdCopyBuffer(ctx.cmdbuf, stgbuf.handle, ssbuf.handle, 1, &region);
                bufbarrier(ssbuf.handle, datasz,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        }

        /* Texture atlas upload */
        if (fontatlas.dirty) {
                vkMapMemory(ctx.dev, stgbuf.mem, datasz, ATLASSIZ*ATLASSIZ, 0, &stgp);
                memcpy(stgp, fontatlas.data, ATLASSIZ*ATLASSIZ);
                vkUnmapMemory(ctx.dev, stgbuf.mem);

                imgbarrier(fontimg.handle, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

                VkBufferImageCopy region = {0};
                region.bufferOffset = datasz;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent.width = ATLASSIZ;
                region.imageExtent.height = ATLASSIZ;
                region.imageExtent.depth = 1;
                vkCmdCopyBufferToImage(ctx.cmdbuf, stgbuf.handle, fontimg.handle,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                imgbarrier(fontimg.handle, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

                fontatlas.dirty = 0;
        }

        /* Render pass */
        {
                VkRenderPassBeginInfo begininfo = {0};
                begininfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                begininfo.renderPass = ctx.pass;
                begininfo.framebuffer = ctx.rt.fb;
                begininfo.renderArea.extent.width = ctx.swapchain.w;
                begininfo.renderArea.extent.height = ctx.swapchain.h;
                vkCmdBeginRenderPass(ctx.cmdbuf, &begininfo, VK_SUBPASS_CONTENTS_INLINE);

                VkViewport vp = {0, 0, (float)sc->w, (float)sc->h, 0, 1};
                VkRect2D scissor = {{0, 0}, {sc->w, sc->h}};
                vkCmdSetViewport(ctx.cmdbuf, 0, 1, &vp);
                vkCmdSetScissor(ctx.cmdbuf, 0, 1, &scissor);
                vkCmdBindPipeline(ctx.cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.pipeline.handle);
                vkCmdBindDescriptorSets(ctx.cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        ctx.pipeline.layout, 0, 1, &ctx.descset, 0, 0);

                VKPC pc;
                pc.vw = (float)sc->w;
                pc.vh = (float)sc->h;
                pc.tw = pc.th = ATLASSIZ;
                vkCmdPushConstants(ctx.cmdbuf, ctx.pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof pc, &pc);
                vkCmdDraw(ctx.cmdbuf, 4, nquad, 0, 0);
                vkCmdEndRenderPass(ctx.cmdbuf);
        }

        Rect dirty = ctx.dirty;
        addrect(&dirty, sc->dirty[imgidx]);
        for (i = 0; i < sc->nimg; i++)
                addrect(sc->dirty + i, ctx.dirty);

        imgbarrier(sc->imgs[imgidx], VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageCopy region = {0};
        region.srcOffset.x = dirty.x;
        region.srcOffset.y = dirty.y;
        region.dstOffset = region.srcOffset;
        region.extent.width = dirty.w;
        region.extent.height = dirty.h;
        region.extent.depth = 1;
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;

        vkCmdCopyImage(ctx.cmdbuf, ctx.rt.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       sc->imgs[imgidx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        imgbarrier(sc->imgs[imgidx], VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        vkEndCommandBuffer(ctx.cmdbuf);

        ctx.dirty = sc->dirty[imgidx] = makerect(0, 0, 0, 0);

        /* Submit command buffer */
        {
                VkPipelineStageFlags mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo info = {0};
                info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                info.waitSemaphoreCount = 1;
                info.pWaitSemaphores = &ctx.acquire;
                info.pWaitDstStageMask = &mask;
                info.commandBufferCount = 1;
                info.pCommandBuffers = &ctx.cmdbuf;
                info.signalSemaphoreCount = 1;
                info.pSignalSemaphores = &ctx.release;
                vkQueueSubmit(ctx.gfxq, 1, &info, VK_NULL_HANDLE);
        }

        /* Present */
        {
                VkPresentInfoKHR info = {0};
                info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
                info.waitSemaphoreCount = 1;
                info.pWaitSemaphores = &ctx.release;
                info.swapchainCount = 1;
                info.pSwapchains = &sc->handle;
                info.pImageIndices = &imgidx;
                vkQueuePresentKHR(ctx.presq, &info);
        }

        vkQueueWaitIdle(ctx.presq);

        return 0;
}
