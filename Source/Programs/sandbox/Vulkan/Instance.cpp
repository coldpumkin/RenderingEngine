#include "Vulkan/Instance.h"

#include <cstring>
#include <vector>

// The instance, and the debug messenger with it
// ============================================================================
//
// One function makes both because **they live and die together**: the messenger cannot
// exist without the instance, and means nothing once the instance is gone.

#if LAMBDA_ENABLE_VULKAN_VALIDATION

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// The return value means "abort this Vulkan call". Anything but VK_FALSE is for
// testing the layer itself.
//
// The [Vulkan] prefix says the line is the layer's rather than ours, and that matters
// because **things we did not write arrive here too** -- Steam and OBS register
// implicit layers system-wide and their warnings come through this callback. Filtered
// at run time, never in code:
//   $env:VK_LOADER_LAYERS_DISABLE="~implicit~"
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) noexcept {

    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
                      : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARN"
                      : "INFO";
    LOG("[Vulkan %s] %s\n", level, data->pMessage != nullptr ? data->pMessage : "(no message)");
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeMessengerInfo() noexcept {
    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    // VERBOSE and INFO are left out: at this size they are noise around the two
    // severities that mean something is wrong.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

// vkEnumerateInstanceLayerProperties is one of the few functions volkInitialize fills
// in. It has to be callable without an instance, which is exactly what lets us ask
// what can be turned on before creating one.
bool HasInstanceLayer(const char* name) noexcept {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) { return false; }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) { return false; }
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) { return true; }
    }
    return false;
}

#endif // LAMBDA_ENABLE_VULKAN_VALIDATION

bool CreateInstance(VulkanInstance* out) noexcept {
    VulkanInstance& inst = *out;

    // volk finds vulkan-1.dll at run time with LoadLibrary, so a machine without it
    // reaches this line and gets a failure. Linked statically against Vulkan::Vulkan,
    // the process would not have started.
    if (volkInitialize() != VK_SUCCESS) {
        LOG("[vk] volkInitialize failed (vulkan-1.dll not found?)\n");
        return false;
    }

    // vkEnumerateInstanceVersion arrived in Vulkan 1.1, so a 1.0 loader leaves volk
    // with nothing to fill in -- **the null pointer is the answer**, not a failure to
    // ask the question.
    if (vkEnumerateInstanceVersion == nullptr) {
        LOG("[vk] Vulkan 1.0 loader; need 1.3\n");
        return false;
    }
    uint32_t loaderVersion = 0;
    vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < kRequiredApiVersion) {
        LOG("[vk] loader is %u.%u; need 1.3\n",
            VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "LambdaEngine";
    appInfo.apiVersion = kRequiredApiVersion;

    // The surface extensions are not optional: without a way onto a window this
    // program has nothing to do. VK_KHR_surface is the platform-independent half,
    // VK_KHR_win32_surface is how a surface is made from an HWND.
    std::vector<const char*> extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> layers;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &appInfo;

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerInfo();
    bool validationOn = false;
    if (HasInstanceLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // Chained into pNext, so **vkCreateInstance and vkDestroyInstance are covered
        // too**. A messenger created afterwards leaves those two unwatched.
        info.pNext = &messengerInfo;
        validationOn = true;
    } else {
        LOG("[vk] validation layer not available (Vulkan SDK installed?)\n");
    }
#endif

    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&info, nullptr, &inst.handle) != VK_SUCCESS) {
        LOG("[vk] vkCreateInstance failed\n");
        return false;
    }

    // **The instance level gets a table as well**, for the reason the device level
    // does: handle and functions from one place cannot be mixed.
    volkLoadInstanceTable(&inst.table, inst.handle);

    // **The globals have to be filled in too**, because volkLoadDeviceTable reaches
    // for the global vkGetDeviceProcAddr internally (volk.c:72-75, 224-228). That is
    // volk's, not ours to remove -- so using the instance table is **a convention, not
    // something enforced**: calling a global by mistake still works, quietly.
    //
    // Only volkLoadInstanceOnly, not volkLoadInstance: the latter fills device-level
    // pointers into the globals as well, and then calling through a global instead of
    // a device table works. With two devices that is a call on the wrong one, and it
    // is silent.
    volkLoadInstanceOnly(inst.handle);

#if LAMBDA_ENABLE_VULKAN_VALIDATION
    if (validationOn) {
        // **A failure here is not a failed instance** -- rendering works, the
        // messages are what is missing. It is still said out loud: working on without
        // knowing validation is off is the expensive part, and here validation is the
        // safety net rather than a convenience.
        if (inst.table.vkCreateDebugUtilsMessengerEXT(
                inst.handle, &messengerInfo, nullptr, &inst.messenger) != VK_SUCCESS) {
            LOG("[vk] could not create the debug messenger -- no validation output\n");
            inst.messenger = VK_NULL_HANDLE;
        }
    }
#endif

    LOG("[vk] instance created (Vulkan 1.3 requested, loader %u.%u)\n",
        VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
    return true;
}

// Destroys itself: it holds both the handle and the table it needs to do so.
VulkanInstance::~VulkanInstance() {
    if (handle == VK_NULL_HANDLE) { return; }   // empty is a legal state
    if (messenger != VK_NULL_HANDLE) {
        table.vkDestroyDebugUtilsMessengerEXT(handle, messenger, nullptr);
    }
    table.vkDestroyInstance(handle, nullptr);
}