#include "vulkan_api.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using qilin::gfx::VulkanApi;

constexpr VkExtent2D extent{16, 16};
constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr std::array<std::uint8_t, 4> expected{255, 0, 255, 255};
constexpr VkDeviceSize readback_size = extent.width * extent.height * expected.size();
constexpr char portability_subset[] = "VK_KHR_portability_subset";

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

template <typename T>
T info(VkStructureType type) {
    T result{};
    result.sType = type;
    return result;
}

template <typename T, typename Enumerate>
std::vector<T> enumerate(Enumerate read) {
    for (;;) {
        std::uint32_t count{};
        check(read(&count, static_cast<T*>(nullptr)), "Vulkan enumeration");
        if (count == 0) {
            return {};
        }

        std::vector<T> values(count);
        const auto result = read(&count, values.data());
        if (result != VK_INCOMPLETE) {
            check(result, "Vulkan enumeration");
            values.resize(count);
            return values;
        }
    }
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [name](const auto& property) {
        return name == property.extensionName;
    });
}

struct Adapter {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::uint32_t family{};
};

class Probe {
public:
    void run() {
        create_instance();
        adapter_ = select_adapter();
        create_device();
        create_targets();
        const auto command = record_commands();
        verify_pixels(command);

        const auto version = adapter_.properties.apiVersion;
        std::cout << "Vulkan 1.3 offscreen clear/readback passed on " << adapter_.properties.deviceName
                  << " (driver API " << VK_API_VERSION_MAJOR(version) << '.'
                  << VK_API_VERSION_MINOR(version) << '.' << VK_API_VERSION_PATCH(version) << ").\n";
    }

    ~Probe() {
        if (device_ && api_.vkDestroyDevice) {
            if (queue_pending_) api_.vkQueueWaitIdle(queue_);
            if (mapped_) api_.vkUnmapMemory(device_, readback_memory_);
            if (pool_) api_.vkDestroyCommandPool(device_, pool_, nullptr);
            if (readback_) api_.vkDestroyBuffer(device_, readback_, nullptr);
            if (view_) api_.vkDestroyImageView(device_, view_, nullptr);
            if (image_) api_.vkDestroyImage(device_, image_, nullptr);
            if (readback_memory_) api_.vkFreeMemory(device_, readback_memory_, nullptr);
            if (image_memory_) api_.vkFreeMemory(device_, image_memory_, nullptr);
            api_.vkDestroyDevice(device_, nullptr);
        }
        if (instance_ && api_.vkDestroyInstance) {
            api_.vkDestroyInstance(instance_, nullptr);
        }
    }

private:
    void create_instance() {
        std::uint32_t version{};
        check(api_.vkEnumerateInstanceVersion(&version), "vkEnumerateInstanceVersion");
        if (version < VK_API_VERSION_1_3) {
            throw std::runtime_error("the Vulkan loader must support Vulkan 1.3");
        }

        const auto extensions = enumerate<VkExtensionProperties>([this](auto* count, auto* values) {
            return api_.vkEnumerateInstanceExtensionProperties(nullptr, count, values);
        });
        const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

        auto app = info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "ApbArcade-QilinComp Vulkan probe";
        app.apiVersion = VK_API_VERSION_1_3;
        auto create = info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        create.pApplicationInfo = &app;
        if (has_extension(extensions, portability)) {
            create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &portability;
        }
        check(api_.vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance");
        api_.load(instance_);
    }

    [[nodiscard]] Adapter select_adapter() const {
        const auto devices = enumerate<VkPhysicalDevice>([this](auto* count, auto* values) {
            return api_.vkEnumeratePhysicalDevices(instance_, count, values);
        });
        for (const auto device : devices) {
            VkPhysicalDeviceProperties properties{};
            api_.vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) {
                continue;
            }

            auto v13 = info<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
            auto features = info<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
            features.pNext = &v13;
            api_.vkGetPhysicalDeviceFeatures2(device, &features);
            if (!v13.dynamicRendering || !v13.synchronization2) {
                continue;
            }

            VkImageFormatProperties image_properties{};
            if (api_.vkGetPhysicalDeviceImageFormatProperties(
                    device, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, image_usage, 0,
                    &image_properties) != VK_SUCCESS) {
                continue;
            }

            std::uint32_t count{};
            api_.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            if (count == 0) {
                continue;
            }
            std::vector<VkQueueFamilyProperties> families(count);
            api_.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
            for (std::uint32_t index = 0; index < count; ++index) {
                if (families[index].queueCount && (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    return {device, properties, index};
                }
            }
        }
        throw std::runtime_error("no Vulkan 1.3 graphics device supports dynamic rendering, "
                                 "synchronization2, and RGBA8 color attachments with transfer readback");
    }

    void create_device() {
        const auto extensions = enumerate<VkExtensionProperties>([this](auto* count, auto* values) {
            return api_.vkEnumerateDeviceExtensionProperties(adapter_.device, nullptr, count, values);
        });
        const char* portability = portability_subset;
        const float priority = 1.0f;
        auto queue = info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue.queueFamilyIndex = adapter_.family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        auto features = info<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        features.synchronization2 = VK_TRUE;
        features.dynamicRendering = VK_TRUE;
        auto create = info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        create.pNext = &features;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue;
        if (has_extension(extensions, portability)) {
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &portability;
        }
        check(api_.vkCreateDevice(adapter_.device, &create, nullptr, &device_), "vkCreateDevice");
        api_.load(device_);
        api_.vkGetDeviceQueue(device_, adapter_.family, 0, &queue_);
    }

    [[nodiscard]] VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags required,
                                          VkMemoryPropertyFlags preferred, bool* coherent = nullptr) const {
        VkPhysicalDeviceMemoryProperties memory{};
        api_.vkGetPhysicalDeviceMemoryProperties(adapter_.device, &memory);

        std::optional<std::uint32_t> selected;
        for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            const auto flags = memory.memoryTypes[index].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << index)) == 0 || (flags & required) != required) {
                continue;
            }
            if (!selected) {
                selected = index;
            }
            if ((flags & preferred) == preferred) {
                selected = index;
                break;
            }
        }
        if (!selected) {
            throw std::runtime_error("no suitable Vulkan memory type");
        }
        if (coherent) {
            *coherent = (memory.memoryTypes[*selected].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }

        auto alloc = info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = *selected;
        VkDeviceMemory result{};
        check(api_.vkAllocateMemory(device_, &alloc, nullptr, &result), "vkAllocateMemory");
        return result;
    }

    void create_targets() {
        auto image = info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = {extent.width, extent.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = image_usage;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(api_.vkCreateImage(device_, &image, nullptr, &image_), "vkCreateImage");

        VkMemoryRequirements requirements{};
        api_.vkGetImageMemoryRequirements(device_, image_, &requirements);
        image_memory_ = allocate(requirements, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(api_.vkBindImageMemory(device_, image_, image_memory_, 0), "vkBindImageMemory");

        auto view = info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view.image = image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = color_range;
        check(api_.vkCreateImageView(device_, &view, nullptr, &view_), "vkCreateImageView");

        auto buffer = info<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer.size = readback_size;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(api_.vkCreateBuffer(device_, &buffer, nullptr, &readback_), "vkCreateBuffer");
        api_.vkGetBufferMemoryRequirements(device_, readback_, &requirements);
        readback_memory_ = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &coherent_);
        check(api_.vkBindBufferMemory(device_, readback_, readback_memory_, 0), "vkBindBufferMemory");
    }

    [[nodiscard]] VkCommandBuffer record_commands() {
        auto pool = info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.queueFamilyIndex = adapter_.family;
        check(api_.vkCreateCommandPool(device_, &pool, nullptr, &pool_), "vkCreateCommandPool");
        auto alloc = info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(api_.vkAllocateCommandBuffers(device_, &alloc, &command), "vkAllocateCommandBuffers");

        auto begin = info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api_.vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");

        auto image_barrier = info<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = image_;
        image_barrier.subresourceRange = color_range;
        auto dependency = info<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &image_barrier;
        api_.vkCmdPipelineBarrier2(command, &dependency);

        auto attachment = info<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        attachment.imageView = view_;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color = {{1.0f, 0.0f, 1.0f, 1.0f}};
        auto rendering = info<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        api_.vkCmdBeginRendering(command, &rendering);
        api_.vkCmdEndRendering(command);

        image_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image_barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        api_.vkCmdPipelineBarrier2(command, &dependency);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        api_.vkCmdCopyImageToBuffer(command, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &region);

        auto host_barrier = info<VkBufferMemoryBarrier2>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
        host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = readback_;
        host_barrier.size = readback_size;
        dependency.imageMemoryBarrierCount = 0;
        dependency.pImageMemoryBarriers = nullptr;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &host_barrier;
        api_.vkCmdPipelineBarrier2(command, &dependency);

        check(api_.vkEndCommandBuffer(command), "vkEndCommandBuffer");
        return command;
    }

    void verify_pixels(VkCommandBuffer command) {
        auto submit = info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(api_.vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        queue_pending_ = true;
        check(api_.vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
        queue_pending_ = false;

        check(api_.vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
        if (!coherent_) {
            auto mapped_range = info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
            mapped_range.memory = readback_memory_;
            mapped_range.size = VK_WHOLE_SIZE;
            check(api_.vkInvalidateMappedMemoryRanges(device_, 1, &mapped_range), "vkInvalidateMappedMemoryRanges");
        }

        const auto* pixels = static_cast<const std::uint8_t*>(mapped_);
        for (std::size_t offset = 0; offset < readback_size; offset += expected.size()) {
            if (std::memcmp(pixels + offset, expected.data(), expected.size()) != 0) {
                throw std::runtime_error("Vulkan readback mismatch at pixel " + std::to_string(offset / expected.size()));
            }
        }
    }

    VulkanApi api_;
    VkInstance instance_ = VK_NULL_HANDLE;
    Adapter adapter_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    bool coherent_ = false;
    bool queue_pending_ = false;
};

} // namespace

int main() {
    try {
        Probe probe;
        probe.run();
    } catch (const std::exception& error) {
        std::cerr << "apb-vulkan-probe: " << error.what() << '\n';
        return 1;
    }
}
