#pragma once

#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <bit>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace qilin::gfx {

class VulkanLibrary {
public:
    VulkanLibrary() {
#if defined(_WIN32)
        handle_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#elif defined(__APPLE__)
        if (const char* path = std::getenv("APB_VULKAN_LIBRARY"); path && *path) {
            handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        } else {
            handle_ = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
            if (!handle_) {
                handle_ = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
            }
        }
#else
        handle_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle_) {
            throw std::runtime_error("could not load a Vulkan runtime");
        }
    }

    VulkanLibrary(const VulkanLibrary&) = delete;
    VulkanLibrary& operator=(const VulkanLibrary&) = delete;

    ~VulkanLibrary() {
#if defined(_WIN32)
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }

    [[nodiscard]] PFN_vkGetInstanceProcAddr entry_point() const {
#if defined(_WIN32)
        const auto symbol = GetProcAddress(handle_, "vkGetInstanceProcAddr");
#else
        const auto symbol = dlsym(handle_, "vkGetInstanceProcAddr");
#endif
        return std::bit_cast<PFN_vkGetInstanceProcAddr>(symbol);
    }

private:
#if defined(_WIN32)
    HMODULE handle_{};
#else
    void* handle_{};
#endif
};

// Keep the required Vulkan entry points together so each is declared and resolved once.
#define APB_VK_GLOBAL(X)                        \
    X(vkEnumerateInstanceVersion)                \
    X(vkEnumerateInstanceExtensionProperties)    \
    X(vkCreateInstance)

#define APB_VK_INSTANCE(X)                       \
    X(vkDestroyInstance)                         \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceFeatures2)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceImageFormatProperties)  \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)

#define APB_VK_DEVICE(X)                         \
    X(vkDestroyDevice)                           \
    X(vkGetDeviceQueue)                          \
    X(vkQueueSubmit)                             \
    X(vkQueueWaitIdle)                           \
    X(vkCreateImage)                             \
    X(vkDestroyImage)                            \
    X(vkGetImageMemoryRequirements)              \
    X(vkBindImageMemory)                         \
    X(vkCreateImageView)                         \
    X(vkDestroyImageView)                        \
    X(vkCreateBuffer)                            \
    X(vkDestroyBuffer)                           \
    X(vkGetBufferMemoryRequirements)             \
    X(vkBindBufferMemory)                        \
    X(vkAllocateMemory)                         \
    X(vkFreeMemory)                             \
    X(vkMapMemory)                              \
    X(vkUnmapMemory)                            \
    X(vkInvalidateMappedMemoryRanges)            \
    X(vkCreateCommandPool)                       \
    X(vkDestroyCommandPool)                      \
    X(vkAllocateCommandBuffers)                 \
    X(vkBeginCommandBuffer)                      \
    X(vkEndCommandBuffer)                        \
    X(vkCmdPipelineBarrier2)                     \
    X(vkCmdBeginRendering)                       \
    X(vkCmdEndRendering)                         \
    X(vkCmdCopyImageToBuffer)

class VulkanApi {
    VulkanLibrary library_;
    PFN_vkGetInstanceProcAddr get_instance_ = library_.entry_point();

    template <typename Function>
    static Function require(PFN_vkVoidFunction function, const char* name) {
        if (!function) {
            throw std::runtime_error(std::string("Vulkan runtime is missing ") + name);
        }
        return std::bit_cast<Function>(function);
    }

public:
    VulkanApi() {
        if (!get_instance_) {
            throw std::runtime_error("Vulkan runtime is missing vkGetInstanceProcAddr");
        }
#define X(name) name = require<PFN_##name>(get_instance_(VK_NULL_HANDLE, #name), #name);
        APB_VK_GLOBAL(X)
#undef X
    }

    void load(VkInstance instance) {
#define X(name) name = require<PFN_##name>(get_instance_(instance, #name), #name);
        APB_VK_INSTANCE(X)
#undef X
    }

    void load(VkDevice device) {
#define X(name) name = require<PFN_##name>(vkGetDeviceProcAddr(device, #name), #name);
        APB_VK_DEVICE(X)
#undef X
    }

#define X(name) PFN_##name name{};
    APB_VK_GLOBAL(X)
    APB_VK_INSTANCE(X)
    APB_VK_DEVICE(X)
#undef X
};

#undef APB_VK_GLOBAL
#undef APB_VK_INSTANCE
#undef APB_VK_DEVICE

} // namespace qilin::gfx
