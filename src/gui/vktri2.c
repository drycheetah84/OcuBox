/* vktri2 - guest-side SwiftShader proof for hollywood_emu.
 *
 * Runs inside the emulated Quest as an init service. dlopen()s the SwiftShader
 * Vulkan ICD directly (bypassing the guest's Adreno-only Vulkan loader), renders
 * a triangle offscreen on the CPU, then blits the result to the synthetic
 * hollywood_fb MMIO device (mmap of /dev/mem @ 0x100000000) which the host GUI
 * displays. Progress is logged to /dev/kmsg so it shows in the emulator console.
 *
 * Build (NDK): aarch64-linux-android30-clang -O2 -o vktri2 vktri2.c -ldl
 * Run:         vktri2 /mnt/gfx/libvk_swiftshader.so
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static const uint32_t vert_spv[] =
#include "tri_vert.inc"
;
static const uint32_t frag_spv[] =
#include "tri_frag.inc"
;

#define W 512
#define H 512
#define FB_PHYS 0x100000000ULL
#define FB_SIZE 0x1000000ULL
#define FB_DATA 0x1000

static void klog(const char* fmt, ...) {
    char b[512]; b[0]=0;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    int f = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (f >= 0) { char line[560]; int m = snprintf(line, sizeof line, "vktri2: %s\n", b); (void)n; write(f, line, m); close(f); }
    fprintf(stderr, "vktri2: %s\n", b);
}

/* function pointers loaded from the ICD */
#define VKFNS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateRenderPass) \
    X(vkCreateFramebuffer) X(vkCreateShaderModule) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdBindPipeline) X(vkCmdDraw) \
    X(vkCmdEndRenderPass) X(vkCmdCopyImageToBuffer) X(vkEndCommandBuffer) X(vkCreateFence) \
    X(vkQueueSubmit) X(vkWaitForFences) X(vkMapMemory) X(vkUnmapMemory)

#define DECL(n) static PFN_##n n;
VKFNS(DECL)

#define VK_CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){ klog("VK error %d at line %d", (int)_r, __LINE__); return 2; } } while(0)

static uint32_t find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    klog("no suitable memory type"); exit(2);
}

int main(int argc, char** argv) {
    const char* libpath = (argc > 1) ? argv[1] : "/mnt/gfx/libvk_swiftshader.so";
    klog("start; dlopen %s", libpath);
    void* lib = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { klog("dlopen failed: %s", dlerror()); return 2; }
#define LOAD(n) n = (PFN_##n)dlsym(lib, #n); if(!n){ klog("missing symbol %s", #n); return 2; }
    VKFNS(LOAD)
    klog("ICD loaded");

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "vktri2"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst; VK_CHECK(vkCreateInstance(&ici, 0, &inst));

    uint32_t n = 0; VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, 0));
    if (!n) { klog("no Vulkan devices"); return 2; }
    VkPhysicalDevice* pds = malloc(n * sizeof(*pds));
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    klog("device=\"%s\" api=%u.%u type=%d", props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), (int)props.deviceType);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, 0);
    VkQueueFamilyProperties* qf = malloc(qn * sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == UINT32_MAX) { klog("no graphics queue"); return 2; }

    float pr = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qi; qci.queueCount = 1; qci.pQueuePriorities = &pr;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev; VK_CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = W; ii.extent.height = H; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage img; VK_CHECK(vkCreateImage(dev, &ii, 0, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; VK_CHECK(vkAllocateMemory(dev, &mai, 0, &imem));
    VK_CHECK(vkBindImageMemory(dev, img, imem, 0));

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ii.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VkImageView view; VK_CHECK(vkCreateImageView(dev, &vci, 0, &view));

    VkAttachmentDescription ad = { 0 };
    ad.format = ii.format; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { 0 };
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
    VkSubpassDependency dep = { 0 };
    dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &ad; rpci.subpassCount = 1; rpci.pSubpasses = &sd;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    VkRenderPass rp; VK_CHECK(vkCreateRenderPass(dev, &rpci, 0, &rp));

    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = W; fci.height = H; fci.layers = 1;
    VkFramebuffer fb; VK_CHECK(vkCreateFramebuffer(dev, &fci, 0, &fb));

    VkShaderModuleCreateInfo smv = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smv.codeSize = sizeof(vert_spv); smv.pCode = vert_spv;
    VkShaderModule vs; VK_CHECK(vkCreateShaderModule(dev, &smv, 0, &vs));
    VkShaderModuleCreateInfo smf = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smf.codeSize = sizeof(frag_spv); smf.pCode = frag_spv;
    VkShaderModule fs; VK_CHECK(vkCreateShaderModule(dev, &smf, 0, &fs));

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; VK_CHECK(vkCreatePipelineLayout(dev, &plci, 0, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 }; VkRect2D scz = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scz;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 }; cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.layout = pl; gp.renderPass = rp;
    VkPipeline pipe; VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, 0, &pipe));

    VkDeviceSize bsz = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bsz; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf; VK_CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev, buf, &bmr);
    VkMemoryAllocateInfo bmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = find_mem(pd, bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; VK_CHECK(vkAllocateMemory(dev, &bmai, 0, &bmem));
    VK_CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = qi;
    VkCommandPool cp; VK_CHECK(vkCreateCommandPool(dev, &cpci, 0, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd; VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkClearValue clr; clr.color.float32[0] = 0.06f; clr.color.float32[1] = 0.07f;
    clr.color.float32[2] = 0.14f; clr.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea.extent.width = W;
    rbi.renderArea.extent.height = H; rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy bic = { 0 };
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = W; bic.imageExtent.height = H; bic.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fnci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; VK_CHECK(vkCreateFence(dev, &fnci, 0, &fence));
    VK_CHECK(vkQueueSubmit(q, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    klog("render complete; blitting %dx%d to hollywood_fb", W, H);

    void* data; VK_CHECK(vkMapMemory(dev, bmem, 0, bsz, 0, &data));
    const uint8_t* px = (const uint8_t*)data;

    /* Write the frame to the pre-allocated /mnt/gfx/framebuffer file. The emulator
     * watches that partition's blocks (overlay-aware) and publishes the frame to
     * the host GUI. Header (16B): "HFB1", gen, width, height; then RGBA8888 pixels.
     * Pixels first (fsync), header last (fsync) so the emulator only ever sees a
     * complete frame when the header block lands. */
    int ffd = open("/mnt/gfx/framebuffer", O_RDWR | O_CLOEXEC);
    if (ffd < 0) { klog("open /mnt/gfx/framebuffer failed (errno=%d)", errno); return 3; }
    size_t pxbytes = (size_t)W * H * 4;
    if (pwrite(ffd, px, pxbytes, 16) != (ssize_t)pxbytes) { klog("pwrite pixels failed (errno=%d)", errno); return 3; }
    fsync(ffd);
    uint8_t hdr[16];
    memcpy(hdr + 0, "HFB1", 4);
    uint32_t gen = 1, w = W, h = H;
    memcpy(hdr + 4, &gen, 4); memcpy(hdr + 8, &w, 4); memcpy(hdr + 12, &h, 4);
    if (pwrite(ffd, hdr, 16, 0) != 16) { klog("pwrite header failed (errno=%d)", errno); return 3; }
    fsync(ffd);
    close(ffd);
    vkUnmapMemory(dev, bmem);
    klog("frame %dx%d written to /mnt/gfx/framebuffer -- sent to host GUI", W, H);
    return 0;
}
