#include "render/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <vector>

namespace ss2 {
namespace {
constexpr auto transfer = vk::ImageUsageFlagBits::eTransferDst;
constexpr auto transferStage = vk::PipelineStageFlagBits::eTransfer;
constexpr std::uint64_t acquireTimeoutNs = 50'000'000;
constexpr char validationLayer[] = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* data) {
    static_cast<ValidationReport*>(data)->errors.fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
    return VK_FALSE;
}

struct DebugMessenger {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy{};
    ~DebugMessenger() {
        if (messenger) destroy(instance, messenger, nullptr);
    }
};
}

struct VulkanWindow::Impl {
    SDL_Window* window;
    vk::UniqueInstance instance;
    DebugMessenger debug;
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice gpu;
    uint32_t family{};
    vk::UniqueDevice device;
    vk::Queue queue;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer command;
    vk::UniqueFence submitted;
    vk::UniqueSemaphore acquired;
    vk::UniqueSwapchainKHR chain;
    std::vector<vk::Image> images;
    // Presentation can outlive the submit fence. Reuse each signal semaphore
    // only after its own swapchain image has been acquired again.
    std::vector<vk::UniqueSemaphore> rendered;
    vk::Extent2D activeExtent;
    bool dirty = true;

    Impl(SDL_Window* w, ValidationReport* report) : window(w) {
        uint32_t count{};
        auto extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!extensions) throw std::runtime_error(SDL_GetError());
        vk::ApplicationInfo app("Sudden Strike II native", 1, "ss2", 1, VK_API_VERSION_1_3);
        std::vector<const char*> names(extensions, extensions + count);
        const char* layer = validationLayer;
        VkDebugUtilsMessengerCreateInfoEXT callback{};
        callback.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        callback.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        callback.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        callback.pfnUserCallback = onValidation;
        callback.pUserData = report;
        if (report) {
            const auto layers = vk::enumerateInstanceLayerProperties();
            if (std::ranges::none_of(layers, [](const auto& props) {
                return std::strcmp(props.layerName, validationLayer) == 0;
            })) throw std::runtime_error("Required VK_LAYER_KHRONOS_validation is unavailable");
            names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        vk::InstanceCreateInfo create({}, &app, report ? 1u : 0u, report ? &layer : nullptr, static_cast<uint32_t>(names.size()), names.data());
        if (report) create.pNext = &callback;
        instance = vk::createInstanceUnique(create);
        if (report) {
            debug.instance = *instance;
            auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                instance->getProcAddr("vkCreateDebugUtilsMessengerEXT"));
            debug.destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                instance->getProcAddr("vkDestroyDebugUtilsMessengerEXT"));
            if (!make || !debug.destroy || make(*instance, &callback, nullptr, &debug.messenger) != VK_SUCCESS)
                throw std::runtime_error("Could not install Vulkan validation callback");
            std::cout << "Enabled " << validationLayer << '\n';
        }
        VkSurfaceKHR handle{};
        if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &handle))
            throw std::runtime_error(SDL_GetError());
        surface = vk::UniqueSurfaceKHR(handle, *instance);
        selectDevice();
        const float priority = 1;
        vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
        const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        device = gpu.createDeviceUnique({{}, 1, &queueInfo, 0, nullptr, 1, &swapchainExtension});
        queue = device->getQueue(family, 0);
        pool = device->createCommandPoolUnique({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family});
        command = device->allocateCommandBuffers({*pool, vk::CommandBufferLevel::ePrimary, 1}).front();
        submitted = device->createFenceUnique({vk::FenceCreateFlagBits::eSignaled});
        acquired = device->createSemaphoreUnique({});
    }

    void selectDevice() {
        int best = -1;
        for (auto candidate : instance->enumeratePhysicalDevices()) {
            const auto props = candidate.getProperties();
            if (props.apiVersion < VK_API_VERSION_1_3) continue;
            const auto extensions = candidate.enumerateDeviceExtensionProperties();
            if (std::ranges::none_of(extensions, [](const auto& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            })) continue;
            if (!(candidate.getSurfaceCapabilitiesKHR(*surface).supportedUsageFlags & transfer) ||
                candidate.getSurfaceFormatsKHR(*surface).empty()) continue;
            const auto families = candidate.getQueueFamilyProperties();
            for (uint32_t i = 0; i < families.size(); ++i) {
                if (!(families[i].queueFlags & vk::QueueFlagBits::eGraphics) ||
                    !candidate.getSurfaceSupportKHR(i, *surface)) continue;
                const int score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 2 :
                                  props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu ? 1 : 0;
                if (score > best) {
                    best = score;
                    gpu = candidate;
                    family = i;
                }
                break;
            }
        }
        if (!gpu) throw std::runtime_error("Need Vulkan 1.3, a graphics/present queue and transfer-capable swapchain");
        const auto info = gpu.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
        std::cout << "SDL " << SDL_GetCurrentVideoDriver() << " | "
                  << info.get<vk::PhysicalDeviceProperties2>().properties.deviceName << " | "
                  << info.get<vk::PhysicalDeviceDriverProperties>().driverName << " | "
                  << info.get<vk::PhysicalDeviceDriverProperties>().driverInfo << '\n';
    }

    bool rebuild() {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) throw std::runtime_error(SDL_GetError());
        if (width <= 0 || height <= 0) return false;
        const auto caps = gpu.getSurfaceCapabilitiesKHR(*surface);
        auto extent = caps.currentExtent;
        if (extent.width == std::numeric_limits<uint32_t>::max()) {
            extent.width = std::clamp(uint32_t(width), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(height), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!extent.width || !extent.height) return false;
        const auto formats = gpu.getSurfaceFormatsKHR(*surface);
        if (formats.empty()) throw std::runtime_error("Surface has no swapchain format");
        auto format = formats.front();
        for (const auto preferred : {vk::Format::eB8G8R8A8Srgb, vk::Format::eR8G8B8A8Srgb,
                                     vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm}) {
            const auto it = std::ranges::find_if(formats, [preferred](const auto& f) {
                return f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear &&
                       (f.format == preferred || f.format == vk::Format::eUndefined);
            });
            if (it != formats.end()) {
                format = {preferred, it->colorSpace};
                break;
            }
        }
        if (format.format == vk::Format::eUndefined) format.format = vk::Format::eB8G8R8A8Unorm;
        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        auto alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        for (auto mode : {alpha, vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
                          vk::CompositeAlphaFlagBitsKHR::ePostMultiplied, vk::CompositeAlphaFlagBitsKHR::eInherit}) {
            if (caps.supportedCompositeAlpha & mode) { alpha = mode; break; }
        }
        device->waitIdle();
        vk::SwapchainCreateInfoKHR create({}, *surface, count, format.format, format.colorSpace,
            extent, 1, transfer, vk::SharingMode::eExclusive, 0, nullptr, caps.currentTransform,
            alpha, vk::PresentModeKHR::eFifo, true, chain.get());
        chain = device->createSwapchainKHRUnique(create);
        images = device->getSwapchainImagesKHR(*chain);
        rendered.clear();
        for (size_t i = 0; i < images.size(); ++i) rendered.push_back(device->createSemaphoreUnique({}));
        activeExtent = extent;
        dirty = false;
        std::cout << "Swapchain " << extent.width << 'x' << extent.height << " FIFO, "
                  << images.size() << " images, " << vk::to_string(format.format) << ", " << vk::to_string(format.colorSpace) << '\n';
        return true;
    }

    void record(vk::Image image) {
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
        const vk::ClearColorValue color(std::array<float, 4>{0.055f, 0.12f, 0.16f, 1.f});
        command.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, range);
        barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
               .setNewLayout(vk::ImageLayout::ePresentSrcKHR)
               .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask({});
        command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, barrier);
        command.end();
    }

    bool present() {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) return false;
        try {
            if (dirty && !rebuild()) return false;
            if (device->waitForFences(*submitted, true, acquireTimeoutNs) != vk::Result::eSuccess) return false;
            auto [status, index] = device->acquireNextImageKHR(*chain, acquireTimeoutNs, *acquired);
            if (status == vk::Result::eTimeout || status == vk::Result::eNotReady) return false;
            record(images.at(index));
            vk::PipelineStageFlags waitStage = transferStage;
            vk::SubmitInfo submit(1, &*acquired, &waitStage, 1, &command, 1, &*rendered[index]);
            device->resetFences(*submitted);
            queue.submit(submit, *submitted);
            const auto result = queue.presentKHR({1, &*rendered[index], 1, &*chain, &index});
            dirty = status == vk::Result::eSuboptimalKHR || result == vk::Result::eSuboptimalKHR;
            return true;
        } catch (const vk::OutOfDateKHRError&) {
            dirty = true;
            return false;
        }
    }

    ~Impl() {
        // Destruction also covers a failed present. A lost device can still be destroyed.
        if (device) (void)vkDeviceWaitIdle(*device);
    }
};

VulkanWindow::VulkanWindow(SDL_Window* w, ValidationReport* report) : impl_(std::make_unique<Impl>(w, report)) {}
VulkanWindow::~VulkanWindow() = default;
void VulkanWindow::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event.window.windowID == SDL_GetWindowID(impl_->window)) impl_->dirty = true;
}
std::pair<unsigned, unsigned> VulkanWindow::extent() const {
    return {impl_->activeExtent.width, impl_->activeExtent.height};
}
bool VulkanWindow::present(const Snapshot&) { return impl_->present(); }
}
