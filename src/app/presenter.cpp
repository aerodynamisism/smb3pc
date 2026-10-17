#include "app/presenter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

// The picture texture is wide enough for 16:9; the 4:3 frame fills its first
// 256 columns and the presentation samples just those.
constexpr uint32_t kFrameW = 426;
constexpr uint32_t kFrameH = 240;
constexpr uint32_t kFrame4x3W = 256;  // 4:3 source width in the texture
// Overscan: 8 pixels are hidden on each side, so the 4:3 picture shown is 240 pixels wide.
constexpr uint32_t kVisibleW = 240;
constexpr VkDeviceSize kFrameBytes = kFrameW * kFrameH * 4;

// Source rectangle of the presentation, in texture pixels. Pushed to the
// fragment shader, which maps the fullscreen triangle's 0..1 UV onto it.
struct PresentRect {
    float x, y, w, h;
};

bool ok(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "Vulkan: %s failed (VkResult %d)\n", what, static_cast<int>(r));
    return false;
}

}  // namespace

// ---------------------------------------------------------------- setup

bool Presenter::init(SDL_Window* window, const std::string& shader_dir) {
    window_ = window;
    shader_dir_ = shader_dir;
    if (!create_instance_and_device()) return false;
    if (!create_static_objects()) return false;
    return create_swapchain();
}

bool Presenter::create_instance_and_device() {
    Uint32 ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!sdl_exts) {
        std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return false;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "smb3pc";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = ext_count;
    ici.ppEnabledExtensionNames = sdl_exts;
    if (!ok(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;

    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        std::fprintf(stderr, "SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return false;
    }

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());
    bool found = false;
    for (VkPhysicalDevice d : devices) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
        for (uint32_t i = 0; i < qn && !found; i++) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &can_present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                phys_ = d;
                queue_family_ = i;
                found = true;
            }
        }
        if (found) break;
    }
    if (!found) {
        std::fprintf(stderr, "no Vulkan device can draw to this window\n");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    if (!ok(vkCreateDevice(phys_, &dci, nullptr, &dev_), "vkCreateDevice")) return false;
    vkGetDeviceQueue(dev_, queue_family_, 0, &queue_);
    return true;
}

uint32_t Presenter::find_memory(uint32_t type_bits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

VkShaderModule Presenter::load_shader(const std::string& file) const {
    std::ifstream f(shader_dir_ + "/" + file, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open shader %s/%s\n", shader_dir_.c_str(), file.c_str());
        return VK_NULL_HANDLE;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4 != 0) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bytes.size();
    si.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (!ok(vkCreateShaderModule(dev_, &si, nullptr, &mod), "vkCreateShaderModule")) return VK_NULL_HANDLE;
    return mod;
}

bool Presenter::create_static_objects() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    if (!ok(vkCreateCommandPool(dev_, &pci, nullptr, &cmd_pool_), "vkCreateCommandPool")) return false;

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (!ok(vkAllocateCommandBuffers(dev_, &cai, &cmd_), "vkAllocateCommandBuffers")) return false;

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!ok(vkCreateFence(dev_, &fci, nullptr, &fence_), "vkCreateFence")) return false;

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!ok(vkCreateSemaphore(dev_, &sci, nullptr, &image_avail_), "vkCreateSemaphore")) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!ok(vkCreateSampler(dev_, &si, nullptr, &sampler_), "vkCreateSampler")) return false;

    // Frame texture: 256x240 RGBA8, sampled by the fragment shader.
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kFrameW, kFrameH, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ok(vkCreateImage(dev_, &ii, nullptr, &tex_), "vkCreateImage")) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev_, tex_, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (!ok(vkAllocateMemory(dev_, &mai, nullptr, &tex_mem_), "vkAllocateMemory(texture)")) return false;
    vkBindImageMemory(dev_, tex_, tex_mem_, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = tex_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!ok(vkCreateImageView(dev_, &vi, nullptr, &tex_view_), "vkCreateImageView")) return false;

    // Staging buffer, persistently mapped (host coherent).
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kFrameBytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!ok(vkCreateBuffer(dev_, &bi, nullptr, &stage_), "vkCreateBuffer")) return false;
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev_, stage_, &br);
    mai.allocationSize = br.size;
    mai.memoryTypeIndex = find_memory(br.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (!ok(vkAllocateMemory(dev_, &mai, nullptr, &stage_mem_), "vkAllocateMemory(staging)")) return false;
    vkBindBufferMemory(dev_, stage_, stage_mem_, 0);
    if (!ok(vkMapMemory(dev_, stage_mem_, 0, kFrameBytes, 0, &stage_map_), "vkMapMemory")) return false;

    // Descriptor set: one combined image sampler for the frame texture.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &binding;
    if (!ok(vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &dset_layout_), "vkCreateDescriptorSetLayout")) return false;

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &pool_size;
    if (!ok(vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_), "vkCreateDescriptorPool")) return false;

    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = dpool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &dset_layout_;
    if (!ok(vkAllocateDescriptorSets(dev_, &dsa, &dset_), "vkAllocateDescriptorSets")) return false;

    VkDescriptorImageInfo dii{sampler_, tex_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = dset_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_, 1, &write, 0, nullptr);

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dset_layout_;
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PresentRect);
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    return ok(vkCreatePipelineLayout(dev_, &pl, nullptr, &pipe_layout_), "vkCreatePipelineLayout");
}

// ---------------------------------------------------------------- swapchain

bool Presenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, fmts.data());
    if (fmts.empty()) return false;
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const VkSurfaceFormatKHR& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        ext.width = std::clamp(static_cast<uint32_t>(w), caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(static_cast<uint32_t>(h), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) return false;

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR comp = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & comp)) {
        comp = static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    }

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = image_count;
    sc.imageFormat = chosen.format;
    sc.imageColorSpace = chosen.colorSpace;
    sc.imageExtent = ext;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = comp;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync; always available
    sc.clipped = VK_TRUE;
    if (!ok(vkCreateSwapchainKHR(dev_, &sc, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    swap_format_ = chosen.format;
    swap_extent_ = ext;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, images.data());
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (!ok(vkCreateImageView(dev_, &vi, nullptr, &view), "vkCreateImageView(swapchain)")) return false;
        swap_views_.push_back(view);
    }

    VkAttachmentDescription att{};
    att.format = swap_format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (!ok(vkCreateRenderPass(dev_, &rpi, nullptr, &render_pass_), "vkCreateRenderPass")) return false;

    VkShaderModule vert = load_shader("present.vert.spv");
    VkShaderModule frag = load_shader("present.frag.spv");
    if (!vert || !frag) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pipe_layout_;
    gp.renderPass = render_pass_;
    gp.subpass = 0;
    bool pipe_ok = ok(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_),
                      "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(dev_, vert, nullptr);
    vkDestroyShaderModule(dev_, frag, nullptr);
    if (!pipe_ok) return false;

    for (VkImageView view : swap_views_) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = render_pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = swap_extent_.width;
        fi.height = swap_extent_.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (!ok(vkCreateFramebuffer(dev_, &fi, nullptr, &fb), "vkCreateFramebuffer")) return false;
        framebuffers_.push_back(fb);
    }

    // One render-finished semaphore per image, so a semaphore is never reused while
    // the presentation engine may still be waiting on it.
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < swap_views_.size(); i++) {
        VkSemaphore s = VK_NULL_HANDLE;
        if (!ok(vkCreateSemaphore(dev_, &sci, nullptr, &s), "vkCreateSemaphore(render_done)")) return false;
        render_done_.push_back(s);
    }
    return true;
}

void Presenter::destroy_swapchain_objects() {
    for (VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(dev_, fb, nullptr);
    framebuffers_.clear();
    for (VkImageView v : swap_views_) vkDestroyImageView(dev_, v, nullptr);
    swap_views_.clear();
    for (VkSemaphore s : render_done_) vkDestroySemaphore(dev_, s, nullptr);
    render_done_.clear();
    vkDestroyPipeline(dev_, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    vkDestroyRenderPass(dev_, render_pass_, nullptr);
    render_pass_ = VK_NULL_HANDLE;
    vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Presenter::recreate_swapchain() {
    vkDeviceWaitIdle(dev_);
    destroy_swapchain_objects();
    create_swapchain();
}

// ---------------------------------------------------------------- frame

bool Presenter::present(const uint32_t* rgba, int src_w) {
    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window_, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return true;  // minimized: nothing to show
    if (!swapchain_ && !create_swapchain()) return false;
    // Fullscreen and window-size changes do not always make the driver report the swapchain as
    // out of date. Without this check the picture keeps the old size and the rest stays black.
    if (static_cast<uint32_t>(win_w) != swap_extent_.width || static_cast<uint32_t>(win_h) != swap_extent_.height) {
        recreate_swapchain();
        if (!swapchain_) return false;
    }

    const bool wide = src_w == static_cast<int>(kFrameW);
    // Previous frame's work must finish before the staging buffer is reused.
    if (!ok(vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences")) return false;
    if (wide) {
        std::memcpy(stage_map_, rgba, kFrameBytes);
    } else {
        // The 4:3 picture fills the first src_w columns; the rest of the texture is unused.
        auto* dst = static_cast<uint8_t*>(stage_map_);
        for (uint32_t y = 0; y < kFrameH; y++) {
            std::memcpy(dst + y * kFrameW * 4u, rgba + static_cast<size_t>(y) * src_w,
                        static_cast<size_t>(src_w) * 4u);
        }
    }

    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, image_avail_, VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return true;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return ok(acq, "vkAcquireNextImageKHR");
    if (!ok(vkResetFences(dev_, 1, &fence_), "vkResetFences")) return false;

    if (!ok(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    // Texture: discard last frame's contents, copy staging -> image, make it readable.
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = tex_;
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_copy);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kFrameW, kFrameH, 1};
    vkCmdCopyBufferToImage(cmd_, stage_, tex_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_sample = to_copy;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_sample);

    // Fit inside the window: the largest scale that fits keeps square pixels.
    // The viewport is centred, so the clear colour pillarboxes/letterboxes
    // the unused area instead of cropping the picture.
    const float sw = static_cast<float>(swap_extent_.width);
    const float sh = static_cast<float>(swap_extent_.height);
    const float disp_w = wide ? static_cast<float>(kFrameW) : static_cast<float>(kVisibleW);
    const float scale = std::min(sw / disp_w, sh / kFrameH);
    const float vw = disp_w * scale;
    const float vh = kFrameH * scale;
    VkViewport viewport{(sw - vw) * 0.5f, (sh - vh) * 0.5f, vw, vh, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, swap_extent_};

    // Source rectangle of the texture this view shows: the whole wide picture,
    // or the 4:3 frame cropped to its 240 visible columns (8 pixels of
    // overscan hidden on each side).
    const PresentRect rect =
        wide ? PresentRect{0.0f, 0.0f, static_cast<float>(kFrameW), static_cast<float>(kFrameH)}
             : PresentRect{8.0f, 0.0f, static_cast<float>(kVisibleW), static_cast<float>(kFrameH)};

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass_;
    rbi.framebuffer = framebuffers_[idx];
    rbi.renderArea = {{0, 0}, swap_extent_};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_, 0, 1, &dset_, 0, nullptr);
    vkCmdPushConstants(cmd_, pipe_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rect), &rect);
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
    if (!ok(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer")) return false;

    VkSemaphore wait_sem = image_avail_;
    VkSemaphore signal_sem = render_done_[idx];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &wait_sem;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &signal_sem;
    if (!ok(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit")) return false;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &signal_sem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || acq == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
        return true;
    }
    return ok(pr, "vkQueuePresentKHR");
}

void Presenter::shutdown() {
    if (dev_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev_);
        destroy_swapchain_objects();
        vkDestroyPipelineLayout(dev_, pipe_layout_, nullptr);
        vkDestroyDescriptorPool(dev_, dpool_, nullptr);
        vkDestroyDescriptorSetLayout(dev_, dset_layout_, nullptr);
        vkDestroySampler(dev_, sampler_, nullptr);
        vkDestroyImageView(dev_, tex_view_, nullptr);
        vkDestroyImage(dev_, tex_, nullptr);
        vkFreeMemory(dev_, tex_mem_, nullptr);
        if (stage_map_) vkUnmapMemory(dev_, stage_mem_);
        vkDestroyBuffer(dev_, stage_, nullptr);
        vkFreeMemory(dev_, stage_mem_, nullptr);
        vkDestroySemaphore(dev_, image_avail_, nullptr);
        vkDestroyFence(dev_, fence_, nullptr);
        vkDestroyCommandPool(dev_, cmd_pool_, nullptr);
        vkDestroyDevice(dev_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }
    dev_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    stage_map_ = nullptr;
}
