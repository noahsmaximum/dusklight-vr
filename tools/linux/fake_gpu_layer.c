// Dev-only Vulkan layer: reports CPU devices (llvmpipe) as integrated GPUs so Dusklight accepts
// them. WSL has no GPU Vulkan driver, and Aurora ignores CPU adapters; with this layer the Linux
// build of the mod can be exercised end to end on a Windows machine (slowly).
//
//   cc -shared -fPIC -O2 -o libfake_gpu_layer.so fake_gpu_layer.c
//   VK_ADD_LAYER_PATH=<dir with the json> VK_INSTANCE_LAYERS=VK_LAYER_DUSKLIGHT_fake_gpu
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <stdlib.h>
#include <string.h>

static PFN_vkGetInstanceProcAddr g_next = NULL;
static PFN_vkGetPhysicalDeviceProperties g_props = NULL;
static PFN_vkGetPhysicalDeviceProperties2 g_props2 = NULL;
static PFN_vkGetPhysicalDeviceProperties2 g_props2khr = NULL;

static void fix(VkPhysicalDeviceProperties* p) {
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    }
}

static VKAPI_ATTR void VKAPI_CALL props(VkPhysicalDevice d, VkPhysicalDeviceProperties* p) {
    g_props(d, p);
    fix(p);
}

static VKAPI_ATTR void VKAPI_CALL props2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
    g_props2(d, p);
    fix(&p->properties);
}

static VKAPI_ATTR void VKAPI_CALL props2khr(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
    g_props2khr(d, p);
    fix(&p->properties);
}

static VKAPI_ATTR VkResult VKAPI_CALL create_instance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc, VkInstance* out) {
    VkLayerInstanceCreateInfo* chain = (VkLayerInstanceCreateInfo*)info->pNext;
    while (chain != NULL &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
        chain = (VkLayerInstanceCreateInfo*)chain->pNext;
    }
    if (chain == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_next = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    PFN_vkCreateInstance next = (PFN_vkCreateInstance)g_next(NULL, "vkCreateInstance");
    VkResult r = next(info, alloc, out);
    if (r == VK_SUCCESS) {
        g_props = (PFN_vkGetPhysicalDeviceProperties)g_next(*out, "vkGetPhysicalDeviceProperties");
        g_props2 = (PFN_vkGetPhysicalDeviceProperties2)g_next(*out, "vkGetPhysicalDeviceProperties2");
        g_props2khr = (PFN_vkGetPhysicalDeviceProperties2)g_next(*out, "vkGetPhysicalDeviceProperties2KHR");
    }
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gpu_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (strcmp(name, "vkCreateInstance") == 0) {
        return (PFN_vkVoidFunction)create_instance;
    }
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return (PFN_vkVoidFunction)fake_gpu_GetInstanceProcAddr;
    }
    if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0 && g_props != NULL) {
        return (PFN_vkVoidFunction)props;
    }
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 && g_props2 != NULL) {
        return (PFN_vkVoidFunction)props2;
    }
    if (strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0 && g_props2khr != NULL) {
        return (PFN_vkVoidFunction)props2khr;
    }
    return g_next != NULL ? g_next(instance, name) : NULL;
}
