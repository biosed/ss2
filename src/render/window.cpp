#include "render/window.hpp"
#include "render/sprite_shaders.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace ss2 {
namespace {
bool gPreferSoftwareRenderer = false;
constexpr auto transfer = vk::ImageUsageFlagBits::eTransferDst;
constexpr auto attachment = vk::ImageUsageFlagBits::eColorAttachment;
struct SpriteVertex { float x, y, u, v, brightness, alpha; };
struct MapVertex { float x, y, u, v; };
struct MapPush {
    float xform[4];
    float texView[4];
    float params[4];
    float grade[4];
    float extra[4];
    float worldAnim[4];
    float weather[4];
};
constexpr auto transferStage = vk::PipelineStageFlagBits::eTransfer;
constexpr std::uint64_t acquireTimeoutNs = 50'000'000;
constexpr char validationLayer[] = "VK_LAYER_KHRONOS_validation";
constexpr std::size_t outlineBand = 65536 * 4;

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
    // Window size in pixels that the current swapchain was built for.
    int builtWidth = 0, builtHeight = 0;
    bool dirty = true;
    struct MapTile {
        int x{}, y{};
        unsigned width{}, height{};
        vk::UniqueDeviceMemory memory;
        vk::UniqueImage image;
        vk::UniqueImageView view;
        vk::DescriptorSet set{};
    };
    std::vector<MapTile> mapTiles;
    bool hasMap() const { return !mapTiles.empty(); }
    vk::Extent2D mapSize;
    vk::UniqueDeviceMemory outlineMemory;
    vk::UniqueBuffer outline;
    std::vector<MapRect> selected;
    std::vector<MapMarker> markers;
    std::vector<MapBar> bars;
    struct MapPatch { vk::DeviceSize offset; int x, y; unsigned width, height; };
    std::vector<MapPatch> patches;
    std::vector<std::uint8_t> patchPixels;
    vk::UniqueDeviceMemory patchMemory;
    vk::UniqueBuffer patchBuffer;
    vk::DeviceSize patchCapacity{};
    struct Overlay {
        vk::UniqueDeviceMemory memory;
        vk::UniqueBuffer buffer;
        vk::DeviceSize capacity{};
        std::vector<std::uint8_t> pixels;
        unsigned width{}, height{};
        int left{}, top{};   // used by slots without a fixed place
        bool stale = false;
        bool hasAlpha = false;
        bool stretchFullscreen = false;
    };
    // Indexed by OverlaySlot and drawn back to front: the bar, the notice, the panel, the menu.
    std::array<Overlay, 5> overlays;
    // Transform of the last recorded frame, including any letterboxing.
    double screenLeft{}, screenTop{}, scaleX = 1.0, scaleY = 1.0;
    double centerX{}, centerY{}, scale = 1.0;
    double targetCenterX{}, targetCenterY{}, targetScale = 1.0;
    bool animatingCamera = false;
    std::uint64_t lastCameraTickNs = 0;
    vk::Format surfaceFormat{};
    bool transformReady = false;
    bool preferSoftware = gPreferSoftwareRenderer;
    bool mapLinear = false;
    MapGrade mapGrade{};
    int weatherKind = 0;
    float weatherPhase = 0.0f;
    float weatherFlash = 0.0f;
    unsigned hudBarHeight = 0;
    bool canSampleMap = false;
    vk::Format mapFormat{};
    vk::UniqueDescriptorPool mapPool;
    vk::UniquePipelineLayout mapPipelineLayout;
    vk::UniquePipeline mapPipeline;
    vk::Format mapPipelineFormat{};
    vk::UniqueDeviceMemory unitQuadMemory;
    vk::UniqueBuffer unitQuadBuffer;
    // Sprite pass. Views follow the swapchain; the pipeline follows the surface format.
    std::vector<vk::UniqueImageView> views;
    bool canCapture = false;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout spriteLayout;
    vk::UniquePipelineLayout spritePipelineLayout;
    vk::UniqueDescriptorPool spritePool;
    vk::UniquePipeline spritePipeline;
    vk::Format spritePipelineFormat{};
    struct Atlas {
        vk::UniqueDeviceMemory memory;
        vk::UniqueImage image;
        vk::UniqueImageView view;
        vk::DescriptorSet set;
        unsigned width{}, height{};
        vk::Format format{};
        std::vector<std::uint8_t> pixels;   // kept so the atlas can follow a surface format change
        vk::UniqueDeviceMemory stagingMemory;
        vk::UniqueBuffer stagingBuffer;
        vk::DeviceSize stagingCapacity{};
        bool pendingUpload = false;
    };
    // The caller's slots, beam glows, alpha-blended HUD overlays, and fullscreen UI panels.
    static constexpr unsigned beamAtlas = VulkanWindow::spriteAtlasSlots;
    static constexpr unsigned hudAtlas = VulkanWindow::spriteAtlasSlots + 1;
    static constexpr unsigned panelAtlas = VulkanWindow::spriteAtlasSlots + 2;
    static constexpr unsigned beamBlock = 16;   // texels per side of one colour's glow
    std::array<Atlas, VulkanWindow::spriteAtlasSlots + 3> atlases;
    vk::UniqueDeviceMemory hudQuadMemory;
    vk::UniqueBuffer hudQuadBuffer;
    vk::UniqueDeviceMemory panelQuadMemory;
    vk::UniqueBuffer panelQuadBuffer;
    std::vector<Sprite> sprites;
    std::vector<MapBeam> beams;
    std::vector<std::array<std::uint8_t, 3>> beamPalette;
    struct SpriteBatch { unsigned atlas; std::uint32_t first, count; };
    std::vector<SpriteBatch> spriteBatches;
    vk::UniqueDeviceMemory spriteMemory;
    vk::UniqueBuffer spriteBuffer;
    vk::DeviceSize spriteCapacity{};
    bool spritesStale = false;
    float spriteSharpness = 0.0f;
    // Where the map landed on screen in the last recorded frame.
    vk::Rect2D mapScreenRect;

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
        vk::PhysicalDeviceVulkan13Features features13;
        features13.dynamicRendering = true;
        vk::DeviceCreateInfo deviceInfo({}, 1, &queueInfo, 0, nullptr, 1, &swapchainExtension);
        deviceInfo.pNext = &features13;
        device = gpu.createDeviceUnique(deviceInfo);
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
            const auto families = candidate.getQueueFamilyProperties();
            for (uint32_t i = 0; i < families.size(); ++i) {
                if (!(families[i].queueFlags & vk::QueueFlagBits::eGraphics) ||
                    // On Wayland a device may support the surface type but lack a
                    // buffer format the current compositor can import.
                    !SDL_Vulkan_GetPresentationSupport(*instance, candidate, i) ||
                    !candidate.getSurfaceSupportKHR(i, *surface)) continue;
                // These queries require a device that can present to this specific surface.
                const auto usage = candidate.getSurfaceCapabilitiesKHR(*surface).supportedUsageFlags;
                if (!(usage & transfer) || !(usage & attachment) ||
                    candidate.getSurfaceFormatsKHR(*surface).empty()) break;
                if (!candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features>()
                         .get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering) break;
                int score = 0;
                if (preferSoftware) {
                    score = props.deviceType == vk::PhysicalDeviceType::eCpu ? 3 :
                            props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 2 :
                            props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu ? 1 : 0;
                } else {
                    score = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 3 :
                            props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu ? 2 :
                            props.deviceType == vk::PhysicalDeviceType::eVirtualGpu ? 1 : 0;
                }
                if (score > best) {
                    best = score;
                    gpu = candidate;
                    family = i;
                }
                break;
            }
        }
        if (!gpu) throw std::runtime_error("Need Vulkan 1.3 with dynamic rendering, a graphics/present queue and a swapchain usable as transfer destination and colour attachment");
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
        auto usage = transfer | attachment;
        canCapture = bool(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc);
        if (canCapture) usage |= vk::ImageUsageFlagBits::eTransferSrc;
        vk::SwapchainCreateInfoKHR create({}, *surface, count, format.format, format.colorSpace,
            extent, 1, usage, vk::SharingMode::eExclusive, 0, nullptr, caps.currentTransform,
            alpha, vk::PresentModeKHR::eFifo, true, chain.get());
        views.clear();
        chain = device->createSwapchainKHRUnique(create);
        images = device->getSwapchainImagesKHR(*chain);
        for (auto image : images)
            views.push_back(device->createImageViewUnique({{}, image, vk::ImageViewType::e2D, format.format, {},
                {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}}));
        rendered.clear();
        for (size_t i = 0; i < images.size(); ++i) rendered.push_back(device->createSemaphoreUnique({}));
        if (hasMap() && !(gpu.getFormatProperties(format.format).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst))
            throw std::runtime_error("Surface format cannot display the map");
        if (hasMap() && surfaceFormat != format.format)
            throw std::runtime_error("Map surface format changed during resize");
        const bool formatChanged = surfaceFormat != format.format;
        surfaceFormat = format.format;
        activeExtent = extent;
        // Atlases are stored in the surface's colour encoding, so they follow it.
        if (formatChanged)
            for (unsigned slot = 0; slot < atlases.size(); ++slot)
                if (atlases[slot].image) uploadAtlas(slot);
        builtWidth = width;
        builtHeight = height;
        for (auto& overlay : overlays) overlay.stale = true; // Stored in the surface's byte order.
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

    void setMap(const assets::MapImage& data, unsigned maxTileSize = 0) {
        const auto limit = gpu.getProperties().limits.maxImageDimension2D;
        if (!data.width || !data.height || uint64_t(data.width) * data.height * 4 != data.rgba.size())
            throw std::runtime_error("Unsupported map dimensions or pixel count");
        if (dirty && !rebuild()) throw std::runtime_error("Map needs a drawable window");
        const bool srgb = surfaceFormat == vk::Format::eR8G8B8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Srgb;
        const bool unorm = surfaceFormat == vk::Format::eR8G8B8A8Unorm || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!srgb && !unorm) throw std::runtime_error("Map requires an RGBA8 or BGRA8 surface");
        const auto format = unorm ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
        const auto fmtFeatures = gpu.getFormatProperties(format).optimalTilingFeatures;
        if (!(fmtFeatures & vk::FormatFeatureFlagBits::eBlitSrc) ||
            !(gpu.getFormatProperties(surfaceFormat).optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst))
            throw std::runtime_error("GPU cannot blit map pixels to the window");
        const bool sampleMap = bool(fmtFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
        device->waitIdle();
        if (sampleMap) ensureMapPipeline();
        mapPool.reset();
        mapTiles.clear();
        if (!outline) {
            // One band of solid colour per MarkerKind plus a dark one for gauge backgrounds,
            // written in the surface's byte order.
            constexpr size_t outlineBytes = outlineBand * 4;
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, outlineBytes, vk::BufferUsageFlagBits::eTransferSrc});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            auto* pixels = static_cast<std::uint8_t*>(device->mapMemory(*memory, 0, outlineBytes));
            const bool bgr = surfaceFormat == vk::Format::eB8G8R8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
            constexpr std::uint8_t colours[4][3] = {{100, 255, 100}, {232, 72, 60}, {240, 210, 90}, {24, 26, 24}};
            for (size_t i = 0; i < outlineBytes; i += 4) {
                const auto& c = colours[i / outlineBand];
                pixels[i] = c[bgr ? 2 : 0];
                pixels[i+1] = c[1];
                pixels[i+2] = c[bgr ? 0 : 2];
                pixels[i+3] = 255;
            }
            device->unmapMemory(*memory);
            outline = std::move(buffer);
            outlineMemory = std::move(memory);
        }

        const unsigned tileMax = std::clamp(maxTileSize ? maxTileSize : 4096u, 16u, limit);
        const std::size_t maxStagingW = std::min(data.width, tileMax);
        const std::size_t maxStagingH = std::min(data.height, tileMax);
        const vk::DeviceSize stagingBytes = vk::DeviceSize(maxStagingW) * maxStagingH * 4;
        auto staging = device->createBufferUnique({{}, stagingBytes, vk::BufferUsageFlagBits::eTransferSrc});
        auto stagingMemory = allocate(device->getBufferMemoryRequirements(*staging),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device->bindBufferMemory(*staging, *stagingMemory, 0);

        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        std::vector<MapTile> newTiles;
        const unsigned cols = (data.width + tileMax - 1) / tileMax;
        const unsigned rows = (data.height + tileMax - 1) / tileMax;
        newTiles.reserve(std::size_t(cols) * rows);

        for (unsigned ty = 0; ty < rows; ++ty) {
            const unsigned tileY = ty * tileMax;
            const unsigned th = std::min(tileMax, data.height - tileY);
            for (unsigned tx = 0; tx < cols; ++tx) {
                const unsigned tileX = tx * tileMax;
                const unsigned tw = std::min(tileMax, data.width - tileX);
                const std::size_t tileBytes = std::size_t(tw) * th * 4;
                auto* dst = static_cast<std::uint8_t*>(device->mapMemory(*stagingMemory, 0, tileBytes));
                for (unsigned row = 0; row < th; ++row) {
                    const auto* srcRow = data.rgba.data() + (std::size_t(tileY + row) * data.width + tileX) * 4;
                    std::memcpy(dst + std::size_t(row) * tw * 4, srcRow, std::size_t(tw) * 4);
                }
                device->unmapMemory(*stagingMemory);

                const vk::Extent3D size(tw, th, 1);
                const auto tileUsage = transfer | vk::ImageUsageFlagBits::eTransferSrc |
                    (sampleMap ? vk::ImageUsageFlagBits::eSampled : vk::ImageUsageFlags{});
                auto image = device->createImageUnique({{}, vk::ImageType::e2D, format, size, 1, 1, vk::SampleCountFlagBits::e1,
                    vk::ImageTiling::eOptimal, tileUsage});
                auto memory = allocate(device->getImageMemoryRequirements(*image), vk::MemoryPropertyFlagBits::eDeviceLocal);
                device->bindImageMemory(*image, *memory, 0);
                command.reset();
                command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
                vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *image, range);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, transferStage, {}, {}, {}, barrier);
                command.copyBufferToImage(*staging, *image, vk::ImageLayout::eTransferDstOptimal,
                                          vk::BufferImageCopy(0, 0, 0, layer, {}, size));
                barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                       .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
                command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
                command.end();
                queue.submit(vk::SubmitInfo({}, {}, command));
                queue.waitIdle();
                vk::UniqueImageView view;
                if (sampleMap) {
                    view = device->createImageViewUnique({{}, *image, vk::ImageViewType::e2D, format, {}, range});
                }
                newTiles.push_back({int(tileX), int(tileY), tw, th, std::move(memory), std::move(image), std::move(view), {}});
            }
        }
        if (sampleMap && !newTiles.empty()) {
            const vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, std::uint32_t(newTiles.size()));
            mapPool = device->createDescriptorPoolUnique({{}, std::uint32_t(newTiles.size()), 1, &poolSize});
            std::vector<vk::DescriptorSetLayout> layouts(newTiles.size(), *spriteLayout);
            const auto sets = device->allocateDescriptorSets({*mapPool, std::uint32_t(layouts.size()), layouts.data()});
            std::vector<vk::DescriptorImageInfo> imgInfos(newTiles.size());
            std::vector<vk::WriteDescriptorSet> writes(newTiles.size());
            for (std::size_t i = 0; i < newTiles.size(); ++i) {
                newTiles[i].set = sets[i];
                imgInfos[i] = vk::DescriptorImageInfo(*sampler, *newTiles[i].view, vk::ImageLayout::eShaderReadOnlyOptimal);
                writes[i] = vk::WriteDescriptorSet(sets[i], 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imgInfos[i]);
            }
            device->updateDescriptorSets(writes, {});
        }
        canSampleMap = sampleMap;
        mapTiles = std::move(newTiles);
        mapFormat = format;
        patches.clear();
        patchPixels.clear();
        selected.clear();
        markers.clear();
        bars.clear();
        sprites.clear();
        beams.clear();
        spriteBatches.clear();
        spritesStale = false;
        transformReady = false;
        mapSize = vk::Extent2D(data.width, data.height);
        centerX = targetCenterX = data.width / 2.0;
        centerY = targetCenterY = data.height / 2.0;
        scale = targetScale = 1.0;
        animatingCamera = false;
        lastCameraTickNs = 0;
    }

    void queuePatch(int x, int y, const assets::MapImage& patch) {
        if (!hasMap()) throw std::invalid_argument("Map region update without a map");
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
        const auto budget = std::clamp<std::size_t>(std::size_t(mapSize.width) * mapSize.height * 4, 1u << 20, 64u << 20);
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
        std::vector<bool> touched(mapTiles.size(), false);
        for (const auto& p : patches) {
            const int px1 = p.x + int(p.width);
            const int py1 = p.y + int(p.height);
            for (std::size_t ti = 0; ti < mapTiles.size(); ++ti) {
                const auto& t = mapTiles[ti];
                if (px1 > t.x && p.x < t.x + int(t.width) && py1 > t.y && p.y < t.y + int(t.height))
                    touched[ti] = true;
            }
        }
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        std::vector<vk::ImageMemoryBarrier> toDst;
        std::vector<vk::ImageMemoryBarrier> toSrc;
        for (std::size_t ti = 0; ti < mapTiles.size(); ++ti) {
            if (!touched[ti]) continue;
            toDst.emplace_back(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eTransferDstOptimal,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *mapTiles[ti].image, range);
            toSrc.emplace_back(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *mapTiles[ti].image, range);
        }
        if (toDst.empty()) return;
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, toDst);
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        const vk::MemoryBarrier ordered(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        bool first = true;
        for (const auto& p : patches) {
            // Later patches may overlap earlier ones and must land on top of them.
            if (!std::exchange(first, false)) command.pipelineBarrier(transferStage, transferStage, {}, ordered, {}, {});
            const int px1 = p.x + int(p.width);
            const int py1 = p.y + int(p.height);
            for (std::size_t ti = 0; ti < mapTiles.size(); ++ti) {
                if (!touched[ti]) continue;
                const auto& tile = mapTiles[ti];
                const int ix0 = std::max(p.x, tile.x);
                const int iy0 = std::max(p.y, tile.y);
                const int ix1 = std::min(px1, tile.x + int(tile.width));
                const int iy1 = std::min(py1, tile.y + int(tile.height));
                if (ix1 <= ix0 || iy1 <= iy0) continue;
                const vk::DeviceSize bufOff = p.offset +
                    (vk::DeviceSize(iy0 - p.y) * p.width + vk::DeviceSize(ix0 - p.x)) * 4;
                command.copyBufferToImage(*patchBuffer, *tile.image, vk::ImageLayout::eTransferDstOptimal,
                    vk::BufferImageCopy(bufOff, p.width, 0, layer, {ix0 - tile.x, iy0 - tile.y, 0},
                                        {unsigned(ix1 - ix0), unsigned(iy1 - iy0), 1}));
            }
        }
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, toSrc);
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
        if (!hasMap()) throw std::invalid_argument("Map capture without a map");
        flushPatches();
        device->waitIdle();
        assets::MapImage result{mapSize.width, mapSize.height, {}};
        result.rgba.resize(std::size_t(mapSize.width) * mapSize.height * 4);
        vk::DeviceSize maxTileBytes = 0;
        for (const auto& t : mapTiles) {
            maxTileBytes = std::max(maxTileBytes, vk::DeviceSize(t.width) * t.height * 4);
        }
        vk::UniqueDeviceMemory memory;
        auto buffer = device->createBufferUnique({{}, maxTileBytes, vk::BufferUsageFlagBits::eTransferDst});
        memory = allocate(device->getBufferMemoryRequirements(*buffer),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device->bindBufferMemory(*buffer, *memory, 0);
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        for (const auto& tile : mapTiles) {
            command.reset();
            command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            command.copyImageToBuffer(*tile.image, vk::ImageLayout::eTransferSrcOptimal, *buffer,
                vk::BufferImageCopy(0, 0, 0, layer, {}, {tile.width, tile.height, 1}));
            command.end();
            queue.submit(vk::SubmitInfo({}, {}, command));
            queue.waitIdle();
            const std::size_t tileBytes = std::size_t(tile.width) * tile.height * 4;
            const auto* pixels = static_cast<const std::uint8_t*>(device->mapMemory(*memory, 0, tileBytes));
            for (unsigned row = 0; row < tile.height; ++row) {
                auto* dstRow = result.rgba.data() +
                    (std::size_t(tile.y + int(row)) * mapSize.width + std::size_t(tile.x)) * 4;
                std::memcpy(dstRow, pixels + std::size_t(row) * tile.width * 4, std::size_t(tile.width) * 4);
            }
            device->unmapMemory(*memory);
        }
        return result;
    }

    // The map occupies the full window beneath a transparent HUD, or the area above an opaque bar.
    int viewHeight() const {
        const auto& hud = overlays[std::size_t(OverlaySlot::hud)];
        const int bar = hud.hasAlpha ? 0 : int(hud.height);
        return std::max(1, int(activeExtent.height) - bar);
    }

    double fitScale() const {
        return std::min(double(activeExtent.width) / mapSize.width, double(viewHeight()) / mapSize.height);
    }

    void clampCenter(double& cx, double& cy, double s) const {
        if (!hasMap() || activeExtent.width == 0 || activeExtent.height == 0 || s <= 0.0) return;
        const int w = int(mapSize.width), h = int(mapSize.height);
        const int screenW = int(activeExtent.width), screenH = viewHeight();
        const int srcW = std::clamp(int(std::round(screenW / s)), 1, w);
        const int srcH = std::clamp(int(std::round(screenH / s)), 1, h);
        cx = std::clamp(cx, srcW / 2.0, w - srcW / 2.0);
        cy = std::clamp(cy, srcH / 2.0, h - srcH / 2.0);
    }

    void cameraEvent(const SDL_Event& event) {
        if (!hasMap()) return;
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            double delta = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            if (delta == 0.0) return;
            const double minScale = std::min(1.0, fitScale());
            const double prevTarget = animatingCamera ? targetScale : scale;
            const double nextScale = std::clamp(prevTarget * std::pow(1.25, delta), minScale, 8.0);
            if (!animatingCamera) {
                targetCenterX = centerX;
                targetCenterY = centerY;
            }
            const bool hasCursor = event.wheel.windowID != 0 || event.wheel.mouse_x != 0.0f || event.wheel.mouse_y != 0.0f;
            if (hasCursor && activeExtent.width > 0 && activeExtent.height > 0) {
                const auto [sx, sy] = windowScale();
                const double px = double(event.wheel.mouse_x) * sx;
                const double py = double(event.wheel.mouse_y) * sy;
                const int screenW = int(activeExtent.width);
                const int screenH = viewHeight();
                if (px >= 0.0 && px <= double(screenW) && py >= 0.0 && py <= double(screenH)) {
                    const double worldX = targetCenterX + (px - screenW * 0.5) / prevTarget;
                    const double worldY = targetCenterY + (py - screenH * 0.5) / prevTarget;
                    targetCenterX = worldX - (px - screenW * 0.5) / nextScale;
                    targetCenterY = worldY - (py - screenH * 0.5) / nextScale;
                }
            }
            targetScale = nextScale;
            clampCenter(targetCenterX, targetCenterY, targetScale);
            animatingCamera = true;
        } else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_MMASK)) {
            const auto [sx, sy] = windowScale();
            const double dx = -event.motion.xrel * sx / scale;
            const double dy = -event.motion.yrel * sy / scale;
            centerX += dx;
            centerY += dy;
            targetCenterX = centerX;
            targetCenterY = centerY;
        } else if (event.type == SDL_EVENT_KEY_DOWN) {
            switch (event.key.key) {
            case SDLK_LEFT: centerX -= 64 / scale; targetCenterX = centerX; break;
            case SDLK_RIGHT: centerX += 64 / scale; targetCenterX = centerX; break;
            case SDLK_UP: centerY -= 64 / scale; targetCenterY = centerY; break;
            case SDLK_DOWN: centerY += 64 / scale; targetCenterY = centerY; break;
            case SDLK_HOME:
                scale = targetScale = fitScale();
                targetCenterX = centerX;
                targetCenterY = centerY;
                animatingCamera = false;
                break;
            case SDLK_1:
                scale = targetScale = 1;
                targetCenterX = centerX;
                targetCenterY = centerY;
                animatingCamera = false;
                break;
            }
        }
    }

    void drawMap(vk::Image destination, vk::ImageView view) {
        const int w = mapSize.width, h = mapSize.height;
        const int screenW = activeExtent.width, screenH = viewHeight();
        const int srcW = std::clamp(int(std::round(screenW / scale)), 1, w);
        const int srcH = std::clamp(int(std::round(screenH / scale)), 1, h);
        clampCenter(centerX, centerY, scale);
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
        const int srcX1 = x + srcW, srcY1 = y + srcH;
        mapScreenRect = vk::Rect2D({left, top}, {unsigned(dstW), unsigned(dstH)});

        if ((mapGrade.enabled || weatherKind > 0) && canSampleMap && mapPipeline && unitQuadBuffer) {
            std::vector<std::size_t> visibleTiles;
            visibleTiles.reserve(mapTiles.size());
            for (std::size_t ti = 0; ti < mapTiles.size(); ++ti) {
                const auto& tile = mapTiles[ti];
                if (int(tile.x + tile.width) <= x || tile.x >= srcX1 ||
                    int(tile.y + tile.height) <= y || tile.y >= srcY1) continue;
                visibleTiles.push_back(ti);
            }
            if (visibleTiles.empty()) return;

            const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
            const auto colourAccess = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
            std::vector<vk::ImageMemoryBarrier> preBarriers;
            preBarriers.reserve(visibleTiles.size() + 1);
            for (const std::size_t ti : visibleTiles) {
                preBarriers.emplace_back(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
                    vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *mapTiles[ti].image, range);
            }
            preBarriers.emplace_back(vk::AccessFlagBits::eTransferWrite, colourAccess,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range);
            command.pipelineBarrier(transferStage,
                vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
                {}, {}, {}, preBarriers);

            vk::RenderingAttachmentInfo target;
            target.imageView = view;
            target.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            target.loadOp = vk::AttachmentLoadOp::eLoad;
            target.storeOp = vk::AttachmentStoreOp::eStore;
            vk::RenderingInfo rendering;
            rendering.renderArea = vk::Rect2D({0, 0}, activeExtent);
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &target;
            command.beginRendering(rendering);
            command.bindPipeline(vk::PipelineBindPoint::eGraphics, *mapPipeline);
            command.setViewport(0, vk::Viewport(0, 0, float(activeExtent.width), float(activeExtent.height), 0, 1));
            command.setScissor(0, mapScreenRect);
            command.bindVertexBuffers(0, *unitQuadBuffer, vk::DeviceSize(0));
            const float invExtW = 2.0f / float(activeExtent.width);
            const float invExtH = 2.0f / float(activeExtent.height);
            const float viewInvW = 1.0f / float(std::max(1, dstW));
            const float viewInvH = 1.0f / float(std::max(1, dstH));
            const bool gOn = mapGrade.enabled;
            const float smoothMode = (gOn && mapLinear) ? 1.0f : 0.0f;
            const float texelsPerScreenPx = gOn ? float(1.0 / std::max(0.01, scaleX)) : 1.0f;
            const float srgbFlag = srgbSurface() ? 1.0f : 0.0f;
            const float sharpVal = gOn ? mapGrade.sharpness : 0.0f;
            const float vigVal = gOn ? mapGrade.vignette : 0.0f;
            const float rG = gOn ? mapGrade.rGain : 1.0f;
            const float gG = gOn ? mapGrade.gGain : 1.0f;
            const float bG = gOn ? mapGrade.bGain : 1.0f;
            const float cVal = gOn ? mapGrade.contrast : 1.0f;
            const float shimmerVal = gOn ? mapGrade.waterShimmer : 0.0f;
            const float uiScale = std::max(1.0f, float(activeExtent.height) / 1080.0f);
            for (const std::size_t ti : visibleTiles) {
                const auto& tile = mapTiles[ti];
                const float tileDstX = float(screenLeft + double(tile.x) * scaleX);
                const float tileDstY = float(screenTop + double(tile.y) * scaleY);
                const float tileDstW = float(double(tile.width) * scaleX);
                const float tileDstH = float(double(tile.height) * scaleY);
                const MapPush push{
                    {tileDstW * invExtW, tileDstH * invExtH, tileDstX * invExtW - 1.0f, tileDstY * invExtH - 1.0f},
                    {float(tile.width), float(tile.height), float(left), float(top)},
                    {viewInvW, viewInvH, smoothMode, sharpVal},
                    {rG, gG, bG, vigVal},
                    {cVal, texelsPerScreenPx, srgbFlag, 0.0f},
                    {float(tile.x), float(tile.y), mapGrade.timeSec, shimmerVal},
                    {float(weatherKind), weatherPhase, weatherFlash, uiScale}
                };
                command.pushConstants(*mapPipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                      0, sizeof(push), &push);
                command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *mapPipelineLayout, 0, tile.set, {});
                command.draw(6, 1, 0, 0);
            }
            command.endRendering();

            std::vector<vk::ImageMemoryBarrier> postBarriers;
            postBarriers.reserve(visibleTiles.size() + 1);
            for (const std::size_t ti : visibleTiles) {
                postBarriers.emplace_back(vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferRead,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *mapTiles[ti].image, range);
            }
            postBarriers.emplace_back(colourAccess, vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferDstOptimal,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range);
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput,
                transferStage, {}, {}, {}, postBarriers);
            return;
        }

        // Linear only where the map format supports it; otherwise stay with nearest.
        const bool linear = mapLinear && canSampleMap;
        const auto filter = linear ? vk::Filter::eLinear : vk::Filter::eNearest;
        for (const auto& tile : mapTiles) {
            const int sx0 = std::max(x, tile.x);
            const int sy0 = std::max(y, tile.y);
            const int sx1 = std::min(srcX1, tile.x + int(tile.width));
            const int sy1 = std::min(srcY1, tile.y + int(tile.height));
            if (sx1 <= sx0 || sy1 <= sy0) continue;
            const int dx0 = std::clamp(int(std::lround(screenLeft + sx0 * scaleX)), left, left + dstW);
            const int dy0 = std::clamp(int(std::lround(screenTop + sy0 * scaleY)), top, top + dstH);
            const int dx1 = std::clamp(int(std::lround(screenLeft + sx1 * scaleX)), left, left + dstW);
            const int dy1 = std::clamp(int(std::lround(screenTop + sy1 * scaleY)), top, top + dstH);
            if (dx1 <= dx0 || dy1 <= dy0) continue;
            const vk::ImageBlit region(
                layer,
                {vk::Offset3D(sx0 - tile.x, sy0 - tile.y, 0), vk::Offset3D(sx1 - tile.x, sy1 - tile.y, 1)},
                layer,
                {vk::Offset3D(dx0, dy0, 0), vk::Offset3D(dx1, dy1, 1)});
            command.blitImage(*tile.image, vk::ImageLayout::eTransferSrcOptimal, destination,
                              vk::ImageLayout::eTransferDstOptimal, region, filter);
        }
    }

    // Solid rectangles from the colour bands: a dark track, then the filled part.
    void drawBars(vk::Image destination) {
        if (bars.empty()) return;
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        const vk::MemoryBarrier finished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        const std::int64_t clipL = mapScreenRect.offset.x, clipT = mapScreenRect.offset.y;
        const std::int64_t clipR = clipL + mapScreenRect.extent.width, clipB = clipT + mapScreenRect.extent.height;
        const int thickness = std::max(3, int(activeExtent.height) / 240);
        const auto fill = [&](std::int64_t l, std::int64_t t, std::int64_t r, std::int64_t b, unsigned band) {
            l = std::max(l, clipL); t = std::max(t, clipT); r = std::min(r, clipR); b = std::min(b, clipB);
            if (r <= l || b <= t || (r - l) * (b - t) > 8192) return;
            command.pipelineBarrier(transferStage, transferStage, {}, finished, {}, {});
            command.copyBufferToImage(*outline, destination, vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy(vk::DeviceSize(band) * outlineBand, 0, 0, layer, {int(l), int(t), 0},
                                    {unsigned(r - l), unsigned(b - t), 1}));
        };
        const auto edge = [](double v) { return std::int64_t(std::llround(std::clamp(v, -1e9, 1e9))); };
        for (const auto& bar : bars) {
            const std::int64_t l = edge(screenLeft + (bar.x - bar.width / 2.0) * scaleX);
            const std::int64_t r = std::max(l + 4, edge(screenLeft + (bar.x + bar.width / 2.0) * scaleX));
            const std::int64_t t = edge(screenTop + bar.y * scaleY) - thickness;
            fill(l - 1, t - 1, r + 1, t + thickness + 1, 3);
            const std::int64_t filled = l + std::llround((r - l) * double(bar.value));
            // Green above 60%, amber above 30%, red below: the bands of selection, move and attack.
            fill(l, t, filled, t + thickness, bar.value > 0.6f ? 0u : bar.value > 0.3f ? 2u : 1u);
        }
    }

    void drawOutlines(vk::Image destination) {
        const int screenW = activeExtent.width, screenH = viewHeight();
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        for (const auto& rect : selected) drawOutline(destination, rect, screenW, screenH, layer);
        for (const auto& marker : markers)
            drawOutline(destination, marker.rect, screenW, screenH, layer, marker.kind,
                        marker.kind == MarkerKind::attack ? 2 : 1);
    }

    bool srgbSurface() const {
        return surfaceFormat == vk::Format::eR8G8B8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Srgb;
    }

    // Built on first use and rebuilt if the surface format changes.
    void ensureSpritePipeline() {
        if (spritePipeline && spritePipelineFormat == surfaceFormat) return;
        device->waitIdle();
        if (!sampler) {
            sampler = device->createSamplerUnique({{}, vk::Filter::eLinear, vk::Filter::eLinear,
                vk::SamplerMipmapMode::eLinear, vk::SamplerAddressMode::eClampToEdge,
                vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
                -0.30f, false, 1.0f, false, {}, 0.0f, 8.0f});
            const vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1,
                vk::ShaderStageFlagBits::eFragment);
            spriteLayout = device->createDescriptorSetLayoutUnique({{}, 1, &binding});
            const vk::PushConstantRange push(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                             0, 6 * sizeof(float));
            spritePipelineLayout = device->createPipelineLayoutUnique({{}, 1, &*spriteLayout, 1, &push});
            const vk::DescriptorPoolSize size(vk::DescriptorType::eCombinedImageSampler, std::uint32_t(atlases.size()));
            spritePool = device->createDescriptorPoolUnique({{}, std::uint32_t(atlases.size()), 1, &size});
            std::vector<vk::DescriptorSetLayout> layouts(atlases.size(), *spriteLayout);
            const auto sets = device->allocateDescriptorSets({*spritePool, std::uint32_t(layouts.size()), layouts.data()});
            for (std::size_t i = 0; i < atlases.size(); ++i) atlases[i].set = sets[i];
        }
        const auto vertex = device->createShaderModuleUnique({{}, sizeof(shaders::spriteVertexSpirv), shaders::spriteVertexSpirv});
        const auto fragment = device->createShaderModuleUnique({{}, sizeof(shaders::spriteFragmentSpirv), shaders::spriteFragmentSpirv});
        const std::array stages{
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"),
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *fragment, "main")};
        const vk::VertexInputBindingDescription bindingInfo(0, sizeof(SpriteVertex), vk::VertexInputRate::eVertex);
        const std::array attributes{
            vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, offsetof(SpriteVertex, x)),
            vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32Sfloat, offsetof(SpriteVertex, u)),
            vk::VertexInputAttributeDescription(2, 0, vk::Format::eR32G32Sfloat, offsetof(SpriteVertex, brightness))};
        const vk::PipelineVertexInputStateCreateInfo input({}, 1, &bindingInfo, std::uint32_t(attributes.size()), attributes.data());
        const vk::PipelineInputAssemblyStateCreateInfo assembly({}, vk::PrimitiveTopology::eTriangleList);
        const vk::PipelineViewportStateCreateInfo viewport({}, 1, nullptr, 1, nullptr);
        vk::PipelineRasterizationStateCreateInfo raster;
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1;
        const vk::PipelineMultisampleStateCreateInfo multisample({}, vk::SampleCountFlagBits::e1);
        // Premultiplied alpha over an opaque target; the target's alpha stays 1.
        vk::PipelineColorBlendAttachmentState blend;
        blend.blendEnable = true;
        blend.srcColorBlendFactor = vk::BlendFactor::eOne;
        blend.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blend.colorBlendOp = vk::BlendOp::eAdd;
        blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
        blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        blend.alphaBlendOp = vk::BlendOp::eAdd;
        blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo blending;
        blending.attachmentCount = 1;
        blending.pAttachments = &blend;
        const std::array dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamicState({}, std::uint32_t(dynamic.size()), dynamic.data());
        vk::PipelineRenderingCreateInfo rendering;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &surfaceFormat;
        vk::GraphicsPipelineCreateInfo info;
        info.pNext = &rendering;
        info.stageCount = std::uint32_t(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamicState;
        info.layout = *spritePipelineLayout;
        auto created = device->createGraphicsPipelineUnique({}, info);
        if (created.result != vk::Result::eSuccess) throw std::runtime_error("Could not create the sprite pipeline");
        spritePipeline = std::move(created.value);
        spritePipelineFormat = surfaceFormat;
    }

    void ensureMapPipeline() {
        ensureSpritePipeline();
        if (mapPipeline && mapPipelineFormat == surfaceFormat) return;
        device->waitIdle();
        if (!mapPipelineLayout) {
            const vk::PushConstantRange push(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                                             0, sizeof(MapPush));
            mapPipelineLayout = device->createPipelineLayoutUnique({{}, 1, &*spriteLayout, 1, &push});
        }
        if (!unitQuadBuffer) {
            const MapVertex corners[4] = {{0.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 1.f, 0.f},
                                          {1.f, 1.f, 1.f, 1.f}, {0.f, 1.f, 0.f, 1.f}};
            const vk::DeviceSize bytes = 6 * sizeof(MapVertex);
            unitQuadBuffer = device->createBufferUnique({{}, bytes, vk::BufferUsageFlagBits::eVertexBuffer});
            unitQuadMemory = allocate(device->getBufferMemoryRequirements(*unitQuadBuffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*unitQuadBuffer, *unitQuadMemory, 0);
            auto* out = static_cast<MapVertex*>(device->mapMemory(*unitQuadMemory, 0, bytes));
            for (int c : {0, 1, 2, 0, 2, 3}) *out++ = corners[c];
            device->unmapMemory(*unitQuadMemory);
        }
        const auto vertex = device->createShaderModuleUnique({{}, sizeof(shaders::mapVertexSpirv), shaders::mapVertexSpirv});
        const auto fragment = device->createShaderModuleUnique({{}, sizeof(shaders::mapFragmentSpirv), shaders::mapFragmentSpirv});
        const std::array stages{
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vertex, "main"),
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *fragment, "main")};
        const vk::VertexInputBindingDescription bindingInfo(0, sizeof(MapVertex), vk::VertexInputRate::eVertex);
        const std::array attributes{
            vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, offsetof(MapVertex, x)),
            vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32Sfloat, offsetof(MapVertex, u))};
        const vk::PipelineVertexInputStateCreateInfo input({}, 1, &bindingInfo, std::uint32_t(attributes.size()), attributes.data());
        const vk::PipelineInputAssemblyStateCreateInfo assembly({}, vk::PrimitiveTopology::eTriangleList);
        const vk::PipelineViewportStateCreateInfo viewport({}, 1, nullptr, 1, nullptr);
        vk::PipelineRasterizationStateCreateInfo raster;
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.lineWidth = 1;
        const vk::PipelineMultisampleStateCreateInfo multisample({}, vk::SampleCountFlagBits::e1);
        vk::PipelineColorBlendAttachmentState blend;
        blend.blendEnable = false;
        blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo blending;
        blending.attachmentCount = 1;
        blending.pAttachments = &blend;
        const std::array dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamicState({}, std::uint32_t(dynamic.size()), dynamic.data());
        vk::PipelineRenderingCreateInfo rendering;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &surfaceFormat;
        vk::GraphicsPipelineCreateInfo info;
        info.pNext = &rendering;
        info.stageCount = std::uint32_t(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamicState;
        info.layout = *mapPipelineLayout;
        auto created = device->createGraphicsPipelineUnique({}, info);
        if (created.result != vk::Result::eSuccess) throw std::runtime_error("Could not create the map pipeline");
        mapPipeline = std::move(created.value);
        mapPipelineFormat = surfaceFormat;
    }

    static void premultiplyInto(const std::vector<std::uint8_t>& src, std::uint8_t* dst, bool srgb) {
        // Callers supply straight alpha. Stored premultiplied, so linear filtering cannot
        // pull the colour of transparent texels into an edge. For an sRGB image the
        // multiply happens in linear light, using a precomputed 64 KiB exact [alpha][value] LUT.
        static const auto srgbPremul = [] {
            std::array<float, 256> toLinear{};
            for (int i = 0; i < 256; ++i) {
                const float c = i / 255.f;
                toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            std::array<std::uint8_t, 256 * 256> lut{};
            for (int a = 0; a < 256; ++a) {
                const float af = a / 255.f;
                for (int v = 0; v < 256; ++v) {
                    if (a == 255) lut[a * 256 + v] = std::uint8_t(v);
                    else if (a == 0) lut[a * 256 + v] = 0;
                    else {
                        const float linear = toLinear[v] * af;
                        const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
                        lut[a * 256 + v] = std::uint8_t(std::lround(std::clamp(encoded, 0.f, 1.f) * 255));
                    }
                }
            }
            return lut;
        }();
        const std::size_t n = src.size();
        const std::uint8_t* srcPtr = src.data();
        std::uint32_t prevSrc = 0;
        std::uint32_t prevOut = 0;
        bool havePrev = false;
        if (srgb) {
            for (std::size_t i = 0; i < n; i += 4) {
                std::uint32_t curSrc = 0;
                std::memcpy(&curSrc, srcPtr + i, 4);
                if (havePrev && curSrc == prevSrc) {
                    std::memcpy(dst + i, &prevOut, 4);
                    continue;
                }
                const unsigned alpha = srcPtr[i + 3];
                const auto* row = &srgbPremul[alpha * 256];
                const std::uint8_t outPx[4] = {
                    row[srcPtr[i + 0]],
                    row[srcPtr[i + 1]],
                    row[srcPtr[i + 2]],
                    std::uint8_t(alpha),
                };
                std::memcpy(&prevOut, outPx, 4);
                std::memcpy(dst + i, &prevOut, 4);
                prevSrc = curSrc;
                havePrev = true;
            }
        } else {
            for (std::size_t i = 0; i < n; i += 4) {
                std::uint32_t curSrc = 0;
                std::memcpy(&curSrc, srcPtr + i, 4);
                if (havePrev && curSrc == prevSrc) {
                    std::memcpy(dst + i, &prevOut, 4);
                    continue;
                }
                const unsigned alpha = srcPtr[i + 3];
                std::uint8_t outPx[4];
                if (alpha == 255) {
                    outPx[0] = srcPtr[i + 0];
                    outPx[1] = srcPtr[i + 1];
                    outPx[2] = srcPtr[i + 2];
                } else if (!alpha) {
                    outPx[0] = outPx[1] = outPx[2] = 0;
                } else {
                    outPx[0] = std::uint8_t((unsigned(srcPtr[i + 0]) * alpha + 127) / 255);
                    outPx[1] = std::uint8_t((unsigned(srcPtr[i + 1]) * alpha + 127) / 255);
                    outPx[2] = std::uint8_t((unsigned(srcPtr[i + 2]) * alpha + 127) / 255);
                }
                outPx[3] = std::uint8_t(alpha);
                std::memcpy(&prevOut, outPx, 4);
                std::memcpy(dst + i, &prevOut, 4);
                prevSrc = curSrc;
                havePrev = true;
            }
        }
    }

    // Creates or replaces the GPU image for a slot from its kept pixels.
    void uploadAtlas(unsigned slot) {
        auto& atlas = atlases[slot];
        const bool srgb = srgbSurface();
        const bool unorm = surfaceFormat == vk::Format::eR8G8B8A8Unorm || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!srgb && !unorm) throw std::runtime_error("Sprites require an RGBA8 or BGRA8 surface");
        const auto format = unorm ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
        const auto fmtFeatures = gpu.getFormatProperties(format).optimalTilingFeatures;
        if (!(fmtFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear))
            throw std::runtime_error("GPU cannot filter sprite atlases");
        ensureSpritePipeline();
        device->waitIdle();
        vk::UniqueDeviceMemory stagingMemory;
        auto staging = device->createBufferUnique({{}, atlas.pixels.size(), vk::BufferUsageFlagBits::eTransferSrc});
        stagingMemory = allocate(device->getBufferMemoryRequirements(*staging),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device->bindBufferMemory(*staging, *stagingMemory, 0);
        auto* mapped = static_cast<std::uint8_t*>(device->mapMemory(*stagingMemory, 0, atlas.pixels.size()));
        premultiplyInto(atlas.pixels, mapped, srgb);
        device->unmapMemory(*stagingMemory);
        const bool wantMips = (slot < VulkanWindow::spriteAtlasSlots) && atlas.width >= 256 && atlas.height >= 256 &&
            bool(fmtFeatures & vk::FormatFeatureFlagBits::eBlitSrc) &&
            bool(fmtFeatures & vk::FormatFeatureFlagBits::eBlitDst);
        const unsigned mipLevels = wantMips ? 3u : 1u;
        const vk::Extent3D size(atlas.width, atlas.height, 1);
        vk::UniqueDeviceMemory memory;
        auto image = device->createImageUnique({{}, vk::ImageType::e2D, format, size, mipLevels, 1, vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal,
            transfer | (wantMips ? vk::ImageUsageFlagBits::eTransferSrc : vk::ImageUsageFlags{}) | vk::ImageUsageFlagBits::eSampled});
        memory = allocate(device->getImageMemoryRequirements(*image), vk::MemoryPropertyFlagBits::eDeviceLocal);
        device->bindImageMemory(*image, *memory, 0);
        const vk::ImageSubresourceRange allMips(vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1);
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *image, allMips);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, transferStage, {}, {}, {}, barrier);
        command.copyBufferToImage(*staging, *image, vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy(0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {}, size));
        if (mipLevels > 1) {
            std::int32_t mipW = static_cast<std::int32_t>(atlas.width);
            std::int32_t mipH = static_cast<std::int32_t>(atlas.height);
            for (unsigned m = 1; m < mipLevels; ++m) {
                const vk::ImageSubresourceRange prevRange(vk::ImageAspectFlagBits::eColor, m - 1, 1, 0, 1);
                vk::ImageMemoryBarrier toSrc(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *image, prevRange);
                command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, toSrc);
                const std::int32_t nextW = std::max<std::int32_t>(1, mipW / 2);
                const std::int32_t nextH = std::max<std::int32_t>(1, mipH / 2);
                const vk::ImageBlit blit(
                    {vk::ImageAspectFlagBits::eColor, m - 1, 0, 1}, {vk::Offset3D(0, 0, 0), vk::Offset3D(mipW, mipH, 1)},
                    {vk::ImageAspectFlagBits::eColor, m, 0, 1}, {vk::Offset3D(0, 0, 0), vk::Offset3D(nextW, nextH, 1)});
                command.blitImage(*image, vk::ImageLayout::eTransferSrcOptimal,
                                  *image, vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
                toSrc.setOldLayout(vk::ImageLayout::eTransferSrcOptimal).setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                     .setSrcAccessMask(vk::AccessFlagBits::eTransferRead).setDstAccessMask(vk::AccessFlagBits::eShaderRead);
                command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, toSrc);
                mipW = nextW;
                mipH = nextH;
            }
        }
        const vk::ImageSubresourceRange lastMip(vk::ImageAspectFlagBits::eColor, mipLevels - 1, 1, 0, 1);
        barrier.setSubresourceRange(lastMip)
               .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
               .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
        command.end();
        queue.submit(vk::SubmitInfo({}, {}, command));
        queue.waitIdle();
        auto view = device->createImageViewUnique({{}, *image, vk::ImageViewType::e2D, format, {}, allMips});
        const vk::DescriptorImageInfo imageInfo(*sampler, *view, vk::ImageLayout::eShaderReadOnlyOptimal);
        device->updateDescriptorSets(vk::WriteDescriptorSet(atlas.set, 0, 0, 1,
            vk::DescriptorType::eCombinedImageSampler, &imageInfo), {});
        // Commit only now, so a failed upload leaves the previous atlas usable.
        atlas.view = std::move(view);
        atlas.image = std::move(image);
        atlas.memory = std::move(memory);
        atlas.format = format;
        atlas.pendingUpload = false;
    }

    void setSpriteAtlas(unsigned slot, const assets::MapImage& data) {
        if (slot >= VulkanWindow::spriteAtlasSlots) throw std::invalid_argument("Sprite atlas slot out of range");
        const auto limit = gpu.getProperties().limits.maxImageDimension2D;
        if (!data.width || !data.height || data.width > limit || data.height > limit ||
            std::uint64_t(data.width) * data.height * 4 != data.rgba.size())
            throw std::invalid_argument("Unsupported sprite atlas dimensions or pixel count");
        if (dirty && !rebuild()) throw std::runtime_error("Sprite atlas needs a drawable window");
        auto& atlas = atlases[slot];
        auto previous = std::move(atlas.pixels);
        const auto previousW = atlas.width, previousH = atlas.height;
        atlas.pixels = data.rgba;
        atlas.width = data.width;
        atlas.height = data.height;
        try { uploadAtlas(slot); }
        catch (...) {
            atlas.pixels = std::move(previous);
            atlas.width = previousW;
            atlas.height = previousH;
            throw;
        }
        spritesStale = true;   // Texture coordinates depend on the atlas size.
    }

    // Gives every colour in the list a row of the glow atlas, rebuilding it when a new
    // colour appears. Each row is a round glow: opaque core, soft rim, clear corners.
    void setBeams(std::vector<MapBeam> list) {
        std::vector<std::array<std::uint8_t, 3>> palette = beamPalette;
        const auto add = [&](std::vector<std::array<std::uint8_t, 3>>& to) {
            for (const auto& b : list) {
                const std::array<std::uint8_t, 3> c{b.r, b.g, b.b};
                if (std::find(to.begin(), to.end(), c) == to.end()) to.push_back(c);
            }
        };
        add(palette);
        if (palette.size() > VulkanWindow::maxBeamColours) {
            // Colours of earlier lists are no longer needed; keep only this list's.
            palette.clear();
            add(palette);
            if (palette.size() > VulkanWindow::maxBeamColours) throw std::invalid_argument("Too many beam colours in one list");
        }
        if (palette != beamPalette) {
            if (dirty && !rebuild()) throw std::runtime_error("Beams need a drawable window");
            auto& atlas = atlases[beamAtlas];
            const unsigned rows = unsigned(palette.size()) * beamBlock;
            std::vector<std::uint8_t> pixels(std::size_t(beamBlock) * rows * 4);
            const float centre = (beamBlock - 1) / 2.f;
            for (unsigned k = 0; k < palette.size(); ++k)
                for (unsigned y = 0; y < beamBlock; ++y)
                    for (unsigned x = 0; x < beamBlock; ++x) {
                        const float d = std::hypot(x - centre, y - centre) / centre;
                        const float t = std::clamp((1.f - d) / 0.55f, 0.f, 1.f);
                        auto* p = &pixels[(std::size_t(k * beamBlock + y) * beamBlock + x) * 4];
                        p[0] = palette[k][0]; p[1] = palette[k][1]; p[2] = palette[k][2];
                        p[3] = std::uint8_t(std::lround(t * t * (3 - 2 * t) * 255));
                    }
            auto previous = std::move(atlas.pixels);
            const auto previousW = atlas.width, previousH = atlas.height;
            atlas.pixels = std::move(pixels);
            atlas.width = beamBlock;
            atlas.height = rows;
            try { uploadAtlas(beamAtlas); }
            catch (...) {
                atlas.pixels = std::move(previous);
                atlas.width = previousW;
                atlas.height = previousH;
                throw;
            }
            beamPalette = std::move(palette);
        }
        beams = std::move(list);
        spritesStale = true;
    }

    // Rebuilds the vertex buffer. Requires that no submitted work is pending.
    void stageSprites() {
        spritesStale = false;
        spriteBatches.clear();
        if (sprites.empty() && beams.empty()) return;
        const vk::DeviceSize bytes = (sprites.size() + beams.size()) * 6 * sizeof(SpriteVertex);
        if (bytes > spriteCapacity) {
            const auto size = std::max<vk::DeviceSize>(bytes, spriteCapacity * 2);
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, size, vk::BufferUsageFlagBits::eVertexBuffer});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            spriteBuffer = std::move(buffer);
            spriteMemory = std::move(memory);
            spriteCapacity = size;
        }
        auto* out = static_cast<SpriteVertex*>(device->mapMemory(*spriteMemory, 0, bytes));
        std::uint32_t index = 0;
        const float rowsTotal = float(std::max(1u, atlases[beamAtlas].height));
        const auto emitBeam = [&](const MapBeam& b) {
            const auto colour = std::find(beamPalette.begin(), beamPalette.end(), std::array<std::uint8_t, 3>{b.r, b.g, b.b});
            const float row = float(colour - beamPalette.begin()) * beamBlock;
            const float inset = 0.5f / beamBlock, half = b.width / 2;
            const float dx = b.x1 - b.x0, dy = b.y1 - b.y0, length = std::hypot(dx, dy);
            const float bTone = b.additive ? -1.35f : 1.0f;
            SpriteVertex corners[4];
            if (length < 1e-3f) {
                const float v0 = (row + 0.5f) / rowsTotal, v1 = (row + beamBlock - 0.5f) / rowsTotal;
                corners[0] = {b.x0 - half, b.y0 - half, inset, v0, bTone, b.alpha0};
                corners[1] = {b.x0 + half, b.y0 - half, 1 - inset, v0, bTone, b.alpha0};
                corners[2] = {b.x0 + half, b.y0 + half, 1 - inset, v1, bTone, b.alpha0};
                corners[3] = {b.x0 - half, b.y0 + half, inset, v1, bTone, b.alpha0};
            } else {
                const float nx = -dy / length * half, ny = dx / length * half;
                const float v = (row + beamBlock / 2.f) / rowsTotal;   // the widest line through the glow
                corners[0] = {b.x0 + nx, b.y0 + ny, inset, v, bTone, b.alpha0};
                corners[1] = {b.x1 + nx, b.y1 + ny, inset, v, bTone, b.alpha1};
                corners[2] = {b.x1 - nx, b.y1 - ny, 1 - inset, v, bTone, b.alpha1};
                corners[3] = {b.x0 - nx, b.y0 - ny, 1 - inset, v, bTone, b.alpha0};
            }
            for (int corner : {0, 1, 2, 0, 2, 3}) *out++ = corners[corner];
            index += 6;
        };
        const std::size_t underCount = static_cast<std::size_t>(std::count_if(
            beams.begin(), beams.end(), [](const MapBeam& b) { return b.underSprites; }));
        if (underCount > 0) {
            spriteBatches.push_back({beamAtlas, index, std::uint32_t(underCount * 6)});
            for (const auto& b : beams) {
                if (b.underSprites) emitBeam(b);
            }
        }
        for (const auto& s : sprites) {
            const auto& atlas = atlases[s.atlas];
            const float u0 = s.u0 / atlas.width, u1 = s.u1 / atlas.width;
            const float v0 = s.v0 / atlas.height, v1 = s.v1 / atlas.height;
            const float x1 = s.x + s.width, y1 = s.y + s.height;
            const SpriteVertex corners[4] = {{s.x, s.y, u0, v0, s.brightness, s.alpha}, {x1, s.y, u1, v0, s.brightness, s.alpha},
                                             {x1, y1, u1, v1, s.brightness, s.alpha}, {s.x, y1, u0, v1, s.brightness, s.alpha}};
            for (int corner : {0, 1, 2, 0, 2, 3}) *out++ = corners[corner];
            if (spriteBatches.empty() || spriteBatches.back().atlas != s.atlas)
                spriteBatches.push_back({s.atlas, index, 0});
            spriteBatches.back().count += 6;
            index += 6;
        }
        // Beams: a quad along the segment that samples across one colour's glow, or the
        // whole glow when the segment is a point.
        const std::size_t overCount = beams.size() - underCount;
        if (overCount > 0) {
            spriteBatches.push_back({beamAtlas, index, std::uint32_t(overCount * 6)});
            for (const auto& b : beams) {
                if (!b.underSprites) emitBeam(b);
            }
        }
        device->unmapMemory(*spriteMemory);
    }

    // Draws the sprites into the open command buffer. The image arrives and leaves
    // in the transfer-destination layout the rest of the frame uses.
    void drawSprites(vk::Image destination, vk::ImageView view) {
        if (spriteBatches.empty() || !mapScreenRect.extent.width || !mapScreenRect.extent.height) return;
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        const auto colourAccess = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
        vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, colourAccess,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range);
        command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, barrier);
        vk::RenderingAttachmentInfo target;
        target.imageView = view;
        target.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        target.loadOp = vk::AttachmentLoadOp::eLoad;
        target.storeOp = vk::AttachmentStoreOp::eStore;
        vk::RenderingInfo rendering;
        rendering.renderArea = vk::Rect2D({0, 0}, activeExtent);
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &target;
        command.beginRendering(rendering);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *spritePipeline);
        command.setViewport(0, vk::Viewport(0, 0, float(activeExtent.width), float(activeExtent.height), 0, 1));
        command.setScissor(0, mapScreenRect);
        // Map pixel -> screen pixel is the frame's camera transform; then to clip space.
        const float s0 = float(scaleX * 2.0 / activeExtent.width);
        const float s1 = float(scaleY * 2.0 / activeExtent.height);
        const float o0 = float(screenLeft * 2.0 / activeExtent.width - 1.0);
        const float o1 = float(screenTop * 2.0 / activeExtent.height - 1.0);
        const float enc = srgbSurface() ? 1.f : 0.f;
        command.bindVertexBuffers(0, *spriteBuffer, vk::DeviceSize(0));
        float lastSharpness = -1.f;
        for (const auto& batch : spriteBatches) {
            const float sharp = (batch.atlas < VulkanWindow::spriteAtlasSlots) ? spriteSharpness : 0.f;
            if (sharp != lastSharpness) {
                const float push[6] = {s0, s1, o0, o1, enc, sharp};
                command.pushConstants(*spritePipelineLayout,
                    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                    0, sizeof push, push);
                lastSharpness = sharp;
            }
            command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *spritePipelineLayout, 0, atlases[batch.atlas].set, {});
            command.draw(batch.count, 1, batch.first, 0);
        }
        command.endRendering();
        barrier.setOldLayout(vk::ImageLayout::eColorAttachmentOptimal).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
               .setSrcAccessMask(colourAccess).setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, transferStage, {}, {}, {}, barrier);
    }

    void drawOutline(vk::Image destination, MapRect r, int screenW, int screenH, vk::ImageSubresourceLayers layer,
                     MarkerKind kind = MarkerKind::selection, int weight = 1) {
        const vk::MemoryBarrier finished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        const vk::DeviceSize band = vk::DeviceSize(kind) * outlineBand;
        // Computed in 64 bits: a far-off rectangle at high zoom must clip, not overflow.
        const auto edge = [](double v) { return std::int64_t(std::clamp(v, -1e9, 1e9)); };
        const std::int64_t x0 = edge(screenLeft + r.x * scaleX) - 2, y0 = edge(screenTop + r.y * scaleY) - 2;
        const std::int64_t x1 = edge(screenLeft + (double(r.x) + r.width) * scaleX) + 2;
        const std::int64_t y1 = edge(screenTop + (double(r.y) + r.height) * scaleY) + 2;
        const auto line = [&](std::int64_t l, std::int64_t t, std::int64_t right, std::int64_t bottom) {
            l = std::max<std::int64_t>(0, l); t = std::max<std::int64_t>(0, t);
            right = std::min<std::int64_t>(screenW, right); bottom = std::min<std::int64_t>(screenH, bottom);
            if (right <= l || bottom <= t || (right-l) * (bottom-t) > 65536) return;
            // The four sides of one rectangle can touch at clipped corners.
            command.pipelineBarrier(transferStage, transferStage, {}, finished, {}, {});
            command.copyBufferToImage(*outline, destination, vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy(band, 0, 0, layer, {int(l), int(t), 0}, {unsigned(right-l), unsigned(bottom-t), 1}));
        };
        const int totalWeight = weight * std::max(1, int(activeExtent.height) / 1080);
        for (int k = 0; k < totalWeight; ++k) {
            const std::int64_t a = x0 + k, b = y0 + k, c = x1 - k, d = y1 - k;
            if (c - a < 2 || d - b < 2) break;
            line(a, b, c, b+1); line(a, d-1, c, d);
            line(a, b+1, a+1, d-1); line(c-1, b+1, c, d-1);
        }
    }

    // Called with no submitted work pending, so the buffer can be replaced.
    void uploadQuadAtlas(unsigned atlasIdx, vk::UniqueBuffer& quadBuf, vk::UniqueDeviceMemory& quadMem,
                         Overlay& overlay, bool opaque) {
        if (!quadBuf) {
            const vk::DeviceSize bytes = 6 * sizeof(SpriteVertex);
            auto buffer = device->createBufferUnique({{}, bytes, vk::BufferUsageFlagBits::eVertexBuffer});
            auto memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            quadBuf = std::move(buffer);
            quadMem = std::move(memory);
        }
        const bool srgb = srgbSurface();
        const bool unorm = surfaceFormat == vk::Format::eR8G8B8A8Unorm || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!srgb && !unorm) throw std::runtime_error("Sprites require an RGBA8 or BGRA8 surface");
        const auto format = unorm ? vk::Format::eR8G8B8A8Unorm : vk::Format::eR8G8B8A8Srgb;
        auto& atlas = atlases[atlasIdx];
        if (atlas.image && atlas.width == overlay.width && atlas.height == overlay.height && atlas.format == format) {
            ensureSpritePipeline();
            if (overlay.pixels.size() > atlas.stagingCapacity) {
                const vk::DeviceSize cap = std::max<vk::DeviceSize>(overlay.pixels.size(), atlas.stagingCapacity * 2);
                auto buffer = device->createBufferUnique({{}, cap, vk::BufferUsageFlagBits::eTransferSrc});
                auto memory = allocate(device->getBufferMemoryRequirements(*buffer),
                    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
                device->bindBufferMemory(*buffer, *memory, 0);
                atlas.stagingBuffer = std::move(buffer);
                atlas.stagingMemory = std::move(memory);
                atlas.stagingCapacity = cap;
            }
            auto* mapped = static_cast<std::uint8_t*>(device->mapMemory(*atlas.stagingMemory, 0, overlay.pixels.size()));
            if (opaque) std::memcpy(mapped, overlay.pixels.data(), overlay.pixels.size());
            else premultiplyInto(overlay.pixels, mapped, srgb);
            device->unmapMemory(*atlas.stagingMemory);
            // Skip copying overlay.pixels into atlas.pixels during in-place updates;
            // overlay.pixels remains authoritative for any non-format-change rebuild.
            atlas.pendingUpload = true;
            return;
        }
        atlas.pixels = overlay.pixels;
        atlas.width = overlay.width;
        atlas.height = overlay.height;
        atlas.pendingUpload = false;
        uploadAtlas(atlasIdx);
    }

    void uploadOverlay(Overlay& overlay, OverlaySlot slot) {
        overlay.stale = false;
        if (!overlay.width) return;
        if (slot == OverlaySlot::hud && overlay.hasAlpha) {
            uploadQuadAtlas(hudAtlas, hudQuadBuffer, hudQuadMemory, overlay, false);
            return;
        }
        if (slot == OverlaySlot::panel && overlay.stretchFullscreen) {
            uploadQuadAtlas(panelAtlas, panelQuadBuffer, panelQuadMemory, overlay, true);
            return;
        }
        const bool bgr = surfaceFormat == vk::Format::eB8G8R8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!bgr && surfaceFormat != vk::Format::eR8G8B8A8Srgb && surfaceFormat != vk::Format::eR8G8B8A8Unorm)
            throw std::runtime_error("Overlay requires an RGBA8 or BGRA8 surface");
        if (overlay.pixels.size() > overlay.capacity) {
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, overlay.pixels.size(), vk::BufferUsageFlagBits::eTransferSrc});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            overlay.buffer = std::move(buffer);
            overlay.memory = std::move(memory);
            overlay.capacity = overlay.pixels.size();
        }
        auto* pixels = static_cast<std::uint8_t*>(device->mapMemory(*overlay.memory, 0, overlay.pixels.size()));
        if (bgr) {
            if (!SDL_ConvertPixels(int(overlay.width), int(overlay.height),
                                   SDL_PIXELFORMAT_RGBA32, overlay.pixels.data(), int(overlay.width) * 4,
                                   SDL_PIXELFORMAT_BGRA32, pixels, int(overlay.width) * 4)) {
                device->unmapMemory(*overlay.memory);
                throw std::runtime_error(SDL_GetError());
            }
        } else {
            std::memcpy(pixels, overlay.pixels.data(), overlay.pixels.size());
        }
        device->unmapMemory(*overlay.memory);
    }

    void drawOverlay(vk::Image destination, const Overlay& overlay, OverlaySlot slot) {
        const int screenW = activeExtent.width, screenH = activeExtent.height;
        const int w = overlay.width, h = overlay.height;
        const bool placed = slot == OverlaySlot::tooltip;
        const int left = placed ? std::clamp(overlay.left, 0, std::max(0, screenW - w)) : (screenW - w) / 2;
        // The bar is flush with the bottom edge. The notice sits above a bottom margin,
        // and above the bar when one is shown, but never starts above the window.
        const int bar = hudBarHeight > 0 ? int(hudBarHeight) : int(overlays[std::size_t(OverlaySlot::hud)].height);
        const int top = placed ? std::clamp(overlay.top, 0, std::max(0, screenH - h))
                      : slot == OverlaySlot::hud ? screenH - h
                      : slot == OverlaySlot::notice ? std::max(0, screenH - bar - std::max(8, screenH / 16) - h)
                      : (screenH - h) / 2;
        // Whatever does not fit is cropped rather than rejected.
        const int srcX = std::max(0, -left), srcY = std::max(0, -top);
        const int dstX = std::max(0, left), dstY = std::max(0, top);
        const int copyW = std::min(w - srcX, screenW - dstX), copyH = std::min(h - srcY, screenH - dstY);
        if (copyW <= 0 || copyH <= 0) return;
        const vk::MemoryBarrier finished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
        command.pipelineBarrier(transferStage, transferStage, {}, finished, {}, {});
        const vk::ImageSubresourceLayers layer(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
        command.copyBufferToImage(*overlay.buffer, destination, vk::ImageLayout::eTransferDstOptimal,
            vk::BufferImageCopy((vk::DeviceSize(srcY) * w + srcX) * 4, w, 0, layer,
                {dstX, dstY, 0}, {unsigned(copyW), unsigned(copyH), 1}));
    }

    void stageHudQuad(const Overlay& overlay) {
        if (!overlay.width || !overlay.hasAlpha || !hudQuadMemory) return;
        const float left = float((int(activeExtent.width) - int(overlay.width)) / 2);
        const float top = float(int(activeExtent.height) - int(overlay.height));
        const float right = left + float(overlay.width);
        const float bottom = top + float(overlay.height);
        const SpriteVertex corners[4] = {
            {left, top, 0.f, 0.f, 1.f, 1.f},
            {right, top, 1.f, 0.f, 1.f, 1.f},
            {right, bottom, 1.f, 1.f, 1.f, 1.f},
            {left, bottom, 0.f, 1.f, 1.f, 1.f},
        };
        auto* out = static_cast<SpriteVertex*>(device->mapMemory(*hudQuadMemory, 0, 6 * sizeof(SpriteVertex)));
        for (int corner : {0, 1, 2, 0, 2, 3}) *out++ = corners[corner];
        device->unmapMemory(*hudQuadMemory);
    }

    void stagePanelQuad(const Overlay& overlay) {
        if (!overlay.width || !overlay.stretchFullscreen || !panelQuadMemory) return;
        const float right = float(activeExtent.width);
        const float bottom = float(activeExtent.height);
        const SpriteVertex corners[4] = {
            {0.f, 0.f, 0.f, 0.f, 1.f, 1.f},
            {right, 0.f, 1.f, 0.f, 1.f, 1.f},
            {right, bottom, 1.f, 1.f, 1.f, 1.f},
            {0.f, bottom, 0.f, 1.f, 1.f, 1.f},
        };
        auto* out = static_cast<SpriteVertex*>(device->mapMemory(*panelQuadMemory, 0, 6 * sizeof(SpriteVertex)));
        for (int corner : {0, 1, 2, 0, 2, 3}) *out++ = corners[corner];
        device->unmapMemory(*panelQuadMemory);
    }

    void drawQuadOverlay(vk::Image destination, vk::ImageView view, unsigned atlasIdx, vk::Buffer quadBuf) {
        if (!quadBuf || !spritePipeline || !atlases[atlasIdx].set) return;
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        const auto colourAccess = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
        vk::ImageMemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite, colourAccess,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range);
        command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {}, barrier);
        vk::RenderingAttachmentInfo target;
        target.imageView = view;
        target.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        target.loadOp = vk::AttachmentLoadOp::eLoad;
        target.storeOp = vk::AttachmentStoreOp::eStore;
        vk::RenderingInfo rendering;
        rendering.renderArea = vk::Rect2D({0, 0}, activeExtent);
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &target;
        command.beginRendering(rendering);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, *spritePipeline);
        command.setViewport(0, vk::Viewport(0, 0, float(activeExtent.width), float(activeExtent.height), 0, 1));
        command.setScissor(0, vk::Rect2D({0, 0}, activeExtent));
        const bool upscaledPanel = (atlasIdx == panelAtlas) &&
            overlays[std::size_t(OverlaySlot::panel)].stretchFullscreen &&
            atlases[atlasIdx].width > 0 && atlases[atlasIdx].width < activeExtent.width;
        const float push[6] = {2.f / float(activeExtent.width), 2.f / float(activeExtent.height),
                              -1.f, -1.f, srgbSurface() ? 1.f : 0.f, upscaledPanel ? 2.f : 0.f};
        command.pushConstants(*spritePipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              0, sizeof push, push);
        command.bindVertexBuffers(0, quadBuf, vk::DeviceSize(0));
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *spritePipelineLayout, 0, atlases[atlasIdx].set, {});
        command.draw(6, 1, 0, 0);
        command.endRendering();
        barrier.setOldLayout(vk::ImageLayout::eColorAttachmentOptimal).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
               .setSrcAccessMask(colourAccess).setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, transferStage, {}, {}, {}, barrier);
    }

    void record(vk::Image image, vk::ImageView view, vk::Buffer capture = {}) {
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        vk::ImageMemoryBarrier barrier({}, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, range);
        command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
        if (hasMap() && !patches.empty()) recordPatches();
        for (unsigned aIdx : {hudAtlas, panelAtlas}) {
            if (atlases[aIdx].pendingUpload && atlases[aIdx].image && atlases[aIdx].stagingBuffer) {
                auto& quadA = atlases[aIdx];
                const vk::ImageSubresourceRange quadRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
                vk::ImageMemoryBarrier toDst({}, vk::AccessFlagBits::eTransferWrite,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *quadA.image, quadRange);
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, transferStage, {}, {}, {}, toDst);
                command.copyBufferToImage(*quadA.stagingBuffer, *quadA.image, vk::ImageLayout::eTransferDstOptimal,
                    vk::BufferImageCopy(0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {},
                                        {quadA.width, quadA.height, 1}));
                vk::ImageMemoryBarrier toRead(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *quadA.image, quadRange);
                command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, toRead);
                quadA.pendingUpload = false;
            }
        }
        const vk::ClearColorValue color(std::array<float, 4>{0.055f, 0.12f, 0.16f, 1.f});
        command.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, color, range);
        if (hasMap()) {
            const vk::MemoryBarrier clearFinished(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferWrite);
            command.pipelineBarrier(transferStage, transferStage, {}, clearFinished, {}, {});
            drawMap(image, view);
            drawSprites(image, view);
            drawBars(image);
            drawOutlines(image);
        }
        for (auto slot : {OverlaySlot::hud, OverlaySlot::tooltip, OverlaySlot::notice, OverlaySlot::panel, OverlaySlot::menu}) {
            if (const auto& overlay = overlays[std::size_t(slot)]; overlay.width) {
                if (slot == OverlaySlot::hud && overlay.hasAlpha) drawQuadOverlay(image, view, hudAtlas, *hudQuadBuffer);
                else if (slot == OverlaySlot::panel && overlay.stretchFullscreen) drawQuadOverlay(image, view, panelAtlas, *panelQuadBuffer);
                else drawOverlay(image, overlay, slot);
            }
        }
        auto finalLayout = vk::ImageLayout::eTransferDstOptimal;
        auto finalAccess = vk::AccessFlags(vk::AccessFlagBits::eTransferWrite);
        if (capture) {
            barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                   .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask(vk::AccessFlagBits::eTransferRead);
            command.pipelineBarrier(transferStage, transferStage, {}, {}, {}, barrier);
            command.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, capture,
                vk::BufferImageCopy(0, 0, 0, {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {},
                                    {activeExtent.width, activeExtent.height, 1}));
            finalLayout = vk::ImageLayout::eTransferSrcOptimal;
            finalAccess = vk::AccessFlagBits::eTransferRead;
        }
        barrier.setOldLayout(finalLayout)
               .setNewLayout(vk::ImageLayout::ePresentSrcKHR)
               .setSrcAccessMask(finalAccess).setDstAccessMask({});
        command.pipelineBarrier(transferStage, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, barrier);
        command.end();
    }

    bool present(vk::Buffer capture = {}) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) return false;
        try {
            if (dirty && !rebuild()) return false;
            const std::uint64_t nowNs = SDL_GetTicksNS();
            double dt = 0.0;
            if (lastCameraTickNs != 0 && nowNs > lastCameraTickNs) {
                dt = std::min(0.1, double(nowNs - lastCameraTickNs) * 1e-9);
            } else if (animatingCamera) {
                dt = 1.0 / 60.0;
            }
            lastCameraTickNs = nowNs;
            if (animatingCamera && hasMap()) {
                const double stepDt = std::max(dt, 1.0 / 240.0);
                const double alpha = 1.0 - std::exp(-stepDt * 18.0);
                scale += (targetScale - scale) * alpha;
                centerX += (targetCenterX - centerX) * alpha;
                centerY += (targetCenterY - centerY) * alpha;
                if (std::abs(targetScale - scale) < 1e-4 &&
                    std::abs(targetCenterX - centerX) < 0.05 &&
                    std::abs(targetCenterY - centerY) < 0.05) {
                    scale = targetScale;
                    centerX = targetCenterX;
                    centerY = targetCenterY;
                    animatingCamera = false;
                }
                clampCenter(centerX, centerY, scale);
            }
            if (mapGrade.enabled && canSampleMap && (!mapPipeline || mapPipelineFormat != surfaceFormat))
                ensureMapPipeline();
            if (device->waitForFences(*submitted, true, acquireTimeoutNs) != vk::Result::eSuccess) return false;
            for (std::size_t s = 0; s < overlays.size(); ++s)
                if (overlays[s].stale) uploadOverlay(overlays[s], OverlaySlot(s));
            const bool needHudQuad = overlays[std::size_t(OverlaySlot::hud)].width &&
                                     overlays[std::size_t(OverlaySlot::hud)].hasAlpha;
            const bool needPanelQuad = overlays[std::size_t(OverlaySlot::panel)].width &&
                                       overlays[std::size_t(OverlaySlot::panel)].stretchFullscreen;
            if ((needHudQuad || needPanelQuad) && (!spritePipeline || spritePipelineFormat != surfaceFormat))
                ensureSpritePipeline();
            stageHudQuad(overlays[std::size_t(OverlaySlot::hud)]);
            stagePanelQuad(overlays[std::size_t(OverlaySlot::panel)]);
            stagePatches();
            if (spritesStale) stageSprites();
            auto [status, index] = device->acquireNextImageKHR(*chain, acquireTimeoutNs, *acquired);
            if (status == vk::Result::eTimeout || status == vk::Result::eNotReady) return false;
            record(images.at(index), *views.at(index), capture);
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
        } catch (const vk::SurfaceLostKHRError&) {
            recoverLostSurface();
            return false;
        }
    }

    void recoverLostSurface() {
        device->waitIdle();
        views.clear();
        chain.reset();
        rendered.clear();
        // VUID-vkDestroySurfaceKHR-surface-01266: swapchain must be destroyed before surface.
        surface.reset();
        VkSurfaceKHR handle{};
        if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &handle))
            throw std::runtime_error(SDL_GetError());
        surface = vk::UniqueSurfaceKHR(handle, *instance);
        if (!SDL_Vulkan_GetPresentationSupport(*instance, gpu, family) ||
            !gpu.getSurfaceSupportKHR(family, *surface))
            throw std::runtime_error("Recreated Vulkan surface is not presentable on active queue");
        dirty = true;
    }

    assets::MapImage captureFrame() {
        if (dirty && !rebuild()) throw std::runtime_error("Frame capture needs a drawable window");
        if (!canCapture) throw std::runtime_error("This surface cannot be read back");
        const bool bgr = surfaceFormat == vk::Format::eB8G8R8A8Srgb || surfaceFormat == vk::Format::eB8G8R8A8Unorm;
        if (!bgr && surfaceFormat != vk::Format::eR8G8B8A8Srgb && surfaceFormat != vk::Format::eR8G8B8A8Unorm)
            throw std::runtime_error("Frame capture requires an RGBA8 or BGRA8 surface");
        for (int attempt = 0; attempt < 200; ++attempt) {
            SDL_PumpEvents();
            if (dirty && !rebuild()) { SDL_Delay(5); continue; }
            device->waitIdle();
            const auto extent = activeExtent;
            const vk::DeviceSize bytes = vk::DeviceSize(extent.width) * extent.height * 4;
            vk::UniqueDeviceMemory memory;
            auto buffer = device->createBufferUnique({{}, bytes, vk::BufferUsageFlagBits::eTransferDst});
            memory = allocate(device->getBufferMemoryRequirements(*buffer),
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
            device->bindBufferMemory(*buffer, *memory, 0);
            const bool shown = present(*buffer);
            // The buffer outlives the frame that wrote it, whatever present reported.
            device->waitIdle();
            if (!shown) { SDL_Delay(5); continue; }
            assets::MapImage result{extent.width, extent.height, {}};
            result.rgba.resize(bytes);
            const auto* pixels = device->mapMemory(*memory, 0, bytes);
            std::memcpy(result.rgba.data(), pixels, bytes);
            device->unmapMemory(*memory);
            if (bgr) for (std::size_t i = 0; i < result.rgba.size(); i += 4) std::swap(result.rgba[i], result.rgba[i + 2]);
            return result;
        }
        throw std::runtime_error("Timed out waiting to capture a frame");
    }

    ~Impl() {
        // Destruction also covers a failed present. A lost device can still be destroyed.
        if (device) (void)vkDeviceWaitIdle(*device);
    }
};

VulkanWindow::VulkanWindow(SDL_Window* w, ValidationReport* report) : impl_(std::make_unique<Impl>(w, report)) {}
VulkanWindow::~VulkanWindow() = default;
void VulkanWindow::setMap(const assets::MapImage& map, unsigned maxTileSize) { impl_->setMap(map, maxTileSize); }
void VulkanWindow::updateMapRegion(int x, int y, const assets::MapImage& patch) { impl_->queuePatch(x, y, patch); }
assets::MapImage VulkanWindow::captureMap() { return impl_->captureMap(); }
void VulkanWindow::focusMap(int x, int y, bool zoomFromOverview) {
    if (zoomFromOverview && impl_->transformReady && impl_->scale <= impl_->fitScale())
        impl_->scale = std::max(1.0,impl_->scale);
    impl_->centerX = x;
    impl_->centerY = y;
    impl_->targetScale = impl_->scale;
    impl_->targetCenterX = impl_->centerX;
    impl_->targetCenterY = impl_->centerY;
    impl_->animatingCamera = false;
}
void VulkanWindow::smoothFocusMap(double mapX, double mapY) {
    if (!std::isfinite(mapX) || !std::isfinite(mapY)) return;
    impl_->targetCenterX = mapX;
    impl_->targetCenterY = mapY;
    impl_->targetScale = impl_->animatingCamera ? impl_->targetScale : impl_->scale;
    impl_->clampCenter(impl_->targetCenterX, impl_->targetCenterY, impl_->targetScale);
    impl_->animatingCamera = true;
}
void VulkanWindow::zoomBy(double factor) {
    if (!impl_->hasMap() || !std::isfinite(factor) || factor <= 0.0) return;
    const double minScale = std::min(1.0, impl_->fitScale());
    const double prevTarget = impl_->animatingCamera ? impl_->targetScale : impl_->scale;
    if (!impl_->animatingCamera) {
        impl_->targetCenterX = impl_->centerX;
        impl_->targetCenterY = impl_->centerY;
    }
    impl_->targetScale = std::clamp(prevTarget * factor, minScale, 8.0);
    impl_->clampCenter(impl_->targetCenterX, impl_->targetCenterY, impl_->targetScale);
    impl_->animatingCamera = true;
}
void VulkanWindow::resetZoom() {
    if (!impl_->hasMap()) return;
    const double minScale = std::min(1.0, impl_->fitScale());
    if (!impl_->animatingCamera) {
        impl_->targetCenterX = impl_->centerX;
        impl_->targetCenterY = impl_->centerY;
    }
    impl_->targetScale = std::clamp(1.0, minScale, 8.0);
    impl_->clampCenter(impl_->targetCenterX, impl_->targetCenterY, impl_->targetScale);
    impl_->animatingCamera = true;
}
void VulkanWindow::fitMapOverview() {
    if (!impl_->hasMap()) return;
    impl_->targetCenterX = static_cast<double>(impl_->mapSize.width) * 0.5;
    impl_->targetCenterY = static_cast<double>(impl_->mapSize.height) * 0.5;
    impl_->targetScale = impl_->fitScale();
    impl_->clampCenter(impl_->targetCenterX, impl_->targetCenterY, impl_->targetScale);
    impl_->animatingCamera = true;
}
void VulkanWindow::panMap(double screenDx, double screenDy) {
    if (!impl_->hasMap() || (screenDx == 0.0 && screenDy == 0.0)) return;
    const auto [sx, sy] = impl_->windowScale();
    const double dx = screenDx * sx / impl_->scale;
    const double dy = screenDy * sy / impl_->scale;
    impl_->centerX += dx;
    impl_->centerY += dy;
    impl_->targetCenterX = impl_->centerX;
    impl_->targetCenterY = impl_->centerY;
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
void VulkanWindow::setMarkers(std::vector<MapMarker> list) {
    for (auto& m : list) {
        if (m.kind != MarkerKind::selection && m.kind != MarkerKind::attack && m.kind != MarkerKind::move)
            throw std::invalid_argument("Unknown marker kind");
        auto& r = m.rect;
        const int right = int(std::clamp<std::int64_t>(std::int64_t(r.x) + r.width, 0, impl_->mapSize.width));
        const int bottom = int(std::clamp<std::int64_t>(std::int64_t(r.y) + r.height, 0, impl_->mapSize.height));
        r.x = std::clamp(r.x, 0, int(impl_->mapSize.width));
        r.y = std::clamp(r.y, 0, int(impl_->mapSize.height));
        r.width = right - r.x;
        r.height = bottom - r.y;
    }
    std::erase_if(list, [](const MapMarker& m) { return m.rect.width <= 0 || m.rect.height <= 0; });
    impl_->markers = std::move(list);
}
void VulkanWindow::setBars(std::vector<MapBar> list) {
    for (const auto& b : list)
        for (float v : {b.x, b.y, b.width, b.value})
            if (!std::isfinite(v)) throw std::invalid_argument("Gauge has a non-finite value");
    std::erase_if(list, [](const MapBar& b) { return b.width <= 0; });
    for (auto& b : list) b.value = std::clamp(b.value, 0.f, 1.f);
    impl_->bars = std::move(list);
}
void VulkanWindow::setMapFilterLinear(bool linear) { impl_->mapLinear = linear; }
void VulkanWindow::setMapGrade(const MapGrade& grade) {
    for (float v : {grade.sharpness, grade.vignette, grade.rGain, grade.gGain, grade.bGain,
                    grade.contrast, grade.timeSec, grade.waterShimmer})
        if (!std::isfinite(v)) throw std::invalid_argument("Map grade has a non-finite value");
    MapGrade clamped = grade;
    clamped.sharpness = std::clamp(grade.sharpness, 0.f, 1.f);
    clamped.vignette = std::clamp(grade.vignette, 0.f, 0.5f);
    clamped.rGain = std::clamp(grade.rGain, 0.5f, 1.5f);
    clamped.gGain = std::clamp(grade.gGain, 0.5f, 1.5f);
    clamped.bGain = std::clamp(grade.bGain, 0.5f, 1.5f);
    clamped.contrast = std::clamp(grade.contrast, 0.5f, 1.5f);
    clamped.waterShimmer = std::clamp(grade.waterShimmer, 0.f, 2.f);
    impl_->mapGrade = clamped;
}
void VulkanWindow::setWeatherEffect(int kind, float phase, float flash) {
    impl_->weatherKind = std::clamp(kind, 0, 3);
    impl_->weatherPhase = std::isfinite(phase) ? phase : 0.0f;
    impl_->weatherFlash = std::isfinite(flash) ? std::clamp(flash, 0.0f, 1.0f) : 0.0f;
}
void VulkanWindow::setHudBarHeight(unsigned height) {
    impl_->hudBarHeight = height;
}
void VulkanWindow::setPreferSoftwareRenderer(bool prefer) { impl_->preferSoftware = prefer; }
void VulkanWindow::setPreferSoftwareRendererDefault(bool prefer) { gPreferSoftwareRenderer = prefer; }
void VulkanWindow::setSpriteAtlas(unsigned slot, const assets::MapImage& atlas) { impl_->setSpriteAtlas(slot, atlas); }
void VulkanWindow::setSprites(std::vector<Sprite> list) {
    for (const auto& s : list) {
        if (s.atlas >= spriteAtlasSlots || !impl_->atlases[s.atlas].image)
            throw std::invalid_argument("Sprite names an atlas slot that has not been set");
        for (float v : {s.x, s.y, s.width, s.height, s.u0, s.v0, s.u1, s.v1, s.brightness, s.alpha})
            if (!std::isfinite(v)) throw std::invalid_argument("Sprite has a non-finite value");
    }
    // Nothing to draw for empty or fully transparent rectangles.
    std::erase_if(list, [](const Sprite& s) { return s.width <= 0 || s.height <= 0 || s.alpha <= 0; });
    for (auto& s : list) {
        s.alpha = std::min(s.alpha, 1.f);
        if (s.brightness <= -100.f) s.brightness = std::clamp(s.brightness, -104.f, -100.f);
        else s.brightness = std::clamp(s.brightness, 0.f, 1.6f);
    }
    impl_->sprites = std::move(list);
    impl_->spritesStale = true;
}
void VulkanWindow::setSpriteSharpness(float sharpness) {
    impl_->spriteSharpness = std::isfinite(sharpness) ? std::clamp(sharpness, 0.f, 4.f) : 0.f;
}
void VulkanWindow::setBeams(std::vector<MapBeam> list) {
    for (const auto& b : list)
        for (float v : {b.x0, b.y0, b.x1, b.y1, b.width, b.alpha0, b.alpha1})
            if (!std::isfinite(v)) throw std::invalid_argument("Beam has a non-finite value");
    std::erase_if(list, [](const MapBeam& b) { return b.width <= 0 || (b.alpha0 <= 0 && b.alpha1 <= 0); });
    for (auto& b : list) {
        b.alpha0 = std::clamp(b.alpha0, 0.f, 1.f);
        b.alpha1 = std::clamp(b.alpha1, 0.f, 1.f);
    }
    impl_->setBeams(std::move(list));
}
assets::MapImage VulkanWindow::captureFrame() { return impl_->captureFrame(); }
bool VulkanWindow::surfaceIsSrgb() const { return impl_->srgbSurface(); }
void VulkanWindow::setOverlay(unsigned width, unsigned height, std::span<const std::uint8_t> rgba,
                              OverlaySlot slot, bool stretchFullscreen) {
    if (!width || !height || width > 16384 || height > 16384 || std::uint64_t(width) * height * 4 != rgba.size())
        throw std::invalid_argument("Unsupported overlay dimensions or pixel count");
    auto& overlay = impl_->overlays.at(std::size_t(slot));
    overlay.pixels.assign(rgba.begin(), rgba.end());
    overlay.width = width;
    overlay.height = height;
    overlay.hasAlpha = false;
    overlay.stretchFullscreen = (slot == OverlaySlot::panel) && stretchFullscreen;
    if (slot == OverlaySlot::hud) {
        for (std::size_t i = 3; i < rgba.size(); i += 4) {
            if (rgba[i] < 255) { overlay.hasAlpha = true; break; }
        }
    }
    overlay.stale = true;
}
void VulkanWindow::clearOverlay(OverlaySlot slot) {
    auto& overlay = impl_->overlays.at(std::size_t(slot));
    overlay.width = overlay.height = 0;
    overlay.stale = false;
    overlay.hasAlpha = false;
    overlay.stretchFullscreen = false;
    if (slot == OverlaySlot::hud) impl_->hudBarHeight = 0;
}
void VulkanWindow::moveOverlay(OverlaySlot slot, int left, int top) {
    auto& overlay = impl_->overlays.at(std::size_t(slot));
    overlay.left = left;
    overlay.top = top;
}
void VulkanWindow::handleEvent(const SDL_Event& event) {
    impl_->cameraEvent(event);
    // The size event queued at window creation repeats the size a swapchain built
    // before the first event pump already has; rebuilding for it would be wasted work.
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event.window.windowID == SDL_GetWindowID(impl_->window) &&
        (!impl_->chain || event.window.data1 != impl_->builtWidth || event.window.data2 != impl_->builtHeight))
        impl_->dirty = true;
}
std::pair<unsigned, unsigned> VulkanWindow::extent() const {
    return {impl_->activeExtent.width, impl_->activeExtent.height};
}
bool VulkanWindow::present(const Snapshot&) { return impl_->present({}); }
}
