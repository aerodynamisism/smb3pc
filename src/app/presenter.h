// Vulkan presenter: uploads each 256x240 RGBA frame to a texture and draws it
// to the swapchain with a nearest-filtered fullscreen triangle, fitted to
// keep square pixels and pillarboxed/letterboxed (no cropping).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;

class Presenter {
public:
    Presenter() = default;
    ~Presenter() { shutdown(); }
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // shader_dir holds the compiled present.vert.spv / present.frag.spv.
    bool init(SDL_Window* window, const std::string& shader_dir);
    // rgba: 256 * 240 pixels, R in the lowest byte of each word.
    // Returns false on an unrecoverable Vulkan error.
    bool present(const uint32_t* rgba);
    void shutdown();

private:
    bool create_instance_and_device();
    bool create_static_objects();
    bool create_swapchain();
    void destroy_swapchain_objects();
    void recreate_swapchain();
    uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags props) const;
    VkShaderModule load_shader(const std::string& file) const;

    SDL_Window* window_ = nullptr;
    std::string shader_dir_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore image_avail_ = VK_NULL_HANDLE;

    // Frame texture and its staging buffer (one frame in flight, so one copy).
    VkImage tex_ = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem_ = VK_NULL_HANDLE;
    VkImageView tex_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer stage_ = VK_NULL_HANDLE;
    VkDeviceMemory stage_mem_ = VK_NULL_HANDLE;
    void* stage_map_ = nullptr;

    VkDescriptorSetLayout dset_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout_ = VK_NULL_HANDLE;

    // Rebuilt whenever the swapchain is recreated.
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swap_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swap_extent_{0, 0};
    std::vector<VkImageView> swap_views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> render_done_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};
