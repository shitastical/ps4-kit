// ps4-kit: Vulkan window system integration on VideoOut. Merged from SSBU's port/ps4_vk_wsi.cpp (SMW's, from
// Resonance's ps4_wsi.cpp and Crema's PS4WSI.cpp), with the ideas the Vita3K/M33/Conker branch of the same file grew
// written anew here: buffer banks with retired swapchains, B8G8R8A8, storage images, vblank waits, flip_pending.
// docs/merge-notes/wsi.md says which behavior came from where.
//
// RADV (the GNM build) is linked into the executable and has no WSI. This file adds VK_EXT_headless_surface and
// VK_KHR_swapchain on top of it: the swapchain images are linear R8G8B8A8 or B8G8R8A8 images in host-visible memory
// (GPU address = CPU address on the PS4), registered as VideoOut buffers, and presenting queues a VideoOut flip behind
// the GPU work submitted so far (ac_ps4_submit_flip in the Mesa winsys). Apps reach all of it through
// ps4kit::vk_get_instance_proc_addr().
//
// Semaphores: there is one in-order queue, and on the PS4 a submit that waits for or signals a binary semaphore with
// no other work loses the device (Crema, Resonance). The acquire semaphore is never signalled here (the image is off
// screen when vkAcquireNextImageKHR returns), so every submit drops its BINARY semaphores; timeline semaphores, which
// DXVK tracks its GPU work with, are kept (the winsys emulates timeline sync objects).
#include "ps4_vk_wsi.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

extern "C" int sceVideoOutClose(int handle);  // exported by libSceVideoOut, not in every SDK header

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char* name);
// Mesa's ac_ps4_drm.c (gpu/mesa-patches): queues a VideoOut flip behind the submitted GPU work (GPU-side flip).
// Weak: with a Mesa without it, presents fall back to a CPU wait and a CPU flip.
extern "C" int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg) __attribute__((weak));
// runtime/ps4_crashlog.cpp; weak so this file also links without it
extern "C" void ps4_boot_trace(const char* stage) __attribute__((weak));

namespace ps4kit {

namespace {

constexpr uint32_t kMaxImages = 4;
constexpr VkDeviceSize kFramebufferAlign = 2 << 20;  // VideoOut buffer alignment
// VideoOut's A8R8G8B8 (missing from the OpenOrbis headers) is B8G8R8A8 in memory; A8B8G8R8 is R8G8B8A8 (DolphinPS4).
constexpr uint32_t kPixelFormatA8R8G8B8Srgb = 0x80000000u;
// sceKernelWaitEqueue's timeout (SCE_KERNEL_ERROR_ETIMEDOUT)
constexpr int kErrorTimedOut = int(0x8002003C);

// VideoOut buffer slots 0-11 as 3 banks of kMaxImages; a swapchain takes a free bank. A renderer makes its new
// swapchain while the old one is still on screen (or still queued to be: the flips are the GPU's), and
// sceVideoOutUnregisterBuffers fails for a buffer on screen or queued. SMW's two alternating index ranges ignored
// that failure: the memory was freed while VideoOut could still scan it out, and the slots stayed taken for the next
// swapchain but one. With banks, a set VideoOut won't give back yet is parked, its memory kept, and released after the
// next flips. Three banks: the live swapchain, one being replaced, one parked.
constexpr int kBanks = 3;
static_assert(kBanks * int(kMaxImages) <= kAppFirstBuffer, "slots 12-15 are the app's");

struct Surface {
    uint32_t magic = 0x50533453;
};

struct Swapchain {
    VkDevice device = VK_NULL_HANDLE;  // VK_NULL_HANDLE once the device is gone (parked swapchains)
    uint32_t image_count = 0;
    VkImage images[kMaxImages]{};
    VkDeviceMemory memory[kMaxImages]{};
    // flip number (= flipArg) that last showed each image; 0: never shown
    std::atomic<int64_t> shown_by[kMaxImages]{};
    int buffer_set = -1;
    int first_buffer = 0;
    uint32_t next_image = 0;
    uint32_t flip_mode = ORBIS_VIDEO_OUT_FLIP_VSYNC;
    uint32_t pitch = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    // window mode (set_window_present): the size asked for, presented to the hook, never registered with VideoOut
    bool window = false;
    bool coherent = true;  // (else the CPU's cache of the image is dropped before each present)
    uint64_t surface = 0;
    uint32_t width = kScreenWidth, height = kScreenHeight;
    uint32_t row_bytes = 0;
    uint8_t* pixels[kMaxImages]{};
};

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
// the real submits of g_device (resolved once per device, not per submit)
PFN_vkQueueSubmit g_queue_submit = nullptr;
PFN_vkQueueSubmit2 g_queue_submit2 = nullptr;

int g_video = -1;
OrbisKernelEqueue g_flip_queue = 0;
std::mutex g_video_lock;

std::atomic<uint64_t> g_flips{0};
std::atomic<uint64_t> g_last_present_ns{0};

// swapchain creation, banks and the parked swapchains
std::mutex g_lock;
bool g_bank_used[kBanks]{};
std::vector<Swapchain*> g_retired;
std::atomic<size_t> g_retired_count{0};  // read by every present without the lock
// presents, so flip numbers go to VideoOut in the order they are counted
std::mutex g_present_lock;

// timeline semaphores (kept in submits; every other semaphore is dropped)
std::shared_mutex g_timeline_lock;
std::unordered_set<VkSemaphore> g_timelines;

// options
std::atomic<void (*)(const char*)> g_log_hook{nullptr};
std::atomic<int> g_max_steps{400};
std::atomic<bool> g_stats_log{true};
std::atomic<bool> g_force_vsync{false};
std::atomic<WindowPresent> g_window_present{nullptr};
void (*g_window_present_c)(unsigned long long, const void*, unsigned, unsigned, unsigned, int) = nullptr;

void emit(const char* line) {
    if (auto hook = g_log_hook.load(std::memory_order_relaxed))
        hook(line);
    else if (ps4_boot_trace)
        ps4_boot_trace(line);
}

void log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit(line);
}

// Step trace: one direct write per line to boot-trace.log (runtime/ps4_crashlog.cpp), so the last line before a
// crash that leaves no crash.log (the system ending the app) says which call it was. The first frames only.
std::atomic<int> g_steps{0};
void step(const char* fmt, ...) {
    if (!ps4_boot_trace || g_steps.fetch_add(1, std::memory_order_relaxed) >= g_max_steps.load()) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ps4_boot_trace(line);
}

uint64_t now_ns() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

template <typename T>
T device_fn(VkDevice device, const char* name) {
    return reinterpret_cast<T>(vk_common_GetDeviceProcAddr(device, name));
}

template <typename T>
T instance_fn(const char* name) {
    return reinterpret_cast<T>(vk_icdGetInstanceProcAddr(g_instance, name));
}

bool open_video_out() {
    std::lock_guard lk(g_video_lock);
    if (g_video >= 0) return true;
    step("wsi: sceVideoOutOpen");
    const int video = sceVideoOutOpen(0xFF, 0, 0, nullptr);  // (the system user, the main bus)
    step("wsi: sceVideoOutOpen = %x", unsigned(video));
    if (video < 0) {
        log("ps4kit/wsi: sceVideoOutOpen failed: %x", unsigned(video));
        return false;
    }
    int r = sceKernelCreateEqueue(&g_flip_queue, "ps4kit flip");
    step("wsi: sceKernelCreateEqueue = %x", unsigned(r));
    if (r >= 0) r = sceVideoOutAddFlipEvent(g_flip_queue, video, nullptr);
    step("wsi: sceVideoOutAddFlipEvent = %x", unsigned(r));
    if (r < 0) {  // acquires then poll the flip status every millisecond instead
        log("ps4kit/wsi: no flip event queue (%x)", unsigned(r));
        g_flip_queue = 0;
    }
    g_video = video;
    return true;
}

// Appends extensions to an enumeration result (count/properties two-call idiom).
VkResult append_extensions(std::vector<VkExtensionProperties>& list,
                           std::initializer_list<std::pair<const char*, uint32_t>> extra, uint32_t* count,
                           VkExtensionProperties* props) {
    for (const auto& [name, spec] : extra) {
        VkExtensionProperties p{};
        std::strncpy(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
        p.specVersion = spec;
        list.push_back(p);
    }
    if (!props) {
        *count = uint32_t(list.size());
        return VK_SUCCESS;
    }
    uint32_t n = std::min<uint32_t>(*count, uint32_t(list.size()));
    std::memcpy(props, list.data(), n * sizeof(VkExtensionProperties));
    *count = n;
    return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

template <typename T, size_t N>
VkResult fill(const T (&supported)[N], uint32_t* count, T* out) {
    if (!out) {
        *count = uint32_t(N);
        return VK_SUCCESS;
    }
    uint32_t n = std::min<uint32_t>(*count, uint32_t(N));
    std::memcpy(out, supported, n * sizeof(T));
    *count = n;
    return n < N ? VK_INCOMPLETE : VK_SUCCESS;
}

std::vector<const char*> without(uint32_t count, const char* const* names, std::initializer_list<const char*> drop) {
    std::vector<const char*> out;
    for (uint32_t k = 0; k < count; k++)
        if (std::none_of(drop.begin(), drop.end(), [&](const char* d) { return std::strcmp(names[k], d) == 0; }))
            out.push_back(names[k]);
    return out;
}

// ---- instance level -------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                                    VkExtensionProperties* props) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;  // (no loader, no layers: not RADV's list under a layer's name)
    auto real = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    uint32_t n = 0;
    real(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    real(nullptr, &n, list.data());
    list.resize(n);
    return append_extensions(list, {{VK_KHR_SURFACE_EXTENSION_NAME, 25}, {VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, 1}},
                             count, props);
}

// An ICD has no layer enumeration, and RADV's vk_icdGetInstanceProcAddr answers null for it: a renderer that asks
// which layers there are (vulkan.hpp, volk users, every debug build) would call a null pointer.
VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                              VkInstance* instance) {
    auto real = reinterpret_cast<PFN_vkCreateInstance>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    VkInstanceCreateInfo copy = *info;
    auto names = without(info->enabledExtensionCount, info->ppEnabledExtensionNames,
                         {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME});
    copy.enabledExtensionCount = uint32_t(names.size());
    copy.ppEnabledExtensionNames = names.data();
    // there is no loader to put layers in: a debug build asking for validation gets none rather than an error
    if (info->enabledLayerCount)
        log("ps4kit/wsi: %u layers asked for, none loaded (no loader)", info->enabledLayerCount);
    copy.enabledLayerCount = 0;
    copy.ppEnabledLayerNames = nullptr;
    VkResult r = real(&copy, alloc, instance);
    if (r == VK_SUCCESS) g_instance = *instance;
    log("ps4kit/wsi: vkCreateInstance = %d", int(r));
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char* layer,
                                                                  uint32_t* count, VkExtensionProperties* props) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    auto real = instance_fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    real(physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    real(physical, nullptr, &n, list.data());
    list.resize(n);
    return append_extensions(list, {{VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70}}, count, props);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* alloc, VkDevice* device) {
    auto real = instance_fn<PFN_vkCreateDevice>("vkCreateDevice");
    VkDeviceCreateInfo copy = *info;
    auto names = without(info->enabledExtensionCount, info->ppEnabledExtensionNames, {VK_KHR_SWAPCHAIN_EXTENSION_NAME});
    copy.enabledExtensionCount = uint32_t(names.size());
    copy.ppEnabledExtensionNames = names.data();
    copy.enabledLayerCount = 0;
    copy.ppEnabledLayerNames = nullptr;
    g_physical = physical;
    VkResult r = real(physical, &copy, alloc, device);
    if (r == VK_SUCCESS) {
        g_device = *device;
        g_queue_submit = device_fn<PFN_vkQueueSubmit>(*device, "vkQueueSubmit");
        // (core in 1.3; a 1.2 device with VK_KHR_synchronization2 only has the KHR name)
        g_queue_submit2 = device_fn<PFN_vkQueueSubmit2>(*device, "vkQueueSubmit2");
        if (!g_queue_submit2) g_queue_submit2 = device_fn<PFN_vkQueueSubmit2>(*device, "vkQueueSubmit2KHR");
    }
    log("ps4kit/wsi: vkCreateDevice with %u extensions = %d", copy.enabledExtensionCount, int(r));
    for (uint32_t k = 0; k < copy.enabledExtensionCount; k++) log("ps4kit/wsi:   %s", copy.ppEnabledExtensionNames[k]);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateHeadlessSurfaceEXT(VkInstance, const VkHeadlessSurfaceCreateInfoEXT*,
                                                        const VkAllocationCallbacks*, VkSurfaceKHR* surface) {
    *surface = reinterpret_cast<VkSurfaceKHR>(new Surface());
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*) {
    delete reinterpret_cast<Surface*>(surface);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t family, VkSurfaceKHR,
                                                                  VkBool32* supported) {
    *supported = family == 0 ? VK_TRUE : VK_FALSE;  // the graphics queue family
    return VK_SUCCESS;
}

// Whether linear images of format can be storage images (written from compute shaders: FSR's sharpening pass writes
// the swapchain image that way).
bool linear_storage(VkPhysicalDevice physical, VkFormat format) {
    VkFormatProperties props{};
    instance_fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, format,
                                                                                                &props);
    return (props.linearTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR,
                                                                       VkSurfaceCapabilitiesKHR* caps) {
    *caps = {};
    caps->minImageCount = 2;
    caps->maxImageCount = 3;  // as DolphinPS4: render into one while one is shown and one is queued
    caps->currentExtent = {kScreenWidth, kScreenHeight};
    caps->minImageExtent = caps->currentExtent;
    caps->maxImageExtent = caps->currentExtent;
    if (g_window_present.load()) {  // (a window: any size; the app's window system says which)
        caps->currentExtent = {0xffffffffu, 0xffffffffu};
        caps->minImageExtent = {1, 1};
        caps->maxImageExtent = {4096, 4096};
    }
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // (the capabilities aren't per format: R8G8B8A8's answer; CreateSwapchainKHR checks the format it makes)
    if (linear_storage(physical, VK_FORMAT_R8G8B8A8_UNORM)) caps->supportedUsageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical,
                                                                        const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                                        VkSurfaceCapabilities2KHR* caps) {
    return GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info->surface, &caps->surfaceCapabilities);
}

// VideoOut A8B8G8R8 = R8G8B8A8 in memory (verified by DolphinPS4), A8R8G8B8 = B8G8R8A8. Both: DXVK takes the first,
// RT64 (plume) and recompui only make B8G8R8A8 swapchains and find no format otherwise (Conker).
const VkSurfaceFormatKHR kFormats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                  VkSurfaceFormatKHR* formats) {
    return fill(kFormats, count, formats);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice,
                                                                   const VkPhysicalDeviceSurfaceInfo2KHR*,
                                                                   uint32_t* count, VkSurfaceFormat2KHR* formats) {
    if (!formats) {
        *count = uint32_t(std::size(kFormats));
        return VK_SUCCESS;
    }
    uint32_t n = std::min<uint32_t>(*count, uint32_t(std::size(kFormats)));
    for (uint32_t k = 0; k < n; k++) formats[k].surfaceFormat = kFormats[k];  // (sType/pNext are the caller's)
    *count = n;
    return n < std::size(kFormats) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                       VkPresentModeKHR* modes) {
    const VkPresentModeKHR supported[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                          VK_PRESENT_MODE_IMMEDIATE_KHR};
    return fill(supported, count, modes);
}

// (VK_KHR_swapchain 70 with Vulkan 1.1 has these too; RADV's would be null)
VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                     VkRect2D* rects) {
    const VkRect2D whole[] = {{{0, 0}, {kScreenWidth, kScreenHeight}}};
    return fill(whole, count, rects);
}

VKAPI_ATTR VkResult VKAPI_CALL GetDeviceGroupPresentCapabilitiesKHR(VkDevice,
                                                                    VkDeviceGroupPresentCapabilitiesKHR* caps) {
    std::memset(caps->presentMask, 0, sizeof caps->presentMask);
    caps->presentMask[0] = 1;
    caps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetDeviceGroupSurfacePresentModesKHR(VkDevice, VkSurfaceKHR,
                                                                    VkDeviceGroupPresentModeFlagsKHR* modes) {
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

// ---- device level ---------------------------------------------------------------------------------------------

uint32_t host_visible_type(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props;
    instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(g_physical, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t k = 0; k < props.memoryTypeCount; k++)
        if ((bits & (1u << k)) && (props.memoryTypes[k].propertyFlags & want) == want) return k;
    return UINT32_MAX;
}

void free_images(Swapchain* sc) {
    if (sc->device) {  // (a parked swapchain whose device is gone has nothing left to free: the device took it)
        auto destroy_image = device_fn<PFN_vkDestroyImage>(sc->device, "vkDestroyImage");
        auto free_memory = device_fn<PFN_vkFreeMemory>(sc->device, "vkFreeMemory");
        for (uint32_t k = 0; k < sc->image_count; k++) {
            if (sc->images[k]) destroy_image(sc->device, sc->images[k], nullptr);
            if (sc->memory[k]) free_memory(sc->device, sc->memory[k], nullptr);
        }
    }
    delete sc;
}

// Gives the swapchain's buffers back to VideoOut, then frees it; parks it if VideoOut won't let go yet. (g_lock held)
void free_swapchain(Swapchain* sc) {
    if (sc->buffer_set >= 0) {
        const int r = sceVideoOutUnregisterBuffers(g_video, sc->buffer_set);
        if (r < 0) {
            log("ps4kit/wsi: buffers %d+ still in use (%x), released after the next flips", sc->first_buffer,
                unsigned(r));
            g_retired.push_back(sc);
            g_retired_count = g_retired.size();
            return;
        }
        g_bank_used[sc->first_buffer / int(kMaxImages)] = false;
        sc->buffer_set = -1;
    }
    free_images(sc);
}

// Frees the parked swapchains VideoOut has let go of since. (g_lock held)
void release_retired() {
    for (size_t k = 0; k < g_retired.size();) {
        Swapchain* sc = g_retired[k];
        if (sceVideoOutUnregisterBuffers(g_video, sc->buffer_set) < 0) {
            k++;
            continue;
        }
        log("ps4kit/wsi: buffers %d+ released", sc->first_buffer);
        g_bank_used[sc->first_buffer / int(kMaxImages)] = false;
        sc->buffer_set = -1;
        g_retired.erase(g_retired.begin() + ptrdiff_t(k));
        free_images(sc);
    }
    g_retired_count = g_retired.size();
}

// Window mode's memory: host-visible, cached for the CPU's reads where there is such a type (the frames are read by the
// CPU at each present), else the device-local host-visible one.
uint32_t readback_type(uint32_t bits, bool* coherent) {
    VkPhysicalDeviceMemoryProperties props;
    instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(g_physical, &props);
    const VkMemoryPropertyFlags tries[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    for (const VkMemoryPropertyFlags want : tries)
        for (uint32_t k = 0; k < props.memoryTypeCount; k++)
            if ((bits & (1u << k)) && (props.memoryTypes[k].propertyFlags & want) == want) {
                *coherent = (props.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return k;
            }
    return UINT32_MAX;
}

// A window mode swapchain (g_lock held): images of the size asked for, nothing of VideoOut's.
VkResult create_window_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info, VkSwapchainKHR* swapchain) {
    auto sc = new Swapchain();
    sc->device = device;
    sc->window = true;
    sc->surface = uint64_t(info->surface);
    sc->image_count = std::clamp<uint32_t>(info->minImageCount, 2, kMaxImages);
    sc->width = std::clamp<uint32_t>(info->imageExtent.width, 1, 4096);
    sc->height = std::clamp<uint32_t>(info->imageExtent.height, 1, 4096);
    const bool bgra = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || info->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
    sc->format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if (info->imageFormat != sc->format)
        log("ps4kit/wsi: format %d asked for, made %d", int(info->imageFormat), int(sc->format));
    auto create_image = device_fn<PFN_vkCreateImage>(device, "vkCreateImage");
    auto requirements = device_fn<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements");
    auto allocate = device_fn<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
    auto map = device_fn<PFN_vkMapMemory>(device, "vkMapMemory");
    auto bind = device_fn<PFN_vkBindImageMemory>(device, "vkBindImageMemory");
    auto layout_of = device_fn<PFN_vkGetImageSubresourceLayout>(device, "vkGetImageSubresourceLayout");
    VkImageUsageFlags usage = info->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !linear_storage(g_physical, sc->format))
        usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
    for (uint32_t k = 0; k < sc->image_count; k++) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = sc->format;
        ici.extent = {sc->width, sc->height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        ici.usage = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = create_image(device, &ici, nullptr, &sc->images[k]);
        if (r != VK_SUCCESS) {
            log("ps4kit/wsi: window image %ux%u: vkCreateImage failed: %d", sc->width, sc->height, int(r));
            free_swapchain(sc);
            return r;
        }
        VkMemoryRequirements req;
        requirements(device, sc->images[k], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = readback_type(req.memoryTypeBits, &sc->coherent);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            log("ps4kit/wsi: no host-visible memory type for a window swapchain (types %#x)", req.memoryTypeBits);
            free_swapchain(sc);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        uint8_t* base = nullptr;
        r = allocate(device, &mai, nullptr, &sc->memory[k]);
        if (r == VK_SUCCESS) r = map(device, sc->memory[k], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&base));
        if (r == VK_SUCCESS) r = bind(device, sc->images[k], sc->memory[k], 0);
        if (r != VK_SUCCESS) {
            log("ps4kit/wsi: window swapchain memory failed: %d", int(r));
            free_swapchain(sc);
            return r;
        }
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        layout_of(device, sc->images[k], &sub, &layout);
        sc->pixels[k] = base + layout.offset;
        sc->row_bytes = uint32_t(layout.rowPitch);
    }
    log("ps4kit/wsi: window swapchain of %u images %ux%u %s, rows of %u bytes, %s memory", sc->image_count, sc->width,
        sc->height, bgra ? "B8G8R8A8" : "R8G8B8A8", sc->row_bytes, sc->coherent ? "coherent" : "cached");
    *swapchain = reinterpret_cast<VkSwapchainKHR>(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                  const VkAllocationCallbacks*, VkSwapchainKHR* swapchain) {
    step("wsi: CreateSwapchainKHR %u images %ux%u format %d mode %d usage %#x", info->minImageCount,
         info->imageExtent.width, info->imageExtent.height, int(info->imageFormat), int(info->presentMode),
         unsigned(info->imageUsage));
    std::lock_guard lk(g_lock);
    if (g_window_present.load()) return create_window_swapchain(device, info, swapchain);
    if (!open_video_out()) return VK_ERROR_INITIALIZATION_FAILED;
    auto sc = new Swapchain();
    sc->device = device;
    sc->image_count = std::clamp<uint32_t>(info->minImageCount, 2, kMaxImages);
    // IMMEDIATE flips at the next scanline (tearing); everything else at the next vblank. MAILBOX too: VideoOut can't
    // replace a flip once queued, so a mailbox can only be FIFO here.
    sc->flip_mode =
        info->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? ORBIS_VIDEO_OUT_FLIP_HSYNC : ORBIS_VIDEO_OUT_FLIP_VSYNC;
    const bool bgra = info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || info->imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
    sc->format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    if (info->imageFormat != sc->format)
        log("ps4kit/wsi: format %d asked for, made %d", int(info->imageFormat), int(sc->format));
    auto create_image = device_fn<PFN_vkCreateImage>(device, "vkCreateImage");
    auto requirements = device_fn<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements");
    auto allocate = device_fn<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
    auto map = device_fn<PFN_vkMapMemory>(device, "vkMapMemory");
    auto bind = device_fn<PFN_vkBindImageMemory>(device, "vkBindImageMemory");
    auto layout_of = device_fn<PFN_vkGetImageSubresourceLayout>(device, "vkGetImageSubresourceLayout");
    VkImageUsageFlags usage = info->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Storage only where linear images of the format can be: asked of a format that can't, image creation is invalid
    // usage, not an error code to fall back on. (FSR, which needs it, then can't run: said once in the log.)
    if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !linear_storage(g_physical, sc->format)) {
        log("ps4kit/wsi: no linear storage images of format %d: storage usage dropped", int(sc->format));
        usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
    }
    void* addresses[kMaxImages]{};
    uint32_t pitch = kScreenWidth;
    for (uint32_t k = 0; k < sc->image_count; k++) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = sc->format;
        ici.extent = {kScreenWidth, kScreenHeight, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        ici.usage = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        step("wsi: image %u: vkCreateImage", k);
        VkResult r = create_image(device, &ici, nullptr, &sc->images[k]);
        if (r != VK_SUCCESS && (ici.usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            log("ps4kit/wsi: no storage swapchain images (%d), made without", int(r));
            usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
            ici.usage = usage;
            r = create_image(device, &ici, nullptr, &sc->images[k]);
        }
        step("wsi: image %u: vkCreateImage = %d", k, int(r));
        if (r != VK_SUCCESS) {
            log("ps4kit/wsi: vkCreateImage failed: %d", int(r));
            free_swapchain(sc);
            return r;
        }
        VkMemoryRequirements req;
        requirements(device, sc->images[k], &req);
        // VideoOut wants 2 MiB aligned buffers: over-allocate, bind at an aligned offset
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size + kFramebufferAlign;
        mai.memoryTypeIndex = host_visible_type(req.memoryTypeBits);
        step("wsi: image %u: %llu bytes, align %llu, types %#x -> type %u", k, (unsigned long long)req.size,
             (unsigned long long)req.alignment, req.memoryTypeBits, mai.memoryTypeIndex);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            log("ps4kit/wsi: no device-local host-visible memory type for the swapchain (types %#x)",
                req.memoryTypeBits);
            free_swapchain(sc);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        uint8_t* base = nullptr;
        r = allocate(device, &mai, nullptr, &sc->memory[k]);
        if (r == VK_SUCCESS) r = map(device, sc->memory[k], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&base));
        step("wsi: image %u: memory %d at %p", k, int(r), static_cast<void*>(base));
        if (r != VK_SUCCESS) {
            log("ps4kit/wsi: swapchain memory failed: %d", int(r));
            free_swapchain(sc);
            return r;
        }
        uintptr_t at = reinterpret_cast<uintptr_t>(base);
        VkDeviceSize offset = ((at + kFramebufferAlign - 1) & ~(kFramebufferAlign - 1)) - at;
        offset = (offset + req.alignment - 1) & ~(req.alignment - 1);
        bind(device, sc->images[k], sc->memory[k], offset);
        VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        layout_of(device, sc->images[k], &sub, &layout);
        addresses[k] = base + offset + layout.offset;
        pitch = uint32_t(layout.rowPitch / 4);
        sc->pitch = pitch;
    }
    OrbisVideoOutBufferAttribute attribute;
    const uint32_t pixel_format = bgra ? kPixelFormatA8R8G8B8Srgb : ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB;
    sceVideoOutSetBufferAttribute(&attribute, pixel_format, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kScreenWidth,
                                  kScreenHeight, pitch);
    // a free bank (the old swapchain is usually still on screen while this one registers)
    release_retired();
    int registered = -1;
    for (int bank = 0; bank < kBanks && sc->buffer_set < 0; bank++) {
        if (g_bank_used[bank]) continue;
        sc->first_buffer = bank * int(kMaxImages);
        step("wsi: sceVideoOutRegisterBuffers %u buffers from %d, pitch %u", sc->image_count, sc->first_buffer, pitch);
        registered = sceVideoOutRegisterBuffers(g_video, sc->first_buffer, addresses, int(sc->image_count), &attribute);
        step("wsi: sceVideoOutRegisterBuffers = %x", unsigned(registered));
        if (registered >= 0) {
            sc->buffer_set = registered;
            g_bank_used[bank] = true;
        }
    }
    if (sc->buffer_set < 0) {
        log("ps4kit/wsi: sceVideoOutRegisterBuffers failed: %x (%zu swapchains parked)", unsigned(registered),
            g_retired.size());
        free_swapchain(sc);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    log("ps4kit/wsi: swapchain of %u images %ux%u %s pitch %u, buffers %d+, %s flips", sc->image_count, kScreenWidth,
        kScreenHeight, bgra ? "B8G8R8A8" : "R8G8B8A8", pitch, sc->first_buffer,
        sc->flip_mode == ORBIS_VIDEO_OUT_FLIP_VSYNC ? "vsync" : "hsync");
    *swapchain = reinterpret_cast<VkSwapchainKHR>(sc);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks*) {
    if (!swapchain) return;
    std::lock_guard lk(g_lock);
    device_fn<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle")(device);
    free_swapchain(reinterpret_cast<Swapchain*>(swapchain));
}

// A parked swapchain must not outlive its device with Vulkan handles in it: release_retired() would free them
// through a destroyed device at the next flip of a new one (a renderer switch, a device lost and recreated). Not seen
// on hardware yet; follows from the parking. What VideoOut still holds stays parked without its images (the device
// takes them) until it lets go of the bank.
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    if (device) {
        std::lock_guard lk(g_lock);
        release_retired();
        for (Swapchain* sc : g_retired)
            if (sc->device == device) {
                log("ps4kit/wsi: device destroyed while VideoOut holds buffers %d+", sc->first_buffer);
                sc->device = VK_NULL_HANDLE;
            }
    }
    device_fn<PFN_vkDestroyDevice>(device, "vkDestroyDevice")(device, alloc);
    if (device == g_device) {
        g_device = VK_NULL_HANDLE;
        g_queue_submit = nullptr;
        g_queue_submit2 = nullptr;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t* count,
                                                     VkImage* images) {
    auto sc = reinterpret_cast<Swapchain*>(swapchain);
    if (!images) {
        *count = sc->image_count;
        return VK_SUCCESS;
    }
    uint32_t n = std::min(*count, sc->image_count);
    std::memcpy(images, sc->images, n * sizeof(VkImage));
    *count = n;
    return n < sc->image_count ? VK_INCOMPLETE : VK_SUCCESS;
}

// Present statistics: a 10 s window, logged at its end and kept for present_stats().
struct {
    std::mutex lock;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t flips = 0;
    uint64_t last_present_ns = 0, longest_ns = 0;
    PresentStats last;
} g_stats;
std::atomic<uint64_t> g_wait_ns{0};
std::atomic<uint32_t> g_timeouts{0};
std::atomic<int64_t> g_last_arg{0};

constexpr int kWaitTries = 40;
constexpr OrbisKernelUseconds kWaitSlice = 5000;  // 40 x 5 ms: 200 ms at most, then the image is used anyway

// Whether flip `needed` has been made. Its flipArg is its flip number.
bool flip_made(int64_t needed) {
    OrbisVideoOutFlipStatus status{};
    if (sceVideoOutGetFlipStatus(g_video, &status) < 0) return true;  // (nothing to wait with)
    g_last_arg = status.flipArg;
    return status.flipArg >= needed;
}

// The image about to be rendered into must be off screen: it is once a later flip than the one that showed it has
// been made. Each image keeps the number of the flip that showed it; SMW's formula (flip n-N+2, n = flips submitted,
// N images) is the same in steady state, but also waited for old flips with a brand-new swapchain's never-shown
// images, and for a whole 200 ms when an image was acquired again without a present in between. The wait sleeps on
// VideoOut's flip event queue between status checks (a flip wakes it at once), and gives up after 200 ms rather than
// freeze when a flip never comes: tearing for a frame beats a hang.
// Returns false (and doesn't wait) for a zero timeout with the image still on screen.
bool wait_off_screen(Swapchain* sc, uint32_t image, uint64_t timeout) {
    const int64_t shown = sc->shown_by[image].load();
    if (shown == 0) return true;  // never shown
    const int64_t needed = shown + 1;
    if (uint64_t(needed) > g_flips.load()) {
        // nothing after it was submitted: waiting can't help (the app acquired more images than it presented)
        return true;
    }
    if (flip_made(needed)) return true;
    if (timeout == 0) return false;
    const uint64_t t0 = now_ns();
    bool done = false;
    for (int tries = 0; tries < kWaitTries && !done; tries++) {
        if (!g_flip_queue) {
            sceKernelUsleep(1000);
        } else {
            OrbisKernelEvent ev;
            int got = 0;
            OrbisKernelUseconds slice = kWaitSlice;
            sceKernelWaitEqueue(g_flip_queue, &ev, 1, &got, &slice);
        }
        done = flip_made(needed);
    }
    if (!done) g_timeouts++;
    g_wait_ns += now_ns() - t0;
    return true;
}

void count_present() {
    std::lock_guard lk(g_stats.lock);
    const uint64_t t = now_ns();
    g_last_present_ns.store(t, std::memory_order_relaxed);
    if (g_stats.last_present_ns) g_stats.longest_ns = std::max(g_stats.longest_ns, t - g_stats.last_present_ns);
    g_stats.last_present_ns = t;
    g_stats.flips++;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - g_stats.start).count();
    if (secs < 10.0) return;
    PresentStats s;
    s.flips_per_second = double(g_stats.flips) / secs;
    s.acquire_wait_ms_per_second = double(g_wait_ns.exchange(0)) / 1e6 / secs;
    s.longest_frame_ms = double(g_stats.longest_ns) / 1e6;
    s.acquire_timeouts = g_timeouts.exchange(0);
    g_stats.last = s;
    if (g_stats_log.load())
        log("ps4kit/wsi: %.1f flips/s, longest frame %.1f ms, acquire waits %.0f ms/s, %u timeouts, flipArg %lld of "
            "%llu submitted",
            s.flips_per_second, s.longest_frame_ms, s.acquire_wait_ms_per_second, s.acquire_timeouts,
            (long long)g_last_arg.load(), (unsigned long long)g_flips.load());
    g_stats.start = now;
    g_stats.flips = 0;
    g_stats.longest_ns = 0;
}

VkQueue graphics_queue(VkDevice device) {
    VkQueue q = VK_NULL_HANDLE;
    device_fn<PFN_vkGetDeviceQueue>(device, "vkGetDeviceQueue")(device, 0, 0, &q);
    return q;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore, VkFence fence, uint32_t* index) {
    auto sc = reinterpret_cast<Swapchain*>(swapchain);
    step("wsi: AcquireNextImageKHR (fence %d)", fence ? 1 : 0);
    const uint32_t image = sc->next_image;
    // (a zero timeout asks not to block: the spec's VK_NOT_READY, the image stays next)
    if (!wait_off_screen(sc, image, timeout)) return VK_NOT_READY;
    *index = image;
    sc->next_image = (image + 1) % sc->image_count;
    step("wsi: acquired image %u", image);
    // The image is free now. The semaphore is never signalled (see the top of the file); a fence, if asked for, is
    // signalled by an empty submit.
    if (fence) {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkResult r = device_fn<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(graphics_queue(device), 1, &si, fence);
        if (r != VK_SUCCESS) log("ps4kit/wsi: acquire fence submit failed: %d", int(r));
        return r;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                    uint32_t* index) {
    return AcquireNextImageKHR(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    VkResult result = VK_SUCCESS;
    bool waited = false;
    for (uint32_t k = 0; k < info->swapchainCount; k++) {
        auto sc = reinterpret_cast<Swapchain*>(info->pSwapchains[k]);
        const uint32_t image = info->pImageIndices[k];
        if (image >= sc->image_count) {
            log("ps4kit/wsi: present of image %u of %u", image, sc->image_count);
            if (info->pResults) info->pResults[k] = VK_ERROR_OUT_OF_DATE_KHR;
            result = VK_ERROR_OUT_OF_DATE_KHR;
            continue;
        }
        if (sc->window) {  // the GPU's frame, once drawn, to the hook (its pixels stay until acquired again)
            if (!waited) device_fn<PFN_vkQueueWaitIdle>(sc->device, "vkQueueWaitIdle")(queue);
            waited = true;
            if (!sc->coherent) {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = sc->memory[image];
                range.size = VK_WHOLE_SIZE;
                device_fn<PFN_vkInvalidateMappedMemoryRanges>(sc->device, "vkInvalidateMappedMemoryRanges")(sc->device,
                                                                                                          1, &range);
            }
            if (auto hook = g_window_present.load())
                hook(sc->surface, sc->pixels[image], sc->width, sc->height, sc->row_bytes,
                     sc->format == VK_FORMAT_B8G8R8A8_UNORM);
            count_present();
            if (info->pResults) info->pResults[k] = VK_SUCCESS;
            continue;
        }
        const uint32_t buffer = uint32_t(sc->first_buffer) + image;
        const uint32_t mode = g_force_vsync.load() ? uint32_t(ORBIS_VIDEO_OUT_FLIP_VSYNC) : sc->flip_mode;
        int r;
        bool gpu_flip;
        uint64_t n = 0;
        {
            std::lock_guard lk(g_present_lock);
            const int64_t arg = int64_t(g_flips.load() + 1);
            // the flip goes to the GPU behind the frame's commands: no CPU wait for the GPU or the vblank
            step("wsi: QueuePresentKHR image %u -> buffer %u, flip %lld", image, buffer, (long long)arg);
            r = ac_ps4_submit_flip ? ac_ps4_submit_flip(g_video, buffer, mode, arg) : -1;
            step("wsi: ac_ps4_submit_flip = %x", unsigned(r));
            gpu_flip = r == 0;
            if (!gpu_flip) {  // fallback: wait for the GPU on the CPU, then flip
                device_fn<PFN_vkQueueWaitIdle>(sc->device, "vkQueueWaitIdle")(queue);
                r = sceVideoOutSubmitFlip(g_video, int(buffer), mode, arg);
                step("wsi: sceVideoOutSubmitFlip = %x", unsigned(r));
            }
            if (r >= 0) {
                sc->shown_by[image] = arg;
                n = ++g_flips;
            }
        }
        if (r < 0) {
            log("ps4kit/wsi: flip failed: %x", unsigned(r));
            result = VK_ERROR_SURFACE_LOST_KHR;
        } else {
            count_present();
            // parked swapchains VideoOut may have let go of now
            if (g_retired_count.load(std::memory_order_relaxed)) {
                std::lock_guard lk(g_lock);
                release_retired();
            }
            // The system keeps its boot splash (the app's picture) over everything until the app says it has a
            // picture of its own.
            if (n == 1) sceSystemServiceHideSplashScreen();
            if (n <= 3)
                log("ps4kit/wsi: flip %llu of buffer %u (%s)", (unsigned long long)n, buffer,
                    gpu_flip ? "GPU-side" : "CPU wait");
        }
        if (info->pResults) info->pResults[k] = r < 0 ? VK_ERROR_SURFACE_LOST_KHR : VK_SUCCESS;
    }
    return result;
}

// ---- semaphores: binary ones are dropped from submits, timeline ones kept -------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* info,
                                               const VkAllocationCallbacks* alloc, VkSemaphore* semaphore) {
    VkResult r = device_fn<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore")(device, info, alloc, semaphore);
    if (r != VK_SUCCESS) return r;
    for (auto p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO &&
            reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(p)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
            std::unique_lock lk(g_timeline_lock);
            g_timelines.insert(*semaphore);
        }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice device, VkSemaphore semaphore,
                                            const VkAllocationCallbacks* alloc) {
    {
        std::unique_lock lk(g_timeline_lock);
        g_timelines.erase(semaphore);
    }
    device_fn<PFN_vkDestroySemaphore>(device, "vkDestroySemaphore")(device, semaphore, alloc);
}

bool is_timeline(VkSemaphore s) {
    std::shared_lock lk(g_timeline_lock);
    return g_timelines.count(s) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    PFN_vkQueueSubmit real = g_queue_submit;
    // (most submits carry no semaphores at all: straight through, without copies)
    bool any = false;
    for (uint32_t k = 0; k < count && !any; k++)
        any = submits[k].waitSemaphoreCount || submits[k].signalSemaphoreCount;
    if (!any) return real(queue, count, submits, fence);
    std::vector<VkSubmitInfo> copy(submits, submits + count);
    std::vector<std::vector<VkSemaphore>> sems(count * 2);
    std::vector<std::vector<VkPipelineStageFlags>> stages(count);
    std::vector<std::vector<uint64_t>> values(count * 2);
    std::vector<VkTimelineSemaphoreSubmitInfo> tl(count);
    for (uint32_t k = 0; k < count; k++) {
        const VkTimelineSemaphoreSubmitInfo* t = nullptr;
        for (auto p = static_cast<const VkBaseInStructure*>(submits[k].pNext); p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                t = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);
        for (uint32_t w = 0; w < submits[k].waitSemaphoreCount; w++)
            if (is_timeline(submits[k].pWaitSemaphores[w])) {
                sems[2 * k].push_back(submits[k].pWaitSemaphores[w]);
                stages[k].push_back(submits[k].pWaitDstStageMask[w]);
                values[2 * k].push_back(t && w < t->waitSemaphoreValueCount ? t->pWaitSemaphoreValues[w] : 0);
            }
        for (uint32_t s = 0; s < submits[k].signalSemaphoreCount; s++)
            if (is_timeline(submits[k].pSignalSemaphores[s])) {
                sems[2 * k + 1].push_back(submits[k].pSignalSemaphores[s]);
                values[2 * k + 1].push_back(t && s < t->signalSemaphoreValueCount ? t->pSignalSemaphoreValues[s] : 0);
            }
        copy[k].waitSemaphoreCount = uint32_t(sems[2 * k].size());
        copy[k].pWaitSemaphores = sems[2 * k].data();
        copy[k].pWaitDstStageMask = stages[k].data();
        copy[k].signalSemaphoreCount = uint32_t(sems[2 * k + 1].size());
        copy[k].pSignalSemaphores = sems[2 * k + 1].data();
        if (t) {  // (the struct is const in the chain: rebuilt with the kept values, chained alone in its place)
            tl[k] = *t;
            tl[k].waitSemaphoreValueCount = uint32_t(values[2 * k].size());
            tl[k].pWaitSemaphoreValues = values[2 * k].data();
            tl[k].signalSemaphoreValueCount = uint32_t(values[2 * k + 1].size());
            tl[k].pSignalSemaphoreValues = values[2 * k + 1].data();
            tl[k].pNext = nullptr;
            copy[k].pNext = &tl[k];
        }
        step("wsi: QueueSubmit [%u] %u command buffers, waits %u of %u kept, signals %u of %u kept, fence %d", k,
             submits[k].commandBufferCount, copy[k].waitSemaphoreCount, submits[k].waitSemaphoreCount,
             copy[k].signalSemaphoreCount, submits[k].signalSemaphoreCount, fence ? 1 : 0);
    }
    VkResult r = real(queue, count, copy.data(), fence);
    step("wsi: QueueSubmit = %d", int(r));
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                            VkFence fence) {
    PFN_vkQueueSubmit2 real = g_queue_submit2;
    bool any = false;
    for (uint32_t k = 0; k < count && !any; k++)
        any = submits[k].waitSemaphoreInfoCount || submits[k].signalSemaphoreInfoCount;
    if (!any) return real(queue, count, submits, fence);
    std::vector<VkSubmitInfo2> copy(submits, submits + count);
    std::vector<std::vector<VkSemaphoreSubmitInfo>> sems(count * 2);
    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t w = 0; w < submits[k].waitSemaphoreInfoCount; w++)
            if (is_timeline(submits[k].pWaitSemaphoreInfos[w].semaphore))
                sems[2 * k].push_back(submits[k].pWaitSemaphoreInfos[w]);
        for (uint32_t s = 0; s < submits[k].signalSemaphoreInfoCount; s++)
            if (is_timeline(submits[k].pSignalSemaphoreInfos[s].semaphore))
                sems[2 * k + 1].push_back(submits[k].pSignalSemaphoreInfos[s]);
        copy[k].waitSemaphoreInfoCount = uint32_t(sems[2 * k].size());
        copy[k].pWaitSemaphoreInfos = sems[2 * k].data();
        copy[k].signalSemaphoreInfoCount = uint32_t(sems[2 * k + 1].size());
        copy[k].pSignalSemaphoreInfos = sems[2 * k + 1].data();
        step("wsi: QueueSubmit2 [%u] %u command buffers, waits %u of %u kept, signals %u of %u kept, fence %d", k,
             submits[k].commandBufferInfoCount, copy[k].waitSemaphoreInfoCount, submits[k].waitSemaphoreInfoCount,
             copy[k].signalSemaphoreInfoCount, submits[k].signalSemaphoreInfoCount, fence ? 1 : 0);
    }
    VkResult r = real(queue, count, copy.data(), fence);
    step("wsi: QueueSubmit2 = %d", int(r));
    return r;
}

PFN_vkVoidFunction intercept(const char* name) {
#define PS4KIT_WSI_ENTRY(fn) \
    if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
    PS4KIT_WSI_ENTRY(EnumerateInstanceExtensionProperties);
    PS4KIT_WSI_ENTRY(EnumerateInstanceLayerProperties);
    PS4KIT_WSI_ENTRY(EnumerateDeviceLayerProperties);
    PS4KIT_WSI_ENTRY(CreateInstance);
    PS4KIT_WSI_ENTRY(EnumerateDeviceExtensionProperties);
    PS4KIT_WSI_ENTRY(CreateDevice);
    PS4KIT_WSI_ENTRY(DestroyDevice);
    PS4KIT_WSI_ENTRY(CreateHeadlessSurfaceEXT);
    PS4KIT_WSI_ENTRY(DestroySurfaceKHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfaceSupportKHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfaceCapabilities2KHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfaceFormatsKHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfaceFormats2KHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDeviceSurfacePresentModesKHR);
    PS4KIT_WSI_ENTRY(GetPhysicalDevicePresentRectanglesKHR);
    PS4KIT_WSI_ENTRY(GetDeviceGroupPresentCapabilitiesKHR);
    PS4KIT_WSI_ENTRY(GetDeviceGroupSurfacePresentModesKHR);
    PS4KIT_WSI_ENTRY(CreateSwapchainKHR);
    PS4KIT_WSI_ENTRY(DestroySwapchainKHR);
    PS4KIT_WSI_ENTRY(GetSwapchainImagesKHR);
    PS4KIT_WSI_ENTRY(AcquireNextImageKHR);
    PS4KIT_WSI_ENTRY(AcquireNextImage2KHR);
    PS4KIT_WSI_ENTRY(QueuePresentKHR);
    PS4KIT_WSI_ENTRY(CreateSemaphore);
    PS4KIT_WSI_ENTRY(DestroySemaphore);
    PS4KIT_WSI_ENTRY(QueueSubmit);
#undef PS4KIT_WSI_ENTRY
    // Only where RADV has them for the device: an app that checks for null to see whether synchronization2 is there
    // must still see null when it isn't.
    if (std::strcmp(name, "vkQueueSubmit2") == 0 || std::strcmp(name, "vkQueueSubmit2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
    // A loader that looks vkGetDeviceProcAddr up again through itself (vulkan.hpp's
    // DispatchLoaderDynamic::init(device)) must get this one: RADV's would resolve the swapchain functions, and it has
    // none (all null).
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    if (auto fn = intercept(name)) {
        if (fn == reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2) && !vk_common_GetDeviceProcAddr(device, name))
            return nullptr;
        return fn;
    }
    return vk_common_GetDeviceProcAddr(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    if (auto fn = intercept(name)) return fn;
    return vk_icdGetInstanceProcAddr(instance, name);
}

// ---- vblank events ----------------------------------------------------------------------------------------------

std::mutex g_vblank_lock;
OrbisKernelEqueue g_vblank_queue = 0;
int g_vblank_ready = 0;  // 0 not set up yet, 1 set up, -1 not available (or gave up after a timeout)
int g_vblanks_timed = 0;
std::chrono::steady_clock::time_point g_vblank_first;
std::atomic<double> g_vblank_rate{0};

}  // namespace

void* vk_get_instance_proc_addr() { return reinterpret_cast<void*>(&GetInstanceProcAddr); }

void set_window_present(WindowPresent hook) { g_window_present = hook; }

bool video_out_open() { return open_video_out(); }

int video_handle() { return open_video_out() ? g_video : -1; }

uint64_t flip_count() { return g_flips.load(); }

bool flip_info(FlipInfo* info) {
    *info = FlipInfo{};
    if (g_video < 0) return false;
    OrbisVideoOutFlipStatus status{};
    if (sceVideoOutGetFlipStatus(g_video, &status) < 0) return false;
    info->submitted = g_flips.load();
    info->shown = status.flipArg;
    info->flips_made = status.num;
    info->flip_time = status.ptime;
    info->gpu_pending = status.numGpuFlipPending;
    info->pending = status.numFlipPending;
    info->current_buffer = status.currentBuffer;
    info->last_present_ns = g_last_present_ns.load(std::memory_order_relaxed);
    return true;
}

// Polled, not on the flip event queue: an acquire may be sleeping on that queue in another thread, and an event one
// waiter takes is gone for the other.
bool flip_pending(unsigned grace_us) {
    const uint64_t submitted = g_flips.load();
    if (g_video < 0 || submitted == 0) return false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(grace_us);
    for (;;) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(g_video, &status) < 0) return false;
        if (status.flipArg >= int64_t(submitted)) return false;
        if (std::chrono::steady_clock::now() >= until) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// Its own event queue, for the same reason. HENkaku's and Conker's emulated vblanks follow these, which keeps a
// game's frames in their place against the vsynced flips (on their own clock they drifted through them: a frame a
// vblank late every 17 s or so).
bool wait_vblank(unsigned timeout_us) {
    std::lock_guard lk(g_vblank_lock);
    if (g_vblank_ready == 0) {
        const int video = video_handle();
        if (video < 0) return false;
        // (the OpenOrbis headers declare it without its parameters)
        using AddVblankEvent = int32_t (*)(OrbisKernelEqueue, int32_t, void*);
        int r = sceKernelCreateEqueue(&g_vblank_queue, "ps4kit vblank");
        if (r >= 0) r = reinterpret_cast<AddVblankEvent>(&sceVideoOutAddVblankEvent)(g_vblank_queue, video, nullptr);
        if (r < 0) {
            log("ps4kit/wsi: no vblank events (%x)", unsigned(r));
            g_vblank_ready = -1;
            return false;
        }
        g_vblank_ready = 1;
    }
    if (g_vblank_ready < 0) return false;
    OrbisKernelEvent ev;
    int got = 0;
    OrbisKernelUseconds timeout = timeout_us;
    const int r = sceKernelWaitEqueue(g_vblank_queue, &ev, 1, &got, &timeout);
    if (r >= 0 && got > 0) {
        if (g_vblanks_timed <= 120) {
            const auto now = std::chrono::steady_clock::now();
            if (g_vblanks_timed == 0) {
                g_vblank_first = now;
            } else if (g_vblanks_timed == 120) {
                const double rate = 120.0 / std::chrono::duration<double>(now - g_vblank_first).count();
                g_vblank_rate = rate;
                log("ps4kit/wsi: the display gives %.2f vblanks a second", rate);
            }
            g_vblanks_timed++;
        }
        return true;
    }
    if (r == kErrorTimedOut || (r >= 0 && got < 1)) {
        log("ps4kit/wsi: no vblank in %u us: wait_vblank answers false from now on", timeout_us);
        g_vblank_ready = -1;
    }
    return false;
}

double vblank_rate() { return g_vblank_rate.load(); }

PresentStats present_stats() {
    std::lock_guard lk(g_stats.lock);
    return g_stats.last;
}

void set_log(void (*hook)(const char* line)) { g_log_hook = hook; }

void set_step_trace(int lines) { g_max_steps = std::max(lines, 0); }

void set_stats_log(bool on) { g_stats_log = on; }

void set_force_vsync(bool on) { g_force_vsync = on; }

// Gives the screen back on the way out. A process that dies with its VideoOut buffers still
// registered can leave the display black until SceShellUI restarts; ps4_kit_exit calls this
// before it asks the shell to close the app.
void shutdown_video() {
    std::lock_guard lk(g_lock);
    if (g_video < 0) return;
    for (Swapchain* sc : g_retired) {
        if (sc->buffer_set >= 0 && sceVideoOutUnregisterBuffers(g_video, sc->buffer_set) >= 0)
            sc->buffer_set = -1;
    }
    int r = -1;
    for (int tries = 0; tries < 20 && r < 0; tries++) {
        r = sceVideoOutClose(g_video);
        if (r < 0) sceKernelUsleep(5000);
    }
    log("ps4kit/wsi: VideoOut closed on the way out (%x)", unsigned(r));
    g_video = -1;
}

}  // namespace ps4kit

// The runtime (ps4_kit_exit) calls this weakly, before the shell ends the process.
extern "C" void ps4kit_video_shutdown(void) { ps4kit::shutdown_video(); }

extern "C" void* ps4kit_vk_get_instance_proc_addr(void) { return ps4kit::vk_get_instance_proc_addr(); }

extern "C" void ps4kit_set_log(void (*log)(const char* line)) { ps4kit::set_log(log); }

extern "C" void ps4kit_set_window_present(void (*hook)(unsigned long long surface, const void* pixels, unsigned width,
                                                       unsigned height, unsigned row_bytes, int bgra)) {
    ps4kit::g_window_present_c = hook;
    ps4kit::WindowPresent present = nullptr;
    if (hook)
        present = [](uint64_t surface, const void* pixels, uint32_t width, uint32_t height, uint32_t row_bytes,
                     bool bgra) { ps4kit::g_window_present_c(surface, pixels, width, height, row_bytes, bgra ? 1 : 0); };
    ps4kit::set_window_present(present);
}

extern "C" int ps4kit_video_handle(void) { return ps4kit::video_handle(); }

extern "C" unsigned long long ps4kit_flip_count(void) { return ps4kit::flip_count(); }

// DXVK's src/vulkan/vulkan_loader.cpp on PS4 (SSBU's patches/dxvk-ps4.patch) looks this name up.
extern "C" __attribute__((weak)) PFN_vkGetInstanceProcAddr dxvk_ps4_vk_get_instance_proc_addr(void) {
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(ps4kit::vk_get_instance_proc_addr());
}
