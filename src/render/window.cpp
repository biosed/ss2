#include "render/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <utility>
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
    vk::UniqueDeviceMemory mapMemory;
    vk::UniqueImage map;
    vk::Extent2D mapSize;
    vk::UniqueDeviceMemory outlineMemory;
    vk::UniqueBuffer outline;
    std::vector<MapRect> selected;
    struct MapPatch { vk::DeviceSize offset; int x, y; unsigned width, height; };
    std::vector<MapPatch> patches;
    std::vector<std::uint8_t> patchPixels;
    vk::UniqueDeviceMemory patchMemory;
    vk::UniqueBuffer patchBuffer;
    vk::DeviceSize patchCapacity{};
    vk::UniqueDeviceMemory overlayMemory;
    vk::UniqueBuffer overlayBuffer;
    vk::DeviceSize overlayCapacity{};
    std::vector<std::uint8_t> overlayPixels;
    unsigned overlayW{}, overlayH{};
    bool overlayStale = false;
    // Transform of the last recorded frame, including any letterboxing.
    double screenLeft{}, screenTop{}, scaleX = 1.0, scaleY = 1.0;
    double centerX{}, centerY{}, scale = 1.0;
    vk::Format surfaceFormat{};
    bool transformReady = false;

    std::pair<double, double> windowScale() const {
        int w{}, h{};
        if (!SDL_GetWindowSize(window, &w, &h) || w <= 0 || h <= 0) return {1.0, 1.0};
        return {double(activeExtent.width) / w, double(activeExtent.height) / h};
    }

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
        if (map && !(gpu.getFormatProperties(format.format).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst))
            throw std::runtime_error("Surface format cannot display the map");
        if (map && surfaceFormat != format.format)
            throw std::runtime_error("Map surface format changed during resize");
        surfaceFormat = format.format;
        activeExtent = extent;
        overlayStale = true; // Stored in the surface's byte order.
        dirty = false;
        std::cout << "Swapchain " << extent.width << 'x' << extent.height << " FIFO, "
                  << images.size() << " images, " << vk::to_string(format.format) << ", " << vk::to_string(format.colorSpace) << '\n';
        return true;
    }

    vk::UniqueDeviceMemory allocate(vk::MemoryRequirements requirements, vk::MemoryPropertyFlags flags) {
        const auto properties = gpu.getMemoryProperties();
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return device->allocateMemoryUnique({requirements.size, i});
        }
        throw std::runtime_error("No compatible Vulkan map memory");
    }

    void setMap(const assets::MapImage& data) {
        const auto limit = gpu.getProperties().limits.maxImageDimension2D;
        if (!data.width || !data.height || data.width > limit || data.height > limit ||
            uint64_t(data.width) * data.height * 4 != data.rgba.size())
            throw std::runtime_error("Unsupported map dimensions or pixel count");
        if (dirty && !rebuild()) throw std::runtime_error("Map needs a drawable window");
        const bool srgb = surfaceFormat == vk::Format::eR8G8B8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Srgb;
        const bool unorm = surfaceFormat == vk::Format::eR8G8B8A8Unorm || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!srgb && !unorm) throw std::runtime_error("Map requires an RGBA8 or BGRA8 surface");
        const auto format = unorm ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
        if (!(gpu.getFormatProperties(format).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc) ||
            !(gpu.getFormatProperties(surfaceFormat).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst))
            throw std::runtime_error("GPU cannot blit map pixels to the window");
        device->waitIdle();
        vk::UniqueDeviceMemory stagingMemory;
        auto staging = device->createBufferUnique({{}, data.rgba.size(), vk::BufferUsageFlagBits::eTransferSrc});
        stagingMemory = allocate(device->getBufferMemoryRequirements(*staging),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device->bindBufferMemory(*staging, *stagingMemory, 0);
        auto mapped = device->mapMemory(*stagingMemory, 0, data.rgba.size());
        std::memcpy(mapped, data.rgba.data(), data.rgba.size());
        device->unmapMemory(*stagingMemory);
        if (!outline) {
            constexpr size_t outlineBytes = 8192 * 4;
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, outlineBytes, vk::BufferUsageFlagBits::eTransferSrc});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            auto* pixels = static_cast<std::uint8_t*>(device->mapMemory(*memory, 0, outlineBytes));
            for (size_t i = 0; i < outlineBytes; i += 4) {
                pixels[i] = pixels[i+2] = 100;
                pixels[i+1] = pixels[i+3] = 255;
            }
            device->unmapMemory(*memory);
            outline = std::move(buffer);
            outlineMemory = std::move(memory);
        }
        const vk::Extent3D size(data.width, data.height, 1);
        vk::UniqueDeviceMemory memory;
        auto image = device->createImageUnique({{}, vk::ImageType::e2D, format, size, 1, 1, vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal, transfer | vk::ImageUsageFlagBits::eTransferSrc});
        memory = allocate(device->getImageMemoryRequirements(*image), vk::MemoryPropertyFlagBits::eDeviceLocal);
        device->bindImageMemory(*image, *memory, 0);
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *image, range);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, transferStage, {}, {}, {}, barrier);
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        command.copyBufferToImage(*staging, *image, vk::ImageLayout::eTransferDstOptimal,
                                  vk::BufferImageCopy(0, 0, 0, layer, {}, size));
        barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
               .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
        command.end();
        queue.submit(vk::SubmitInfo({}, {}, command));
        queue.waitIdle();
        map = std::move(image);
        mapMemory = std::move(memory);
        patches.clear();
        patchPixels.clear();
        selected.clear();
        transformReady = false;
        mapSize = vk::Extent2D(data.width, data.height);
        centerX = data.width / 2.0;
        centerY = data.height / 2.0;
        scale = 1.0;
    }

    void queuePatch(int x, int y, const assets::MapImage& patch) {
        if (!map) throw std::invalid_argument("Map region update without a map");
        if (!patch.width || !patch.height || std::uint64_t(patch.width) * patch.height * 4 != patch.rgba.size())
            throw std::invalid_argument("Unsupported map patch dimensions or pixel count");
        // Clip in 64 bits: the origin and size together can exceed int.
        const std::int64_t left = std::max<std::int64_t>(x, 0), top = std::max<std::int64_t>(y, 0);
        const std::int64_t right = std::min<std::int64_t>(std::int64_t(x) + patch.width, mapSize.width);
        const std::int64_t bottom = std::min<std::int64_t>(std::int64_t(y) + patch.height, mapSize.height);
        if (right <= left || bottom <= top) return;
        const auto width = std::size_t(right - left), height = std::size_t(bottom - top);
        const auto offset = patchPixels.size();
        patchPixels.resize(offset + width * height * 4);
        for (std::size_t row = 0; row < height; ++row) {
            const auto source = (std::size_t(top - y) + row) * patch.width + std::size_t(left - x);
            std::memcpy(&patchPixels[offset + row * width * 4], &patch.rgba[source * 4], width * 4);
        }
        patches.push_back({offset, int(left), int(top), unsigned(width), unsigned(height)});
        // Without presentation (a minimized window) the queue would grow without bound.
        const auto budget = std::max<std::size_t>(std::size_t(mapSize.width) * mapSize.height * 4, 1u << 20);
        if (patchPixels.size() > budget) flushPatches();
    }

    // Copies queued pixels to the staging buffer. Requires that no submitted work is pending.
    void stagePatches() {
        if (patches.empty()) return;
        if (patchPixels.size() > patchCapacity) {
            const auto size = std::max<vk::DeviceSize>(patchPixels.size(), patchCapacity * 2);
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, size, vk::BufferUsageFlagBits::eTransferSrc});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            patchBuffer = std::move(buffer);
            patchMemory = std::move(memory);
            patchCapacity = size;
        }
        auto* pixels = device->mapMemory(*patchMemory, 0, patchPixels.size());
        std::memcpy(pixels, patchPixels.data(), patchPixels.size());
        device->unmapMemory(*patchMemory);
    }

    // Records the staged patches into the open command buffer. The map is left readable again.
    void recordPatches() {
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *map, range);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        const vk::MemoryBarrier ordered(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        bool first = true;
        for (const auto& p : patches) {
            // Later patches may overlap earlier ones and must land on top of them.
            if (!std::exchange(first, false)) command.pipelineBarrier(transferStage, transferStage, {}, ordered, {}, {});
            command.copyBufferToImage(*patchBuffer, *map, vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy(p.offset, 0, 0, layer, {p.x, p.y, 0}, {p.width, p.height, 1}));
        }
        barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
               .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
    }

    // Applies the queue now instead of with the next frame.
    void flushPatches() {
        if (patches.empty()) return;
        device->waitIdle();
        stagePatches();
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        recordPatches();
        command.end();
        queue.submit(vk::SubmitInfo({}, {}, command));
        queue.waitIdle();
        patches.clear();
        patchPixels.clear();
    }

    assets::MapImage captureMap() {
        if (!map) throw std::invalid_argument("Map capture without a map");
        flushPatches();
        device->waitIdle();
        assets::MapImage result{mapSize.width, mapSize.height, {}};
        result.rgba.resize(std::size_t(mapSize.width) * mapSize.height * 4);
        vk::UniqueDeviceMemory memory;
        auto buffer = device->createBufferUnique({{}, result.rgba.size(), vk::BufferUsageFlagBits::eTransferDst});
        memory = allocate(device->getBufferMemoryRequirements(*buffer),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device->bindBufferMemory(*buffer, *memory, 0);
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        command.copyImageToBuffer(*map, vk::ImageLayout::eTransferSrcOptimal, *buffer,
            vk::BufferImageCopy(0, 0, 0, layer, {}, {mapSize.width, mapSize.height, 1}));
        command.end();
        queue.submit(vk::SubmitInfo({}, {}, command));
        queue.waitIdle();
        const auto* pixels = device->mapMemory(*memory, 0, result.rgba.size());
        std::memcpy(result.rgba.data(), pixels, result.rgba.size());
        device->unmapMemory(*memory);
        return result;
    }

    double fitScale() const {
        return std::min(double(activeExtent.width) / mapSize.width, double(activeExtent.height) / mapSize.height);
    }

    void cameraEvent(const SDL_Event& event) {
        if (!map) return;
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            double delta = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            scale = std::clamp(scale * std::pow(1.25, delta), std::min(1.0, fitScale()), 8.0);
        } else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & (SDL_BUTTON_RMASK | SDL_BUTTON_MMASK))) {
            const auto [sx, sy] = windowScale();
            centerX -= event.motion.xrel * sx / scale;
            centerY -= event.motion.yrel * sy / scale;
        } else if (event.type == SDL_EVENT_KEY_DOWN) {
            switch (event.key.key) {
            case SDLK_LEFT: centerX -= 64 / scale; break;
            case SDLK_RIGHT: centerX += 64 / scale; break;
            case SDLK_UP: centerY -= 64 / scale; break;
            case SDLK_DOWN: centerY += 64 / scale; break;
            case SDLK_HOME: scale = fitScale(); break;
            case SDLK_1: scale = 1; break;
            }
        }
    }

    void drawMap(vk::Image destination) {
        const int w = mapSize.width, h = mapSize.height;
        const int screenW = activeExtent.width, screenH = activeExtent.height;
        const int srcW = std::clamp(int(std::round(screenW / scale)), 1, w);
        const int srcH = std::clamp(int(std::round(screenH / scale)), 1, h);
        centerX = std::clamp(centerX, srcW / 2.0, w - srcW / 2.0);
        centerY = std::clamp(centerY, srcH / 2.0, h - srcH / 2.0);
        const int x = int(centerX - srcW / 2.0), y = int(centerY - srcH / 2.0);
        const int dstW = std::clamp(int(std::round(srcW * scale)), 1, screenW);
        const int dstH = std::clamp(int(std::round(srcH * scale)), 1, screenH);
        const int left = (screenW - dstW) / 2, top = (screenH - dstH) / 2;
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        transformReady = true;
        scaleX = double(dstW) / srcW;
        scaleY = double(dstH) / srcH;
        screenLeft = left - x * scaleX;
        screenTop = top - y * scaleY;
        const vk::ImageBlit region(layer, {vk::Offset3D(x, y, 0), vk::Offset3D(x + srcW, y + srcH, 1)},
            layer, {vk::Offset3D(left, top, 0), vk::Offset3D(left + dstW, top + dstH, 1)});
        command.blitImage(*map, vk::ImageLayout::eTransferSrcOptimal, destination,
                          vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        for (const auto& rect : selected) drawOutline(destination, rect, screenW, screenH, layer);
    }

    void drawOutline(vk::Image destination, MapRect r, int screenW, int screenH, vk::ImageSubresourceLayers layer) {
        const vk::MemoryBarrier finished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        command.pipelineBarrier(transferStage, transferStage, {}, finished, {}, {});
        const int x0 = int(screenLeft + r.x * scaleX) - 2, y0 = int(screenTop + r.y * scaleY) - 2;
        const int x1 = int(screenLeft + (r.x + r.width) * scaleX) + 2;
        const int y1 = int(screenTop + (r.y + r.height) * scaleY) + 2;
        const auto line = [&](int l, int t, int right, int bottom) {
            l = std::max(0, l); t = std::max(0, t);
            right = std::min(screenW, right); bottom = std::min(screenH, bottom);
            if (right <= l || bottom <= t || std::int64_t(right-l) * (bottom-t) > 8192) return;
            command.copyBufferToImage(*outline, destination, vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy(0, 0, 0, layer, {l, t, 0}, {unsigned(right-l), unsigned(bottom-t), 1}));
        };
        line(x0, y0, x1, y0+1); line(x0, y1-1, x1, y1);
        line(x0, y0+1, x0+1, y1-1); line(x1-1, y0+1, x1, y1-1);
    }

    // Called with no submitted work pending, so the buffer can be replaced.
    void uploadOverlay() {
        overlayStale = false;
        if (!overlayW) return;
        const bool bgr = surfaceFormat == vk::Format::eB8G8R8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!bgr && surfaceFormat != vk::Format::eR8G8B8A8Srgb && surfaceFormat != vk::Format::eR8G8B8A8Unorm)
            throw std::runtime_error("Overlay requires an RGBA8 or BGRA8 surface");
        if (overlayPixels.size() > overlayCapacity) {
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, overlayPixels.size(), vk::BufferUsageFlagBits::eTransferSrc});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            overlayBuffer = std::move(buffer);
            overlayMemory = std::move(memory);
            overlayCapacity = overlayPixels.size();
        }
        auto* pixels = static_cast<std::uint8_t*>(device->mapMemory(*overlayMemory, 0, overlayPixels.size()));
        std::memcpy(pixels, overlayPixels.data(), overlayPixels.size());
        if (bgr) for (size_t i = 0; i < overlayPixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        device->unmapMemory(*overlayMemory);
    }

    void drawOverlay(vk::Image destination) {
        const int screenW = activeExtent.width, screenH = activeExtent.height;
        const int w = overlayW, h = overlayH;
        const int left = (screenW - w) / 2, top = (screenH - h) / 2;
        // A panel larger than the window is cropped equally on both sides.
        const int srcX = std::max(0, -left), srcY = std::max(0, -top);
        const int copyW = std::min(w - srcX, screenW), copyH = std::min(h - srcY, screenH);
        if (copyW <= 0 || copyH <= 0) return;
        const vk::MemoryBarrier finished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        command.pipelineBarrier(transferStage, transferStage, {}, finished, {}, {});
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        command.copyBufferToImage(*overlayBuffer, destination, vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy((vk::DeviceSize(srcY) * w + srcX) * 4, w, 0, layer,
                {std::max(0, left), std::max(0, top), 0}, {unsigned(copyW), unsigned(copyH), 1}));
    }

    void record(vk::Image image) {
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
        if (map && !patches.empty()) recordPatches();
        const vk::ClearColorValue color(std::array<float, 4>{0.055f, 0.12f, 0.16f, 1.f});
        command.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, range);
        if (map) {
            const vk::MemoryBarrier clearFinished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
            command.pipelineBarrier(transferStage, transferStage, {}, clearFinished, {}, {});
            drawMap(image);
        }
        if (overlayW) drawOverlay(image);
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
            if (overlayStale) uploadOverlay();
            stagePatches();
            auto [status, index] = device->acquireNextImageKHR(*chain, acquireTimeoutNs, *acquired);
            if (status == vk::Result::eTimeout || status == vk::Result::eNotReady) return false;
            record(images.at(index));
            vk::PipelineStageFlags waitStage = transferStage;
            vk::SubmitInfo submit(1, &*acquired, &waitStage, 1, &command, 1, &*rendered[index]);
            device->resetFences(*submitted);
            queue.submit(submit, *submitted);
            patches.clear();
            patchPixels.clear();
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
void VulkanWindow::setMap(const assets::MapImage& map) { impl_->setMap(map); }
void VulkanWindow::updateMapRegion(int x, int y, const assets::MapImage& patch) { impl_->queuePatch(x, y, patch); }
assets::MapImage VulkanWindow::captureMap() { return impl_->captureMap(); }
void VulkanWindow::focusMap(int x, int y) {
    impl_->centerX = x;
    impl_->centerY = y;
}
std::pair<double, double> VulkanWindow::mapPosition(float x, float y) const {
    if (!impl_->transformReady) return {-1, -1};
    const auto [sx, sy] = impl_->windowScale();
    return {(x * sx - impl_->screenLeft) / impl_->scaleX, (y * sy - impl_->screenTop) / impl_->scaleY};
}
void VulkanWindow::highlight(std::vector<MapRect> rects) {
    // Clip drag rectangles at the map boundary before converting them to drawable pixels.
    for (auto& r : rects) {
        const int right = int(std::clamp<std::int64_t>(std::int64_t(r.x) + r.width, 0, impl_->mapSize.width));
        const int bottom = int(std::clamp<std::int64_t>(std::int64_t(r.y) + r.height, 0, impl_->mapSize.height));
        r.x = std::clamp(r.x, 0, int(impl_->mapSize.width));
        r.y = std::clamp(r.y, 0, int(impl_->mapSize.height));
        r.width = right - r.x;
        r.height = bottom - r.y;
    }
    std::erase_if(rects, [](MapRect r) { return r.width <= 0 || r.height <= 0; });
    impl_->selected = std::move(rects);
}
void VulkanWindow::setOverlay(unsigned width, unsigned height, std::span<const std::uint8_t> rgba) {
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width) * height * 4 != rgba.size())
        throw std::invalid_argument("Unsupported overlay dimensions or pixel count");
    impl_->overlayPixels.assign(rgba.begin(), rgba.end());
    impl_->overlayW = width;
    impl_->overlayH = height;
    impl_->overlayStale = true;
}
void VulkanWindow::clearOverlay() {
    impl_->overlayW = impl_->overlayH = 0;
    impl_->overlayStale = false;
}
void VulkanWindow::handleEvent(const SDL_Event& event) {
    impl_->cameraEvent(event);
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event.window.windowID == SDL_GetWindowID(impl_->window)) impl_->dirty = true;
}
std::pair<unsigned, unsigned> VulkanWindow::extent() const {
    return {impl_->activeExtent.width, impl_->activeExtent.height};
}
bool VulkanWindow::present(const Snapshot&) { return impl_->present(); }
}
