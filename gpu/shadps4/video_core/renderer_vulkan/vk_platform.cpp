// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Include the vulkan platform specific header
#if defined(ANDROID)
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN64)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_dlss.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace Vulkan {

static const char* const VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
static const char* const CRASH_DIAGNOSTIC_LAYER_NAME = "VK_LAYER_LUNARG_crash_diagnostic";

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT type,
    const vk::DebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data) {

    spdlog::level level{};
    switch (severity) {
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
        level = spdlog::level::err;
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
        level = spdlog::level::info;
        break;
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
    case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
        level = spdlog::level::debug;
        break;
    default:
        level = spdlog::level::info;
    }

    LOG_GENERIC(Common::Log::Class::Render_Vulkan, level, "{}: {}",
                callback_data->pMessageIdName ? callback_data->pMessageIdName : "<null>",
                callback_data->pMessage ? callback_data->pMessage : "<null>");

    return VK_FALSE;
}

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window) {
    const auto& window_info = emu_window.GetWindowInfo();
    vk::SurfaceKHR surface{};

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (window_info.type == Frontend::WindowSystemType::Windows) {
        const vk::Win32SurfaceCreateInfoKHR win32_ci = {
            .hinstance = nullptr,
            .hwnd = static_cast<HWND>(window_info.render_surface),
        };

        if (instance.createWin32SurfaceKHR(&win32_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_CRITICAL(Render_Vulkan, "Failed to initialize Win32 surface");
            UNREACHABLE();
        }
    }
#elif defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
    if (window_info.type == Frontend::WindowSystemType::X11) {
        const vk::XlibSurfaceCreateInfoKHR xlib_ci = {
            .dpy = static_cast<Display*>(window_info.display_connection),
            .window = reinterpret_cast<Window>(window_info.render_surface),
        };

        if (instance.createXlibSurfaceKHR(&xlib_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to initialize Xlib surface");
            UNREACHABLE();
        }
    } else if (window_info.type == Frontend::WindowSystemType::Wayland) {
        if (EmulatorSettings.IsRenderdocEnabled()) {
            LOG_ERROR(Render_Vulkan,
                      "RenderDoc is not compatible with Wayland, use an X11 window instead.");
        }

        const vk::WaylandSurfaceCreateInfoKHR wayland_ci = {
            .display = static_cast<wl_display*>(window_info.display_connection),
            .surface = static_cast<wl_surface*>(window_info.render_surface),
        };

        if (instance.createWaylandSurfaceKHR(&wayland_ci, nullptr, &surface) !=
            vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to initialize Wayland surface");
            UNREACHABLE();
        }
    }
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    if (window_info.type == Frontend::WindowSystemType::Metal) {
        const vk::MetalSurfaceCreateInfoEXT macos_ci = {
            .pLayer = static_cast<const CAMetalLayer*>(window_info.render_surface),
        };

        if (instance.createMetalSurfaceEXT(&macos_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_CRITICAL(Render_Vulkan, "Failed to initialize MacOS surface");
            UNREACHABLE();
        }
    }
#endif

    if (!surface) {
        LOG_CRITICAL(Render_Vulkan, "Presentation not supported on this platform");
        UNREACHABLE();
    }

    return surface;
}

static auto GetLayerExtensions(std::vector<const char*>&& extensions,
                               const std::vector<const char*>& layers) {
    auto all_missing_vk_settings = true;

    for (const auto& layer_name : layers) {
        const auto [layer_properties_result, layer_extensions] =
            vk::enumerateInstanceExtensionProperties(std::string(layer_name));
        if (layer_properties_result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to query extension properties of {}: {}", layer_name,
                      vk::to_string(layer_properties_result));
        }
        auto found = false;
        for (const auto& extension : layer_extensions) {
            if (extension.extensionName == std::string_view(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
                found = true;
                all_missing_vk_settings = false;
                break;
            }
        }
        if (!found) {
            LOG_ERROR(Render_Vulkan, "Settings for layer {} not available.", layer_name);
        }
    }

    if (!all_missing_vk_settings) {
        extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    }

    return extensions;
}

std::vector<const char*> GetInstanceExtensions(Frontend::WindowSystemType window_type,
                                               bool enable_debug_utils) {
    const auto [properties_result, properties] = vk::enumerateInstanceExtensionProperties();
    if (properties_result != vk::Result::eSuccess || properties.empty()) {
        LOG_ERROR(Render_Vulkan, "Failed to query extension properties: {}",
                  vk::to_string(properties_result));
        return {};
    }

    // Add the windowing system specific extension
    std::vector<const char*> extensions;
    extensions.reserve(7);

    switch (window_type) {
    case Frontend::WindowSystemType::Headless:
        break;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case Frontend::WindowSystemType::Windows:
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        break;
#elif defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
    case Frontend::WindowSystemType::X11:
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        break;
    case Frontend::WindowSystemType::Wayland:
        extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
        break;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    case Frontend::WindowSystemType::Metal:
        extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        break;
#endif
    default:
        LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
        break;
    }

    if (window_type != Frontend::WindowSystemType::Headless) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }

    if (EmulatorSettings.IsHdrAllowed()) {
        extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    if (enable_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Sanitize extension list
    std::erase_if(extensions, [&](const char* extension) -> bool {
        const auto it =
            std::find_if(properties.begin(), properties.end(), [extension](const auto& prop) {
                return std::strcmp(extension, prop.extensionName) == 0;
            });

        if (it == properties.end()) {
            LOG_INFO(Render_Vulkan, "Candidate instance extension {} is not available", extension);
            return true;
        }
        return false;
    });

    return extensions;
}

std::vector<const char*> GetInstanceLayers(bool enable_validation, bool enable_crash_diagnostic) {
    const auto [properties_result, properties] = vk::enumerateInstanceLayerProperties();
    if (properties_result != vk::Result::eSuccess || properties.empty()) {
        LOG_ERROR(Render_Vulkan, "Failed to query layer properties: {}",
                  vk::to_string(properties_result));
        return {};
    }

    std::vector<const char*> layers;
    layers.reserve(2);

    if (enable_validation) {
        layers.push_back(VALIDATION_LAYER_NAME);
    }
    if (enable_crash_diagnostic) {
        layers.push_back(CRASH_DIAGNOSTIC_LAYER_NAME);
    }

    // Sanitize layer list
    std::erase_if(layers, [&](const char* layer) -> bool {
        const auto it = std::ranges::find_if(properties, [layer](const auto& prop) {
            return std::strcmp(layer, prop.layerName) == 0;
        });
        if (it == properties.end()) {
            LOG_ERROR(Render_Vulkan, "Requested layer {} is not available", layer);
            return true;
        }
        return false;
    });

    return layers;
}

namespace {
void SetProcessEnv(const char* name, const char* value) {
#ifdef _WIN32
    // The Vulkan loader reads the process environment (GetEnvironmentVariable); _putenv_s
    // updates it as well as the CRT's copy.
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

/// bbport: AMD's driver installs an implicit Vulkan layer, VK_LAYER_AMD_switchable_graphics, that
/// loads into every Vulkan program. It picks the GPU on AMD+AMD systems (an AMD iGPU with an AMD
/// discrete GPU), but it is loaded on AMD iGPU + NVIDIA laptops too and sits between the game and
/// the NVIDIA driver, where it is known to break Vulkan programs. So: the GPU the port will use
/// (the same rule as Instance's choice, or the configured index) is looked up first with the layer
/// off; the layer stays on when that GPU is AMD's and is disabled for this process only when the
/// game runs on another vendor's GPU in a system that also has an AMD GPU. The user's own
/// DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 (any value) or BB_AMD_SWITCHABLE_LAYER=1 (keep) wins.
void ChooseAmdSwitchableLayer(s32 physical_device_index) {
    static constexpr const char* DisableVar = "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1";
    if (std::getenv(DisableVar)) {
        LOG_INFO(Render_Vulkan, "AMD switchable graphics layer: {} set by the user", DisableVar);
        return;
    }
    if (const char* keep = std::getenv("BB_AMD_SWITCHABLE_LAYER"); keep && keep[0] == '1') {
        return;
    }
    const auto& d = VULKAN_HPP_DEFAULT_DISPATCHER;
    if (!d.vkCreateInstance || !d.vkGetInstanceProcAddr) {
        return;
    }
    SetProcessEnv(DisableVar, "1"); // the probe sees every GPU (the layer may hide some)
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance probe = VK_NULL_HANDLE;
    bool disable = false;
    std::string chosen_name;
    if (d.vkCreateInstance(&info, nullptr, &probe) == VK_SUCCESS) {
        const auto get = [&](const char* name) { return d.vkGetInstanceProcAddr(probe, name); };
        const auto enumerate =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get("vkEnumeratePhysicalDevices"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            get("vkGetPhysicalDeviceProperties"));
        const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get("vkGetPhysicalDeviceMemoryProperties"));
        const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
        u32 count = 0;
        std::vector<VkPhysicalDevice> devices;
        if (enumerate && properties && memory &&
            enumerate(probe, &count, nullptr) == VK_SUCCESS && count) {
            devices.resize(count);
            enumerate(probe, &count, devices.data());
            devices.resize(count);
        }
        struct Candidate {
            VkPhysicalDeviceProperties props;
            u64 local_memory;
        };
        std::vector<Candidate> candidates;
        bool any_amd = false;
        for (VkPhysicalDevice device : devices) {
            Candidate c{};
            properties(device, &c.props);
            VkPhysicalDeviceMemoryProperties mem{};
            memory(device, &mem);
            for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
                if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                    mem.memoryHeaps[i].size > c.local_memory) {
                    c.local_memory = mem.memoryHeaps[i].size;
                }
            }
            any_amd |= c.props.vendorID == 0x1002;
            candidates.push_back(c);
        }
        // Instance's rule: the target API version, a discrete GPU, not a CPU renderer, the most
        // device-local memory.
        const auto better = [](const Candidate& a, const Candidate& b) {
            const bool a_api = a.props.apiVersion >= TargetVulkanApiVersion;
            const bool b_api = b.props.apiVersion >= TargetVulkanApiVersion;
            if (a_api != b_api) {
                return a_api;
            }
            const bool a_discrete = a.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            const bool b_discrete = b.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (a_discrete != b_discrete) {
                return a_discrete;
            }
            const bool a_cpu = a.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            const bool b_cpu = b.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            if (a_cpu != b_cpu) {
                return b_cpu;
            }
            return a.local_memory > b.local_memory;
        };
        const Candidate* chosen = nullptr;
        if (physical_device_index >= 0 && u32(physical_device_index) < candidates.size()) {
            chosen = &candidates[physical_device_index];
        } else {
            for (const auto& c : candidates) {
                if (!chosen || better(c, *chosen)) {
                    chosen = &c;
                }
            }
        }
        if (chosen) {
            chosen_name = chosen->props.deviceName;
            disable = any_amd && chosen->props.vendorID != 0x1002;
        }
        if (destroy) {
            destroy(probe, nullptr);
        }
    }
    if (disable) {
        std::printf("GPU: AMD switchable graphics layer off for this game (it runs on %s; "
                    "BB_AMD_SWITCHABLE_LAYER=1 keeps the layer)\n",
                    chosen_name.c_str());
    } else {
        SetProcessEnv(DisableVar, nullptr);
    }
}
} // namespace

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic, s32 physical_device_index) {
    LOG_INFO(Render_Vulkan, "Creating vulkan instance");

#if defined(__APPLE__)
    // Initialize the environment with the path to the included ICD, so that the loader will
    // find it.
    static const auto icd_path = [] {
        char path[PATH_MAX];
        u32 length = PATH_MAX;
        _NSGetExecutablePath(path, &length);
        return std::filesystem::path(path).parent_path();
    }();
    setenv("VK_DRIVER_FILES", icd_path.c_str(), true);
#endif

    static vk::detail::DynamicLoader dl;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

    ChooseAmdSwitchableLayer(physical_device_index);

    const auto [available_version_result, available_version] =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion
            ? vk::enumerateInstanceVersion()
            : vk::ResultValue(vk::Result::eSuccess, VK_API_VERSION_1_0);
    ASSERT_MSG(available_version_result == vk::Result::eSuccess,
               "Failed to query Vulkan API version: {}", vk::to_string(available_version_result));
    ASSERT_MSG(available_version >= TargetVulkanApiVersion,
               "Vulkan {}.{} is required, but only {}.{} is supported by instance!",
               VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
               VK_VERSION_MAJOR(available_version), VK_VERSION_MINOR(available_version));

    const auto layers = GetInstanceLayers(enable_validation, enable_crash_diagnostic);
    auto extensions = GetLayerExtensions(GetInstanceExtensions(window_type, true), layers);
    // bbport: DLSS (optional bridge library) needs its own instance extensions.
    if (Dlss* dlss = Dlss::Get()) {
        dlss->AppendInstanceExtensions(extensions);
    }

    const vk::ApplicationInfo application_info = {
        .pApplicationName = "shadPS4",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "shadPS4 Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = available_version,
    };

    const std::string extensions_string = fmt::format("{}", fmt::join(extensions, ", "));
    const std::string layers_string = fmt::format("{}", fmt::join(layers, ", "));
    LOG_INFO(Render_Vulkan, "Enabled instance extensions: {}", extensions_string);
    LOG_INFO(Render_Vulkan, "Enabled instance layers: {}", layers_string);

    // Validation settings
    vk::Bool32 enable_core = EmulatorSettings.IsVkValidationCoreEnabled() ? vk::True : vk::False;
    vk::Bool32 enable_sync = EmulatorSettings.IsVkValidationSyncEnabled() ? vk::True : vk::False;
    vk::Bool32 enable_gpuav = EmulatorSettings.IsVkValidationGpuEnabled() ? vk::True : vk::False;

    // Crash diagnostics settings
    static const auto crash_diagnostic_path =
        Common::FS::GetUserPathString(Common::FS::PathType::LogDir);
    const char* log_path = crash_diagnostic_path.c_str();
    vk::Bool32 enable_force_barriers = vk::True;

    const std::array layer_setings = {
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "validate_core",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_core,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "validate_sync",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_sync,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "syncval_submit_time_validation",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_sync,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_enable",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_descriptor_checks",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_buffers_validation",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_draws_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_dispatches_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_indirect_trace_rays_buffers",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = VALIDATION_LAYER_NAME,
            .pSettingName = "gpuav_buffer_copies",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_gpuav,
        },
        vk::LayerSettingEXT{
            .pLayerName = "lunarg_crash_diagnostic",
            .pSettingName = "output_path",
            .type = vk::LayerSettingTypeEXT::eString,
            .valueCount = 1,
            .pValues = &log_path,
        },
        vk::LayerSettingEXT{
            .pLayerName = "lunarg_crash_diagnostic",
            .pSettingName = "sync_after_commands",
            .type = vk::LayerSettingTypeEXT::eBool32,
            .valueCount = 1,
            .pValues = &enable_force_barriers,
        },
    };

    vk::StructureChain<vk::InstanceCreateInfo, vk::LayerSettingsCreateInfoEXT> instance_ci_chain = {
        vk::InstanceCreateInfo{
            .pApplicationInfo = &application_info,
            .enabledLayerCount = static_cast<u32>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        },
        vk::LayerSettingsCreateInfoEXT{
            .settingCount = layer_setings.size(),
            .pSettings = layer_setings.data(),
        },
    };

    auto [instance_result, instance] = vk::createInstanceUnique(instance_ci_chain.get());
    ASSERT_MSG(instance_result == vk::Result::eSuccess, "Failed to create instance: {}",
               vk::to_string(instance_result));

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);

    return std::move(instance);
}

vk::UniqueDebugUtilsMessengerEXT CreateDebugCallback(vk::Instance instance) {
    const vk::DebugUtilsMessengerCreateInfoEXT msg_ci = {
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = DebugUtilsCallback,
    };
    auto [messenger_result, messenger] = instance.createDebugUtilsMessengerEXTUnique(msg_ci);
    ASSERT_MSG(messenger_result == vk::Result::eSuccess, "Failed to create debug callback: {}",
               vk::to_string(messenger_result));
    return std::move(messenger);
}

} // namespace Vulkan
