// The Vulkan 1.3 renderer (gfx.hpp). The Vulkan library is loaded at run time
// through volk, so there's nothing to link against and a machine without
// Vulkan gets a message.
//
// Shared with GBTGhoulies-Qilincomp, less depth and stencil.
//
// One command buffer records a frame. A render pass (dynamic rendering) on
// the back buffer is begun when the first thing needs it and ended before
// anything else uses the image. A clear of the whole buffer that arrives
// before the pass begins becomes the pass's load operation, which is what
// tile-based GPUs (Raspberry Pi, Apple) want; a clear inside a pass, or of a
// rectangle, is a vkCmdClearAttachments.
//
// A window, when there is one, gets each presented frame by a blit from the
// frame buffer into the acquired swapchain image: no shaders, and the blit
// converts to whatever format the window has.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gfx.hpp"
#include "volk.h"

// Vulkan structures are zero-initialised by naming only their sType
// (VkFoo f{VK_STRUCTURE_TYPE_FOO}), which -Wextra would flag in GCC and Clang.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace qilin::gfx {
namespace {

[[noreturn]] void fatal(const char* what, VkResult r) {
    std::fflush(stdout);
    std::fprintf(stderr, "qilin: Vulkan: %s failed (VkResult %d)\n", what, int(r));
    std::exit(3);
}
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) fatal(what, r);
}

struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
};

// The pipeline stages and accesses that go with an image layout, for barriers.
void layout_use(VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            access = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:  // the presentation engine waits on a semaphore, not a stage
            stage = VK_PIPELINE_STAGE_2_NONE;
            access = VK_ACCESS_2_NONE;
            break;
        default:  // undefined: nothing to wait for
            stage = VK_PIPELINE_STAGE_2_NONE;
            access = VK_ACCESS_2_NONE;
            break;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++*static_cast<uint32_t*>(user);
    std::fprintf(stderr, "qilin: Vulkan validation: %s\n", data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool has(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::ranges::any_of(list, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

class VulkanRenderer final : public Renderer {
public:
    ~VulkanRenderer() override { destroy(); }
    bool init(const Config& config, std::string* error);

    const std::string& device_name() const override { return device_name_; }
    const Config& config() const override { return config_; }
    void clear(const Rect* rects, uint32_t count, const float rgba[4]) override;
    void present() override;
    uint64_t frames_presented() const override { return frames_; }
    uint32_t back_index() const override { return back_; }
    uint32_t front_index() const override { return front_; }
    void flush() override {
        end_pass();
        submit();
    }
    void write_back_buffer(const uint32_t* bgra) override;
    void resize_window(uint32_t width, uint32_t height) override {
        config_.window_width = width;
        config_.window_height = height;
        swapchain_stale_ = true;
    }
    Image read_front_buffer() override { return read(color_[front_]); }
    Image read_back_buffer() override { return read(color_[back_]); }
    uint32_t validation_errors() const override { return validation_errors_; }

private:
    bool create_instance(std::string* error);
    bool pick_device(std::string* error);
    void create_device();
    Target create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const;
    void destroy();

    void record();                                   // makes sure the frame's command buffer is recording
    void transition(Target& t, VkImageLayout to);
    void begin_pass();                               // makes sure the render pass is open, applying pending clears
    void end_pass();
    // Ends the command buffer, submits it and waits for it; optionally waiting
    // on one semaphore at the transfer stage and signalling another.
    void submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    Image read(Target& t);
    VkBuffer host_buffer(VkBufferUsageFlags usage, VkDeviceSize bytes, VkDeviceMemory* memory);
    void create_swapchain();
    void destroy_swapchain();
    void show(Target& frame);                        // blits a frame to the window and presents it

    Config config_;
    std::string device_name_;
    uint32_t validation_errors_ = 0;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    bool portability_subset_ = false;
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    std::vector<Target> color_;  // the flip chain: back_ is drawn to, front_ was presented last
    uint32_t back_ = 0, front_ = 0;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkBuffer upload_ = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
    void* upload_mapped_ = nullptr;

    // The window, if any.
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkSemaphore> rendered_;   // one per swapchain image: the blit is done, present it
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    bool swapchain_stale_ = false;

    bool recording_ = false, pass_open_ = false;
    struct PendingClear {  // a whole-buffer clear waiting to become the pass's load operation
        bool color = false;
        VkClearColorValue color_value{};
    } pending_;
    uint64_t frames_ = 0;
};

bool VulkanRenderer::create_instance(std::string* error) {
    if (volkInitialize() != VK_SUCCESS) {
        *error = "no Vulkan library found: install a graphics driver with Vulkan 1.3 (on macOS, MoltenVK)";
        return false;
    }
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        *error = "the Vulkan library is older than Vulkan 1.3: update the graphics driver";
        return false;
    }
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
    available.resize(n);
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    layers.resize(n);

    std::vector<const char*> extensions, enabled_layers;
    for (const std::string& e : config_.instance_extensions) {
        if (!has(available, e.c_str())) {
            *error = "the Vulkan library has no " + e + ", which the window needs";
            return false;
        }
        extensions.push_back(e.c_str());
    }
    VkInstanceCreateFlags flags = 0;
    // MoltenVK is a "portability" implementation, which the loader hides unless asked.
    if (has(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    const char* env = std::getenv("QILIN_VK_VALIDATION");
    bool validate = config_.validation || (env && *env && *env != '0');
    static constexpr char kValidation[] = "VK_LAYER_KHRONOS_validation";
    validate = validate && has(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
               std::ranges::any_of(layers, [](const VkLayerProperties& l) { return std::strcmp(l.layerName, kValidation) == 0; });
    if (validate) {
        enabled_layers.push_back(kValidation);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "qilin";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = uint32_t(enabled_layers.size());
    info.ppEnabledLayerNames = enabled_layers.data();
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (const VkResult r = vkCreateInstance(&info, nullptr, &instance_); r != VK_SUCCESS) {
        *error = "can't create a Vulkan instance (VkResult " + std::to_string(int(r)) + ")";
        return false;
    }
    volkLoadInstance(instance_);
    if (config_.create_surface) {
        surface_ = reinterpret_cast<VkSurfaceKHR>(config_.create_surface(instance_));
        if (surface_ == VK_NULL_HANDLE) {
            *error = "can't make a Vulkan surface for the window";
            return false;
        }
    }
    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        m.pfnUserCallback = on_debug_message;
        m.pUserData = &validation_errors_;
        check(vkCreateDebugUtilsMessengerEXT(instance_, &m, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
    }
    return true;
}

// The best device that has what the renderer needs: Vulkan 1.3 with dynamic
// rendering and synchronization2 (both required by 1.3) and a graphics queue.
// A discrete GPU beats an integrated one, which beats a software renderer.
bool VulkanRenderer::pick_device(std::string* error) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());
    devices.resize(n);
    if (devices.empty()) {
        *error = "Vulkan found no graphics device";
        return false;
    }
    std::string rejected;
    int best_score = -1;
    for (uint32_t i = 0; i < devices.size(); ++i) {
        if (config_.gpu >= 0 && uint32_t(config_.gpu) != i) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        std::string why;
        uint32_t family = UINT32_MAX;
        if (props.apiVersion < VK_API_VERSION_1_3) {
            why = "its driver has Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
                  std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ", not 1.3";
        } else {
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(devices[i], &f2);
            if (!f13.dynamicRendering) why = "no dynamic rendering";
            else if (!f13.synchronization2) why = "no synchronization2";
            uint32_t q = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q, nullptr);
            std::vector<VkQueueFamilyProperties> families(q);
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q, families.data());
            for (uint32_t k = 0; k < q && family == UINT32_MAX; ++k) {
                VkBool32 presents = VK_TRUE;
                if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], k, surface_, &presents);
                if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) family = k;
            }
            if (why.empty() && family == UINT32_MAX) why = surface_ ? "no graphics queue that can present to the window" : "no graphics queue";
            if (why.empty() && surface_) {
                uint32_t e = 0;
                vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &e, nullptr);
                std::vector<VkExtensionProperties> exts(e);
                vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &e, exts.data());
                exts.resize(e);
                if (!has(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) why = "no swapchains";
            }
        }
        if (!why.empty()) {
            rejected += std::string(rejected.empty() ? "" : "; ") + props.deviceName + ": " + why;
            continue;
        }
        int score = 1;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 4;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 3;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 2;
        if (score > best_score) {
            best_score = score;
            gpu_ = devices[i];
            queue_family_ = family;
            device_name_ = props.deviceName;
        }
    }
    if (gpu_ == VK_NULL_HANDLE) {
        *error = config_.gpu >= 0 && uint32_t(config_.gpu) >= devices.size()
                     ? "there is no Vulkan device number " + std::to_string(config_.gpu)
                     : "no graphics device can run the renderer (" + rejected + ")";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memory_);
    uint32_t e = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &e, nullptr);
    std::vector<VkExtensionProperties> extensions(e);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &e, extensions.data());
    extensions.resize(e);
    portability_subset_ = has(extensions, "VK_KHR_portability_subset");  // must be enabled where it's offered (MoltenVK)
    return true;
}

void VulkanRenderer::create_device() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = queue_family_;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    std::vector<const char*> extensions;
    if (portability_subset_) extensions.push_back("VK_KHR_portability_subset");
    if (surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(gpu_, &info, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    check(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &alloc, &cmd_), "vkAllocateCommandBuffers");
    const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
}

uint32_t VulkanRenderer::memory_type(uint32_t allowed, VkMemoryPropertyFlags wanted) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    fatal("finding a memory type", VK_ERROR_FEATURE_NOT_PRESENT);
}

Target VulkanRenderer::create_target(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Target t;
    t.aspect = aspect;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {config_.width, config_.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &info, nullptr, &t.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, &t.memory), "vkAllocateMemory");
    check(vkBindImageMemory(device_, t.image, t.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &view, nullptr, &t.view), "vkCreateImageView");
    return t;
}

bool VulkanRenderer::init(const Config& config, std::string* error) {
    config_ = config;
    if (config_.width == 0 || config_.height == 0 || config_.width > 16384 || config_.height > 16384 || config_.back_buffers == 0 ||
        config_.back_buffers > 3) {
        *error = "bad frame buffer size or count";
        return false;
    }
    if (!create_instance(error) || !pick_device(error)) return false;
    create_device();

    // B8G8R8A8: the swapchain format every target offers.
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (uint32_t i = 0; i < config_.back_buffers + 1; ++i)
        color_.push_back(create_target(VK_FORMAT_B8G8R8A8_UNORM, color_usage, VK_IMAGE_ASPECT_COLOR_BIT));
    back_ = 0;
    front_ = uint32_t(color_.size()) - 1;

    // Every buffer starts as zeros, so a frame never shows what the GPU's memory held before.
    record();
    const VkClearColorValue zero_color{};
    for (Target& t : color_) {
        transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{t.aspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd_, t.image, t.layout, &zero_color, 1, &range);
    }
    submit();
    if (surface_) {
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &info, nullptr, &acquired_), "vkCreateSemaphore");
        create_swapchain();
    }
    return true;
}

void VulkanRenderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) fatal("a window that can be blitted to", VK_ERROR_FEATURE_NOT_PRESENT);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    formats.resize(n);
    // The frame's own format where offered (a straight copy of its values),
    // else the first format a blit can write.
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : formats) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(gpu_, f.format, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (chosen.format == VK_FORMAT_UNDEFINED || f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) chosen = f;
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) break;
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) fatal("finding a window format a blit can write", VK_ERROR_FORMAT_NOT_SUPPORTED);

    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &n, modes.data());
    modes.resize(n);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // always offered
    if (!config_.vsync) {
        if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {  // the window takes the swapchain's size
        extent.width = std::clamp(config_.window_width ? config_.window_width : config_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(config_.window_height ? config_.window_height : config_.height, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    swapchain_stale_ = false;
    if (extent.width == 0 || extent.height == 0) return;  // minimised: nothing to show until it's back

    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                          : caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    check(vkCreateSwapchainKHR(device_, &info, nullptr, &created), "vkCreateSwapchainKHR");
    destroy_swapchain();
    swapchain_ = created;
    swapchain_format_ = chosen.format;
    swapchain_extent_ = extent;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapchain_images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());
    const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    rendered_.resize(n);
    for (VkSemaphore& s : rendered_) check(vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore");
}

void VulkanRenderer::destroy_swapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
    rendered_.clear();
    swapchain_images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanRenderer::show(Target& frame) {
    if (swapchain_stale_) create_swapchain();
    if (!swapchain_) return;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        create_swapchain();
        if (!swapchain_) return;
        r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return;   // still changing: try again next frame
    if (r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");

    record();
    transition(frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImage image = swapchain_images_[index];
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                       VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &dep);
    };
    // The acquire semaphore is waited on at the transfer stage, so the
    // layout change chains after it.
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    // Scale: the largest whole number that fits, or below 1x, the largest fit.
    const double sx = double(swapchain_extent_.width) / config_.width, sy = double(swapchain_extent_.height) / config_.height;
    double scale = std::min(sx, sy);
    const bool whole = scale >= 1.0;
    if (whole) scale = std::floor(scale);
    const auto w = std::max<int32_t>(1, int32_t(config_.width * scale)), h = std::max<int32_t>(1, int32_t(config_.height * scale));
    const int32_t x = (int32_t(swapchain_extent_.width) - w) / 2, y = (int32_t(swapchain_extent_.height) - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(config_.width), int32_t(config_.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    vkCmdBlitImage(cmd_, frame.image, frame.layout, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   whole ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    submit(acquired_, rendered_[index]);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_stale_ = true;
    else check(r, "vkQueuePresentKHR");
}

VkBuffer VulkanRenderer::host_buffer(VkBufferUsageFlags usage, VkDeviceSize bytes, VkDeviceMemory* memory) {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &alloc, nullptr, memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device_, buffer, *memory, 0), "vkBindBufferMemory");
    return buffer;
}

void VulkanRenderer::write_back_buffer(const uint32_t* bgra) {
    const VkDeviceSize bytes = VkDeviceSize(config_.width) * config_.height * 4;
    if (!upload_) {
        upload_ = host_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bytes, &upload_memory_);
        check(vkMapMemory(device_, upload_memory_, 0, bytes, 0, &upload_mapped_), "vkMapMemory");
    }
    // Everything before is overwritten: a pending clear is dropped, an open pass ended.
    pending_ = {};
    end_pass();
    std::memcpy(upload_mapped_, bgra, size_t(bytes));  // the previous copy has finished: every submit waits
    record();
    Target& back = color_[back_];
    transition(back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {config_.width, config_.height, 1};
    vkCmdCopyBufferToImage(cmd_, upload_, back.image, back.layout, 1, &region);
}

void VulkanRenderer::destroy() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        auto drop = [&](Target& t) {
            if (t.view) vkDestroyImageView(device_, t.view, nullptr);
            if (t.image) vkDestroyImage(device_, t.image, nullptr);
            if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        };
        destroy_swapchain();
        if (acquired_) vkDestroySemaphore(device_, acquired_, nullptr);
        for (Target& t : color_) drop(t);
        if (upload_) vkDestroyBuffer(device_, upload_, nullptr);
        if (upload_memory_) vkFreeMemory(device_, upload_memory_, nullptr);
        if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
        if (readback_memory_) vkFreeMemory(device_, readback_memory_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}

void VulkanRenderer::record() {
    if (recording_) return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");
    recording_ = true;
}

void VulkanRenderer::transition(Target& t, VkImageLayout to) {
    if (t.layout == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    layout_use(t.layout, b.srcStageMask, b.srcAccessMask);
    layout_use(to, b.dstStageMask, b.dstAccessMask);
    b.oldLayout = t.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {t.aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd_, &dep);
    t.layout = to;
}

void VulkanRenderer::begin_pass() {
    if (pass_open_) return;
    record();
    Target& back = color_[back_];
    transition(back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = back.view;
    color.imageLayout = back.layout;
    color.loadOp = pending_.color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = pending_.color_value;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {config_.width, config_.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd_, &info);
    pass_open_ = true;
    pending_ = {};
}

void VulkanRenderer::end_pass() {
    if (pending_.color) begin_pass();  // a clear nothing drew after still happens
    if (!pass_open_) return;
    vkCmdEndRendering(cmd_);
    pass_open_ = false;
}

void VulkanRenderer::clear(const Rect* rects, uint32_t count, const float rgba[4]) {
    // Clip the rectangles to the buffer; no rectangles means all of it.
    std::vector<VkClearRect> clipped;
    bool whole = count == 0;
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t x0 = std::max<int64_t>(rects[i].x, 0), y0 = std::max<int64_t>(rects[i].y, 0);
        const int64_t x1 = std::min<int64_t>(int64_t(rects[i].x) + rects[i].w, config_.width);
        const int64_t y1 = std::min<int64_t>(int64_t(rects[i].y) + rects[i].h, config_.height);
        if (x1 <= x0 || y1 <= y0) continue;
        whole = whole || (x0 == 0 && y0 == 0 && x1 == config_.width && y1 == config_.height);
        clipped.push_back({{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    if (!whole && clipped.empty()) return;
    VkClearColorValue color{};
    for (int k = 0; k < 4; ++k) color.float32[k] = rgba[k];

    if (whole && !pass_open_) {  // becomes the load operation of the pass that follows
        pending_.color = true;
        pending_.color_value = color;
        return;
    }
    begin_pass();
    if (whole) clipped.assign(1, {{{0, 0}, {config_.width, config_.height}}, 0, 1});
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = color;
    vkCmdClearAttachments(cmd_, 1, &attachment, uint32_t(clipped.size()), clipped.data());
}

void VulkanRenderer::submit(VkSemaphore wait, VkSemaphore signal) {
    if (!recording_) return;
    check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
    recording_ = false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd_;
    VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, signals{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits.semaphore = wait;
    waits.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    signals.semaphore = signal;
    signals.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.waitSemaphoreInfoCount = wait ? 1 : 0;
    info.pWaitSemaphoreInfos = &waits;
    info.signalSemaphoreInfoCount = signal ? 1 : 0;
    info.pSignalSemaphoreInfos = &signals;
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &cb;
    check(vkQueueSubmit2(queue_, 1, &info, fence_), "vkQueueSubmit2");
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(device_, 1, &fence_), "vkResetFences");
}

void VulkanRenderer::present() {
    end_pass();
    if (surface_) show(color_[back_]);
    submit();
    front_ = back_;
    back_ = (back_ + 1) % uint32_t(color_.size());
    ++frames_;
}

Image VulkanRenderer::read(Target& t) {
    const VkDeviceSize bytes = VkDeviceSize(config_.width) * config_.height * 4;
    if (!readback_) readback_ = host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, bytes, &readback_memory_);
    end_pass();
    record();
    transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {config_.width, config_.height, 1};
    vkCmdCopyImageToBuffer(cmd_, t.image, t.layout, readback_, 1, &region);
    submit();

    Image out;
    out.width = config_.width;
    out.height = config_.height;
    out.rgba.resize(size_t(bytes));
    void* mapped = nullptr;
    check(vkMapMemory(device_, readback_memory_, 0, bytes, 0, &mapped), "vkMapMemory");
    const uint8_t* bgra = static_cast<const uint8_t*>(mapped);
    for (size_t i = 0; i < size_t(bytes); i += 4) {
        out.rgba[i] = bgra[i + 2];
        out.rgba[i + 1] = bgra[i + 1];
        out.rgba[i + 2] = bgra[i];
        out.rgba[i + 3] = bgra[i + 3];
    }
    vkUnmapMemory(device_, readback_memory_);
    return out;
}

}  // namespace

std::unique_ptr<Renderer> Renderer::create(const Config& config, std::string* error) {
    std::string local;
    auto r = std::make_unique<VulkanRenderer>();
    if (!r->init(config, error ? error : &local)) return nullptr;
    return r;
}

}  // namespace qilin::gfx
