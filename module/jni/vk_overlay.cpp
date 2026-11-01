#include "vk_overlay.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "il2cpp_dumper.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "shadowhook.h"
#include "ui.h"

namespace vkov {

struct SwapInfo {
    VkFormat format;
    VkExtent2D extent;
    VkImageUsageFlags usage;
    VkSurfaceTransformFlagBitsKHR transform;
};

static std::mutex g_mtx;
static std::map<VkSwapchainKHR, SwapInfo> g_swaps;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static uint32_t g_queue_family = UINT32_MAX;

static PFN_vkGetInstanceProcAddr orig_gipa = nullptr;
static PFN_vkGetDeviceProcAddr orig_gdpa = nullptr;
static PFN_vkQueuePresentKHR real_present = nullptr;
static PFN_vkCreateSwapchainKHR real_create_sc = nullptr;
static PFN_vkDestroySwapchainKHR real_destroy_sc = nullptr;
static PFN_vkCreateInstance real_create_inst = nullptr;
static PFN_vkCreateDevice real_create_dev = nullptr;

// ---- overlay resources (valid while g_ready) ----
static bool g_ctx_created = false, g_ready = false, g_failed = false;
static VkSwapchainKHR g_sc = VK_NULL_HANDLE;
static VkRenderPass g_rp = VK_NULL_HANDLE;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkExtent2D g_extent{};
static std::vector<VkImageView> g_views;
static std::vector<VkFramebuffer> g_fbs;
static std::vector<VkCommandBuffer> g_cbs;
static std::vector<VkFence> g_fences;
static std::vector<VkSemaphore> g_sems;
static uint64_t g_vk_present_count = 0;

static void teardown() {
    if (!g_ready && g_views.empty() && g_rp == VK_NULL_HANDLE) return;
    if (g_device) vkDeviceWaitIdle(g_device);
    if (g_ready) ImGui_ImplVulkan_Shutdown();
    for (auto f : g_fences) vkDestroyFence(g_device, f, nullptr);
    for (auto s : g_sems) vkDestroySemaphore(g_device, s, nullptr);
    for (auto f : g_fbs) vkDestroyFramebuffer(g_device, f, nullptr);
    for (auto v : g_views) vkDestroyImageView(g_device, v, nullptr);
    if (g_pool) vkDestroyCommandPool(g_device, g_pool, nullptr);
    if (g_rp) vkDestroyRenderPass(g_device, g_rp, nullptr);
    g_fences.clear(); g_sems.clear(); g_fbs.clear(); g_views.clear(); g_cbs.clear();
    g_pool = VK_NULL_HANDLE;
    g_rp = VK_NULL_HANDLE;
    g_sc = VK_NULL_HANDLE;
    g_ready = false;
}

static bool setup(VkQueue queue, VkSwapchainKHR sc, const SwapInfo &si) {
    if (!g_instance || !g_device) {
        dumper::log("vulkan: setup called without instance/device");
        return false;
    }
    if (!(si.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        dumper::log("vulkan: swapchain lacks COLOR_ATTACHMENT usage, overlay unsupported");
        g_failed = true;
        return false;
    }
    if (g_phys == VK_NULL_HANDLE) {
        g_phys = ImGui_ImplVulkanH_SelectPhysicalDevice(g_instance);
    }
    if (g_phys == VK_NULL_HANDLE) {
        dumper::log("vulkan: physical device not found");
        return false;
    }
    uint32_t family = (g_queue_family != UINT32_MAX)
                          ? g_queue_family
                          : ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_phys);
    if (family == UINT32_MAX) family = 0;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g_device, sc, &n, nullptr);
    if (n == 0) return false;
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(g_device, sc, &n, images.data());

    VkAttachmentDescription att{};
    att.format = si.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    if (vkCreateRenderPass(g_device, &rpi, nullptr, &g_rp) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = si.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        if (vkCreateImageView(g_device, &vi, nullptr, &v) != VK_SUCCESS) return false;
        g_views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_rp; fi.attachmentCount = 1; fi.pAttachments = &v;
        fi.width = si.extent.width; fi.height = si.extent.height; fi.layers = 1;
        VkFramebuffer fb;
        if (vkCreateFramebuffer(g_device, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        g_fbs.push_back(fb);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence f;
        vkCreateFence(g_device, &fci, nullptr, &f);
        g_fences.push_back(f);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(g_device, &sci, nullptr, &s);
        g_sems.push_back(s);
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (vkCreateCommandPool(g_device, &pci, nullptr, &g_pool) != VK_SUCCESS) return false;
    g_cbs.resize(n);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = n;
    if (vkAllocateCommandBuffers(g_device, &cai, g_cbs.data()) != VK_SUCCESS) return false;

    if (!g_ctx_created) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ui::init_style();
        g_ctx_created = true;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = g_instance;
    ii.PhysicalDevice = g_phys;
    ii.Device = g_device;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = n < 2 ? 2 : n;
    ii.ImageCount = n;
    ii.PipelineInfoMain.RenderPass = g_rp;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        dumper::log("vulkan: ImGui_ImplVulkan_Init failed");
        return false;
    }
    g_sc = sc;
    g_extent = si.extent;
    g_ready = true;
    dumper::log("vulkan overlay ready (%u images, queue family %u, size %ux%u)",
                n, family, g_extent.width, g_extent.height);
    return true;
}

// Renders the overlay; on success returns true and sets *wait to the semaphore present must wait on.
static bool render(VkQueue queue, const VkPresentInfoKHR *pi, VkSemaphore *wait) {
    if (g_failed || !g_device) return false;
    if (!ui::acquire_backend(ui::Backend::Vulkan)) return false;
    std::lock_guard<std::mutex> render_lock(ui::render_mutex());

    VkSwapchainKHR sc = pi->pSwapchains[0];
    SwapInfo si;
    {
        auto it = g_swaps.find(sc);
        if (it == g_swaps.end()) return false;
        si = it->second;
    }
    if (g_sc != sc) {
        teardown();
        if (!setup(queue, sc, si)) {
            if (!g_failed) {
                teardown();
                g_failed = true;
                dumper::log("vulkan: overlay setup failed");
            }
            return false;
        }
    }
    uint32_t idx = pi->pImageIndices[0];
    if (idx >= g_cbs.size()) return false;

    g_vk_present_count++;
    if (g_vk_present_count % 300 == 1) {
        dumper::log("vulkan present heartbeat: frame %llu (%ux%u)",
                    (unsigned long long)g_vk_present_count, g_extent.width, g_extent.height);
    }

    vkWaitForFences(g_device, 1, &g_fences[idx], VK_TRUE, UINT64_MAX);
    vkResetFences(g_device, 1, &g_fences[idx]);
    VkCommandBuffer cb = g_cbs[idx];
    vkResetCommandBuffer(cb, 0);

    ImGui_ImplVulkan_NewFrame();
    ui::begin_frame((int)g_extent.width, (int)g_extent.height);
    ImGui::NewFrame();
    ui::draw();
    ImGui::Render();

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_rp;
    rb.framebuffer = g_fbs[idx];
    rb.renderArea.extent = g_extent;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(pi->waitSemaphoreCount,
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = pi->waitSemaphoreCount;
    sub.pWaitSemaphores = pi->pWaitSemaphores;
    sub.pWaitDstStageMask = stages.data();
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &g_sems[idx];
    if (vkQueueSubmit(queue, 1, &sub, g_fences[idx]) != VK_SUCCESS) {
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        vkQueueSubmit(queue, 1, &empty, g_fences[idx]);
        return false;
    }
    *wait = g_sems[idx];
    return true;
}

// ----------------------------------------------------------------- wrappers
static thread_local bool t_in_present = false;

static VKAPI_ATTR VkResult VKAPI_CALL my_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    if (!real_present) return VK_ERROR_INITIALIZATION_FAILED;
    if (!t_in_present && pi && pi->swapchainCount == 1) {
        t_in_present = true;
        VkSemaphore wait = VK_NULL_HANDLE;
        bool ok = false;
        {
            std::lock_guard<std::mutex> l(g_mtx);
            ok = render(queue, pi, &wait);
        }
        VkResult r;
        if (ok) {
            VkPresentInfoKHR copy = *pi;
            copy.waitSemaphoreCount = 1;
            copy.pWaitSemaphores = &wait;
            r = real_present(queue, &copy);
        } else {
            r = real_present(queue, pi);
        }
        t_in_present = false;
        return r;
    }
    return real_present(queue, pi);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_create_sc(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                                                   const VkAllocationCallbacks *a,
                                                   VkSwapchainKHR *out) {
    if (!real_create_sc) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real_create_sc(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) {
        std::lock_guard<std::mutex> l(g_mtx);
        g_device = dev;
        g_swaps[*out] = {ci->imageFormat, ci->imageExtent, ci->imageUsage, ci->preTransform};
        dumper::log("vulkan: swapchain created %p (%ux%u)", *out, ci->imageExtent.width, ci->imageExtent.height);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_destroy_sc(VkDevice dev, VkSwapchainKHR sc,
                                                const VkAllocationCallbacks *a) {
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_sc == sc) teardown();
        g_swaps.erase(sc);
    }
    if (real_destroy_sc) real_destroy_sc(dev, sc, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_create_inst(const VkInstanceCreateInfo *ci,
                                                     const VkAllocationCallbacks *a,
                                                     VkInstance *out) {
    if (!real_create_inst) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real_create_inst(ci, a, out);
    if (r == VK_SUCCESS && out && *out) {
        g_instance = *out;
        dumper::log("vulkan: instance created %p", g_instance);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_create_dev(VkPhysicalDevice phys,
                                                    const VkDeviceCreateInfo *ci,
                                                    const VkAllocationCallbacks *a,
                                                    VkDevice *out) {
    if (!real_create_dev) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real_create_dev(phys, ci, a, out);
    if (r == VK_SUCCESS && out && *out) {
        g_device = *out;
        g_phys = phys;
        if (ci && ci->queueCreateInfoCount > 0 && ci->pQueueCreateInfos) {
            g_queue_family = ci->pQueueCreateInfos[0].queueFamilyIndex;
        }
        dumper::log("vulkan: device created %p (phys %p, qfamily %u)", g_device, g_phys, g_queue_family);
    }
    return r;
}

static PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction r) {
    if (!r || !name) return r;
    if (!strcmp(name, "vkQueuePresentKHR")) {
        if (!real_present) real_present = reinterpret_cast<PFN_vkQueuePresentKHR>(r);
        return reinterpret_cast<PFN_vkVoidFunction>(my_present);
    }
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        if (!real_create_sc) real_create_sc = reinterpret_cast<PFN_vkCreateSwapchainKHR>(r);
        return reinterpret_cast<PFN_vkVoidFunction>(my_create_sc);
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
        if (!real_destroy_sc) real_destroy_sc = reinterpret_cast<PFN_vkDestroySwapchainKHR>(r);
        return reinterpret_cast<PFN_vkVoidFunction>(my_destroy_sc);
    }
    return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hook_gipa(VkInstance inst, const char *name) {
    if (!orig_gipa) return nullptr;
    PFN_vkVoidFunction r = orig_gipa(inst, name);
    if (inst) g_instance = inst;
    return wrap(name, r);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hook_gdpa(VkDevice dev, const char *name) {
    if (!orig_gdpa) return nullptr;
    PFN_vkVoidFunction r = orig_gdpa(dev, name);
    if (dev) g_device = dev;
    return wrap(name, r);
}

void install() {
    std::thread([] {
        using namespace std::chrono_literals;
        for (int iter = 0; iter < 60; ++iter) {
            std::this_thread::sleep_for(iter == 0 ? 50ms : 500ms);
            if (real_present) break;

            // Use UNIQUE mode so functions work identically whether entered via inline hook
            // on libvulkan.so or via wrapped pointers returned by vkGet{Instance,Device}ProcAddr.
            shadowhook_hook_sym_name_2("libvulkan.so", "vkQueuePresentKHR",
                                       reinterpret_cast<void *>(my_present),
                                       reinterpret_cast<void **>(&real_present),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
            shadowhook_hook_sym_name_2("libvulkan.so", "vkCreateSwapchainKHR",
                                       reinterpret_cast<void *>(my_create_sc),
                                       reinterpret_cast<void **>(&real_create_sc),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
            shadowhook_hook_sym_name_2("libvulkan.so", "vkDestroySwapchainKHR",
                                       reinterpret_cast<void *>(my_destroy_sc),
                                       reinterpret_cast<void **>(&real_destroy_sc),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
            shadowhook_hook_sym_name_2("libvulkan.so", "vkCreateInstance",
                                       reinterpret_cast<void *>(my_create_inst),
                                       reinterpret_cast<void **>(&real_create_inst),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
            shadowhook_hook_sym_name_2("libvulkan.so", "vkCreateDevice",
                                       reinterpret_cast<void *>(my_create_dev),
                                       reinterpret_cast<void **>(&real_create_dev),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);

            shadowhook_hook_sym_name_2("libvulkan.so", "vkGetInstanceProcAddr",
                                       reinterpret_cast<void *>(hook_gipa),
                                       reinterpret_cast<void **>(&orig_gipa),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);
            shadowhook_hook_sym_name_2("libvulkan.so", "vkGetDeviceProcAddr",
                                       reinterpret_cast<void *>(hook_gdpa),
                                       reinterpret_cast<void **>(&orig_gdpa),
                                       SHADOWHOOK_HOOK_WITH_UNIQUE_MODE);

            if (real_present) {
                dumper::log("vulkan: successfully hooked vkQueuePresentKHR");
                break;
            }
        }
    }).detach();
}

}  // namespace vkov
