// Drive rare WSI responses through the production renderer and a real Vulkan device.
// The three replacements are linked only into this Linux test executable.
#include "render/window.hpp"
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
enum class Fault { none, zeroExtent, acquireOutOfDate, presentOutOfDate, acquireSurfaceLost, presentSurfaceLost };
Fault nextFault = Fault::none;
bool consume(Fault fault) {
    if (nextFault != fault) return false;
    nextFault = Fault::none;
    return true;
}

template<class F> F next(const char* symbol) {
    auto proc = reinterpret_cast<F>(dlsym(RTLD_NEXT, symbol));
    if (!proc) std::terminate();
    return proc;
}
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice device, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps) {
    static auto query = next<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(__func__);
    auto result = query(device, surface, caps);
    if (result == VK_SUCCESS && consume(Fault::zeroExtent)) caps->currentExtent = {0, 0};
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device,
    VkSwapchainKHR chain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    if (consume(Fault::acquireOutOfDate)) return VK_ERROR_OUT_OF_DATE_KHR;
    if (consume(Fault::acquireSurfaceLost)) return VK_ERROR_SURFACE_LOST_KHR;
    static auto acquire = next<PFN_vkAcquireNextImageKHR>(__func__);
    return acquire(device, chain, timeout, semaphore, fence, index);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    static auto present = next<PFN_vkQueuePresentKHR>(__func__);
    auto result = present(queue, info);
    if (result == VK_SUCCESS) {
        if (consume(Fault::presentOutOfDate)) return VK_ERROR_OUT_OF_DATE_KHR;
        if (consume(Fault::presentSurfaceLost)) return VK_ERROR_SURFACE_LOST_KHR;
    }
    return result;
}

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    ss2::ValidationReport report;
    int status = 0;
    try {
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow("WSI recovery test", 320, 240, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
        if (!window) throw std::runtime_error(SDL_GetError());
        bool validate = argc == 2 && std::string_view(argv[1]) == "--validate";
        ss2::VulkanWindow renderer(window.get(), validate ? &report : nullptr);
        for (auto fault : {Fault::zeroExtent, Fault::acquireOutOfDate, Fault::presentOutOfDate,
                           Fault::acquireSurfaceLost, Fault::presentSurfaceLost}) {
            nextFault = fault;
            bool recovered = false;
            for (int attempt = 0; attempt < 100 && !recovered; ++attempt) {
                SDL_PumpEvents();
                bool awaitingFault = nextFault != Fault::none;
                bool presented = renderer.present({});
                if (awaitingFault && nextFault == Fault::none && presented)
                    throw std::runtime_error("Fault counted as a presented frame");
                recovered = !awaitingFault && presented;
                if (!recovered) SDL_Delay(5);
            }
            if (!recovered) throw std::runtime_error("Did not recover presentation from the WSI fault");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    SDL_Quit();
    return report.errors.load() ? 1 : status;
}
