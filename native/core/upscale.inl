// Render scale (all modes). Below 1 the scene renders at gRW x gRH into gU.col / gU.lz; a fullscreen pass then fills the shared images (Catmull-Rom colour,
// nearest linear depth, so Unity's particles and the IVA hand-off still test against full-size depth). Enhanced with TAA upsamples in the TAA pass and takes
// only the depth from here.
#include "fs_vert.h"
#include "upscale_frag.h"
static std::atomic<float> gRScale{1};
static int gRW, gRH; static bool gUp;   // this frame's scene size; gUp: smaller than the shared images
static struct {
    int w = 0, h = 0; bool fail = false; VImg col, lz;
    VkSampler smp = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout lay = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
} gU;

// Fullscreen triangle into n targets: RGBA8, then R32F.
static VkPipeline fsPipe(const uint32_t* code, size_t bytes, VkPipelineLayout lay, uint32_t n = 1) {
    VkShaderModule vs = shmod(fs_vert, sizeof fs_vert), fs = shmod(code, bytes);
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr } };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState ba[2]{}; ba[0].colorWriteMask = ba[1].colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = n; cb.pAttachments = ba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkFormat cf[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT };
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; ri.colorAttachmentCount = n; ri.pColorAttachmentFormats = cf;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st;
        pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = lay;
        if (vkCreateGraphicsPipelines(gVd, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    for (VkShaderModule m : { vs, fs }) if (m) vkDestroyShaderModule(gVd, m, nullptr);
    return p;
}

static bool upInit() {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK(vkCreateSampler(gVd, &sci, nullptr, &gU.smp));
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                                          { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 2; dli.pBindings = b;
    VKCHECK(vkCreateDescriptorSetLayout(gVd, &dli, nullptr, &gU.dsl));
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &gU.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(gVd, &pli, nullptr, &gU.lay));
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VKCHECK(vkCreateDescriptorPool(gVd, &dpi, nullptr, &gU.pool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = gU.pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &gU.dsl;
    VKCHECK(vkAllocateDescriptorSets(gVd, &dai, &gU.ds));
    return (gU.pipe = fsPipe(upscale_frag, sizeof upscale_frag, gU.lay, 2)) != VK_NULL_HANDLE;
}
static void upFree() { freeVImg(gU.col); freeVImg(gU.lz); gU.w = gU.h = 0; }
static bool upImgs(int w, int h) {
    if (!gU.pipe && !gU.fail && !upInit()) { gU.fail = true; logf("render scale: upscale pipeline failed, rendering at full size"); }
    if (gU.fail) return false;
    if (gU.w == w && gU.h == h) return true;
    vkQueueWaitIdle(gQ); upFree();
    const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!makeVImg(gU.col, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, u, VK_IMAGE_ASPECT_COLOR_BIT) || !makeVImg(gU.lz, VK_FORMAT_R32_SFLOAT, w, h, 1, 1, u, VK_IMAGE_ASPECT_COLOR_BIT)) {
        upFree(); gU.fail = true; logf("render scale: image allocation failed, rendering at full size"); return false;
    }
    gU.w = w; gU.h = h;
    VkDescriptorImageInfo ii[2] = { { gU.smp, gU.col.view, VK_IMAGE_LAYOUT_GENERAL }, { gU.smp, gU.lz.view, VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet wr[2]{};
    for (int i = 0; i < 2; i++) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = gU.ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(gVd, 2, wr, 0, nullptr);
    logf("render scale: scene %dx%d", w, h);
    return true;
}
// Render thread, before enhBegin: this frame's scene size.
static void upScale(const ImageSet& set) {
    float s = std::max(0.5f, gRScale.load());
    gRW = set.w; gRH = set.h;
    if (s < 0.999f) { if (upImgs(std::max(1, (int)lroundf(set.w * s)), std::max(1, (int)lroundf(set.h * s)))) gRW = gU.w, gRH = gU.h; }
    else if (gU.w) { vkQueueWaitIdle(gQ); upFree(); }
    gUp = gRW != set.w || gRH != set.h;
}
// Before the main pass: earlier frames are done reading the scene-size targets, which start undefined.
static void upBegin(VkCommandBuffer cb) {
    for (VImg* i : { &gU.col, &gU.lz })
        imgBarrier(cb, i->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}
// Upscale into the shared images (both left as attachments). depthOnly: the colour is already full size (TAAU).
static void upDraw(VkCommandBuffer cb, int si, ImageSet& set, bool depthOnly, VkImageLayout lzFrom) {
    const VkPipelineStageFlags src = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    imgBarrier(cb, gU.col.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, src, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    imgBarrier(cb, gU.lz.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, lzFrom, VK_IMAGE_LAYOUT_GENERAL, src, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkRenderingAttachmentInfo a[2]{};
    for (int i = 0; i < 2; i++) {
        a[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; a[i].imageView = i ? set.depth[si].view : depthOnly ? VK_NULL_HANDLE : set.color[si].view;
        a[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; a[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = { { 0, 0 }, { (uint32_t)set.w, (uint32_t)set.h } }; ri.layerCount = 1; ri.colorAttachmentCount = 2; ri.pColorAttachments = a;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp{ 0, 0, (float)set.w, (float)set.h, 0, 1 }; VkRect2D sc{ { 0, 0 }, { (uint32_t)set.w, (uint32_t)set.h } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gU.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gU.lay, 0, 1, &gU.ds, 0, nullptr);
    float pc[4] = { 1.f / set.w, 1.f / set.h, 0, 0 };
    vkCmdPushConstants(cb, gU.lay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}
static void upShutdown() {
    if (!gVd) return;
    upFree();
    if (gU.pipe) vkDestroyPipeline(gVd, gU.pipe, nullptr);
    if (gU.lay) vkDestroyPipelineLayout(gVd, gU.lay, nullptr);
    if (gU.pool) vkDestroyDescriptorPool(gVd, gU.pool, nullptr);
    if (gU.dsl) vkDestroyDescriptorSetLayout(gVd, gU.dsl, nullptr);
    if (gU.smp) vkDestroySampler(gVd, gU.smp, nullptr);
    gU.pipe = VK_NULL_HANDLE; gU.lay = VK_NULL_HANDLE; gU.pool = VK_NULL_HANDLE; gU.dsl = VK_NULL_HANDLE; gU.smp = VK_NULL_HANDLE;
}
