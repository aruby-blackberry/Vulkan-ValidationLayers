#!/usr/bin/python3 -i
#
# Copyright (c) 2015-2023 Valve Corporation
# Copyright (c) 2015-2023 LunarG, Inc.
# Copyright (c) 2015-2023 Google Inc.
# Copyright (c) 2023-2023 RasterGrid Kft.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This script generates the dispatch portion of a factory layer which intercepts
# all Vulkan  functions. The resultant factory layer allows rapid development of
# layers and interceptors.

import sys
from generator import *
from common_codegen import *

# NOTE: should be removed if generation scripts ever get refactored
from generators.parameter_validation_generator import ParameterValidationOutputGenerator

# LayerChassisOutputGenerator - subclass of OutputGenerator.
# Generates a LayerFactory layer that intercepts all API entrypoints
#  This is intended to be used as a starting point for creating custom layers
#
# ---- methods ----
# LayerChassisOutputGenerator(errFile, warnFile, diagFile) - args as for
#   OutputGenerator. Defines additional internal state.
# ---- methods overriding base class ----
# beginFile(genOpts)
# endFile()
# beginFeature(interface, emit)
# endFeature()
# genType(typeinfo,name)
# genStruct(typeinfo,name)
# genGroup(groupinfo,name)
# genEnum(enuminfo, name)
# genCmd(cmdinfo)
class LayerChassisOutputGenerator(OutputGenerator):
    """Generate specified API interfaces in a specific style, such as a C header"""
    # This is an ordered list of sections in the header file.
    TYPE_SECTIONS = ['include', 'define', 'basetype', 'handle', 'enum',
                     'group', 'bitmask', 'funcpointer', 'struct']
    ALL_SECTIONS = TYPE_SECTIONS + ['command']

    manual_functions = [
        # Include functions here to be interecpted w/ manually implemented function bodies
        'vkGetDeviceProcAddr',
        'vkGetInstanceProcAddr',
        'vkCreateDevice',
        'vkDestroyDevice',
        'vkCreateInstance',
        'vkDestroyInstance',
        'vkEnumerateInstanceLayerProperties',
        'vkEnumerateInstanceExtensionProperties',
        'vkEnumerateDeviceLayerProperties',
        'vkEnumerateDeviceExtensionProperties',
        # Functions that are handled explicitly due to chassis architecture violations
        'vkCreateGraphicsPipelines',
        'vkCreateComputePipelines',
        'vkCreateRayTracingPipelinesNV',
        'vkCreateRayTracingPipelinesKHR',
        'vkCreatePipelineLayout',
        'vkCreateShaderModule',
        'vkAllocateDescriptorSets',
        'vkCreateBuffer',
        # ValidationCache functions do not get dispatched
        'vkCreateValidationCacheEXT',
        'vkDestroyValidationCacheEXT',
        'vkMergeValidationCachesEXT',
        'vkGetValidationCacheDataEXT',
        'vkGetPhysicalDeviceToolPropertiesEXT',
        ]

    alt_ret_codes = [
        # Include functions here which must tolerate VK_INCOMPLETE as a return code
        'vkEnumeratePhysicalDevices',
        'vkEnumeratePhysicalDeviceGroupsKHR',
        'vkGetValidationCacheDataEXT',
        'vkGetPipelineCacheData',
        'vkGetShaderInfoAMD',
        'vkGetPhysicalDeviceDisplayPropertiesKHR',
        'vkGetPhysicalDeviceDisplayProperties2KHR',
        'vkGetPhysicalDeviceDisplayPlanePropertiesKHR',
        'vkGetDisplayPlaneSupportedDisplaysKHR',
        'vkGetDisplayModePropertiesKHR',
        'vkGetDisplayModeProperties2KHR',
        'vkGetPhysicalDeviceSurfaceFormatsKHR',
        'vkGetPhysicalDeviceSurfacePresentModesKHR',
        'vkGetPhysicalDevicePresentRectanglesKHR',
        'vkGetPastPresentationTimingGOOGLE',
        'vkGetSwapchainImagesKHR',
        'vkEnumerateInstanceLayerProperties',
        'vkEnumerateDeviceLayerProperties',
        'vkEnumerateInstanceExtensionProperties',
        'vkEnumerateDeviceExtensionProperties',
        'vkGetPhysicalDeviceCalibrateableTimeDomainsEXT',
    ]

    pre_dispatch_debug_utils_functions = {
        'vkDebugMarkerSetObjectNameEXT' : 'layer_data->report_data->DebugReportSetMarkerObjectName(pNameInfo);',
        'vkSetDebugUtilsObjectNameEXT' : 'layer_data->report_data->DebugReportSetUtilsObjectName(pNameInfo);',
        'vkQueueBeginDebugUtilsLabelEXT' : 'BeginQueueDebugUtilsLabel(layer_data->report_data, queue, pLabelInfo);',
        'vkQueueInsertDebugUtilsLabelEXT' : 'InsertQueueDebugUtilsLabel(layer_data->report_data, queue, pLabelInfo);',
        }

    post_dispatch_debug_utils_functions = {
        'vkQueueEndDebugUtilsLabelEXT' : 'EndQueueDebugUtilsLabel(layer_data->report_data, queue);',
        'vkCreateDebugReportCallbackEXT' : 'LayerCreateReportCallback(layer_data->report_data, false, pCreateInfo, pCallback);',
        'vkDestroyDebugReportCallbackEXT' : 'LayerDestroyCallback(layer_data->report_data, callback);',
        'vkCreateDebugUtilsMessengerEXT' : 'LayerCreateMessengerCallback(layer_data->report_data, false, pCreateInfo, pMessenger);',
        'vkDestroyDebugUtilsMessengerEXT' : 'LayerDestroyCallback(layer_data->report_data, messenger);',
        }

    # Avoid using auto in generated code. Intellisense has been known to have issues with large files.
    precallvalidate_loop = "for (const ValidationObject* intercept : layer_data->object_dispatch) {"
    precallrecord_loop = "for (ValidationObject* intercept : layer_data->object_dispatch) {"
    postcallrecord_loop = precallrecord_loop

    # Vulkan validation layer list
    vk_validation_layers = [
        {
            'include': 'thread_tracker/thread_safety_validation.h',
            'class': 'ThreadSafety',
            'enabled': '!disables[thread_safety]'
        },
        {
            'include': 'stateless/stateless_validation.h',
            'class': 'StatelessValidation',
            'enabled': '!disables[stateless_checks]'
        },
        {
            'include': 'object_tracker/object_lifetime_validation.h',
            'class': 'ObjectLifetimes',
            'enabled': '!disables[object_tracking]'
        },
        {
            'include': 'core_checks/core_validation.h',
            'class': 'CoreChecks',
            'enabled': '!disables[core_checks]'
        },
        {
            'include': 'best_practices/best_practices_validation.h',
            'class': 'BestPractices',
            'enabled': 'enables[best_practices]'
        },
        {
            'include': 'gpu_validation/gpu_validation.h',
            'class': 'GpuAssisted',
            'enabled': 'enables[gpu_validation]'
        },
        {
            'include': 'gpu_validation/debug_printf.h',
            'class': 'DebugPrintf',
            'enabled': 'enables[debug_printf]'
        },
        {
            'include': 'sync/sync_validation.h',
            'class': 'SyncValidator',
            'enabled': 'enables[sync_validation]'
        }
    ]

    # Vulkan extension lists
    vk_instance_extensions = [
        'VK_EXT_debug_report',
        'VK_EXT_debug_utils',
        'VK_EXT_validation_features'
    ]

    vk_device_extensions = [
        'VK_EXT_validation_cache',
        'VK_EXT_debug_marker',
        'VK_EXT_tooling_info'
    ]

    inline_custom_header_preamble = """
#include <atomic>
#include <mutex>
#include <cinttypes>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <memory>
#include <string_view>

#include "vulkan/vulkan.h"
#include "utils/cast_utils.h"
#include "vk_layer_settings_ext.h"
#include "vk_layer_config.h"
#include "containers/custom_containers.h"
#include "error_message/logging.h"
#include "vk_object_types.h"
#include "vulkan/vk_layer.h"
#include "vk_enum_string_helper.h"
#include "utils/vk_layer_extension_utils.h"
#include "utils/vk_layer_utils.h"
#include "vulkan/vk_layer.h"
#include "vk_dispatch_table_helper.h"
#include "vk_extension_helper.h"
#include "vk_safe_struct.h"
#include "vk_typemap_helper.h"


extern std::atomic<uint64_t> global_unique_id;

// To avoid re-hashing unique ids on each use, we precompute the hash and store the
// hash's LSBs in the high 24 bits.
struct HashedUint64 {
    static const int HASHED_UINT64_SHIFT = 40;
    size_t operator()(const uint64_t &t) const { return t >> HASHED_UINT64_SHIFT; }

    static uint64_t hash(uint64_t id) {
        uint64_t h = (uint64_t)vvl::hash<uint64_t>()(id);
        id |= h << HASHED_UINT64_SHIFT;
        return id;
    }
};

extern vl_concurrent_unordered_map<uint64_t, uint64_t, 4, HashedUint64> unique_id_mapping;


VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetPhysicalDeviceProcAddr(
    VkInstance                                  instance,
    const char*                                 funcName);
"""

    inline_custom_header_class_definition = """

// Layer object type identifiers
enum LayerObjectTypeId {
    LayerObjectTypeInstance,                    // Container for an instance dispatch object
    LayerObjectTypeDevice,                      // Container for a device dispatch object
    LayerObjectTypeThreading,                   // Instance or device threading layer object
    LayerObjectTypeParameterValidation,         // Instance or device parameter validation layer object
    LayerObjectTypeObjectTracker,               // Instance or device object tracker layer object
    LayerObjectTypeCoreValidation,              // Instance or device core validation layer object
    LayerObjectTypeBestPractices,               // Instance or device best practices layer object
    LayerObjectTypeGpuAssisted,                 // Instance or device gpu assisted validation layer object
    LayerObjectTypeDebugPrintf,                 // Instance or device shader debug printf layer object
    LayerObjectTypeSyncValidation,              // Instance or device synchronization validation layer object
    LayerObjectTypeMaxEnum,                     // Max enum count
};

struct TEMPLATE_STATE {
    VkDescriptorUpdateTemplate desc_update_template;
    safe_VkDescriptorUpdateTemplateCreateInfo create_info;
    bool destroyed;

    TEMPLATE_STATE(VkDescriptorUpdateTemplate update_template, safe_VkDescriptorUpdateTemplateCreateInfo *pCreateInfo)
        : desc_update_template(update_template), create_info(*pCreateInfo), destroyed(false) {}
};

class LAYER_PHYS_DEV_PROPERTIES {
public:
    VkPhysicalDeviceProperties properties;
    std::vector<VkQueueFamilyProperties> queue_family_properties;
};

typedef enum ValidationCheckDisables {
    VALIDATION_CHECK_DISABLE_COMMAND_BUFFER_STATE,
    VALIDATION_CHECK_DISABLE_OBJECT_IN_USE,
    VALIDATION_CHECK_DISABLE_QUERY_VALIDATION,
    VALIDATION_CHECK_DISABLE_IMAGE_LAYOUT_VALIDATION,
} ValidationCheckDisables;

typedef enum ValidationCheckEnables {
    VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_ARM,
    VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_AMD,
    VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_IMG,
    VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_NVIDIA,
    VALIDATION_CHECK_ENABLE_VENDOR_SPECIFIC_ALL,
    VALIDATION_CHECK_ENABLE_SYNCHRONIZATION_VALIDATION_QUEUE_SUBMIT,
} ValidationCheckEnables;

typedef enum VkValidationFeatureEnable {
    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION,
} VkValidationFeatureEnable;

// CHECK_DISABLED and CHECK_ENABLED vectors are containers for bools that can opt in or out of specific classes of validation
// checks. Enum values can be specified via the vk_layer_settings.txt config file or at CreateInstance time via the
// VK_EXT_validation_features extension that can selectively disable or enable checks.
typedef enum DisableFlags {
    command_buffer_state,
    object_in_use,
    query_validation,
    image_layout_validation,
    object_tracking,
    core_checks,
    thread_safety,
    stateless_checks,
    handle_wrapping,
    shader_validation,
    shader_validation_caching,
    // Insert new disables above this line
    kMaxDisableFlags,
} DisableFlags;

typedef enum EnableFlags {
    gpu_validation,
    gpu_validation_reserve_binding_slot,
    best_practices,
    vendor_specific_arm,
    vendor_specific_amd,
    vendor_specific_img,
    vendor_specific_nvidia,
    debug_printf,
    sync_validation,
    sync_validation_queue_submit,
    // Insert new enables above this line
    kMaxEnableFlags,
} EnableFlags;

typedef std::array<bool, kMaxDisableFlags> CHECK_DISABLED;
typedef std::array<bool, kMaxEnableFlags> CHECK_ENABLED;

#if defined(__clang__)
#define DECORATE_PRINTF(_fmt_argnum, _first_param_num)  __attribute__((format (printf, _fmt_argnum, _first_param_num)))
#elif defined(__GNUC__)
#define DECORATE_PRINTF(_fmt_argnum, _first_param_num)  __attribute__((format (gnu_printf, _fmt_argnum, _first_param_num)))
#else
#define DECORATE_PRINTF(_fmt_num, _first_param_num)
#endif
// Layer chassis validation object base class definition
class ValidationObject {
    public:
        APIVersion api_version;
        debug_report_data* report_data = nullptr;
        std::vector<std::vector<ValidationObject*>> intercept_vectors;

        VkLayerInstanceDispatchTable instance_dispatch_table;
        VkLayerDispatchTable device_dispatch_table;

        InstanceExtensions instance_extensions;
        DeviceExtensions device_extensions = {};
        CHECK_DISABLED disabled = {};
        CHECK_ENABLED enabled = {};
        bool fine_grained_locking{true};

        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        LAYER_PHYS_DEV_PROPERTIES phys_dev_properties = {};

        std::vector<ValidationObject*> object_dispatch;
        LayerObjectTypeId container_type;

        vl_concurrent_unordered_map<VkDeferredOperationKHR, std::vector<std::function<void()>>, 0> deferred_operation_post_completion;
        vl_concurrent_unordered_map<VkDeferredOperationKHR, std::vector<std::function<void(const std::vector<VkPipeline>&)>>, 0> deferred_operation_post_check;
        vl_concurrent_unordered_map<VkDeferredOperationKHR, std::vector<VkPipeline>, 0> deferred_operation_pipelines;

        std::string layer_name = "CHASSIS";

        // Constructor
        ValidationObject(){};
        // Destructor
        virtual ~ValidationObject() {};

        void InitObjectDispatchVectors();

        mutable std::shared_mutex validation_object_mutex;
        virtual ReadLockGuard ReadLock() const {
            return ReadLockGuard(validation_object_mutex);
        }
        virtual WriteLockGuard WriteLock() {
            return WriteLockGuard(validation_object_mutex);
        }

        ValidationObject* GetValidationObject(std::vector<ValidationObject*>& object_dispatch, LayerObjectTypeId object_type) {
            for (auto validation_object : object_dispatch) {
                if (validation_object->container_type == object_type) {
                    return validation_object;
                }
            }
            return nullptr;
        }

        // Debug Logging Helpers
        bool DECORATE_PRINTF(4, 5) LogError(const LogObjectList &objlist, std::string_view vuid_text, const char *format, ...) const {
            va_list argptr;
            va_start(argptr, format);
            const bool result = LogMsg(report_data, kErrorBit, objlist, vuid_text, format, argptr);
            va_end(argptr);
            return result;
        }

        bool DECORATE_PRINTF(4, 5) LogWarning(const LogObjectList &objlist, std::string_view vuid_text, const char *format, ...) const {
            va_list argptr;
            va_start(argptr, format);
            const bool result = LogMsg(report_data, kWarningBit, objlist, vuid_text, format, argptr);
            va_end(argptr);
            return result;
        }

        bool DECORATE_PRINTF(4, 5) LogPerformanceWarning(const LogObjectList &objlist, std::string_view vuid_text, const char *format, ...) const {
            va_list argptr;
            va_start(argptr, format);
            const bool result = LogMsg(report_data, kPerformanceWarningBit, objlist, vuid_text, format, argptr);
            va_end(argptr);
            return result;
        }

        bool DECORATE_PRINTF(4, 5) LogInfo(const LogObjectList &objlist, std::string_view vuid_text, const char *format, ...) const {
            va_list argptr;
            va_start(argptr, format);
            const bool result = LogMsg(report_data, kInformationBit, objlist, vuid_text, format, argptr);
            va_end(argptr);
            return result;
        }

        bool DECORATE_PRINTF(4, 5) LogVerbose(const LogObjectList &objlist, std::string_view vuid_text, const char *format, ...) const {
            va_list argptr;
            va_start(argptr, format);
            const bool result = LogMsg(report_data, kVerboseBit, objlist, vuid_text, format, argptr);
            va_end(argptr);
            return result;
        }

        void LogInternalError(std::string_view failure_location, const LogObjectList &obj_list,
                              std::string_view entrypoint, VkResult err) const {
            const std::string_view err_string = string_VkResult(err);
            std::string vuid = "INTERNAL-ERROR-";
            vuid += entrypoint;
            LogError(obj_list, vuid, "In %s: %s() was called in the Validation Layer state tracking and failed with result = %s.",
                     failure_location.data(), entrypoint.data(), err_string.data());
        }

        // Handle Wrapping Data
        // Reverse map display handles
        vl_concurrent_unordered_map<VkDisplayKHR, uint64_t, 0> display_id_reverse_mapping;
        // Wrapping Descriptor Template Update structures requires access to the template createinfo structs
        vvl::unordered_map<uint64_t, std::unique_ptr<TEMPLATE_STATE>> desc_template_createinfo_map;
        struct SubpassesUsageStates {
            vvl::unordered_set<uint32_t> subpasses_using_color_attachment;
            vvl::unordered_set<uint32_t> subpasses_using_depthstencil_attachment;
        };
        // Uses unwrapped handles
        vvl::unordered_map<VkRenderPass, SubpassesUsageStates> renderpasses_states;
        // Map of wrapped swapchain handles to arrays of wrapped swapchain image IDs
        // Each swapchain has an immutable list of wrapped swapchain image IDs -- always return these IDs if they exist
        vvl::unordered_map<VkSwapchainKHR, std::vector<VkImage>> swapchain_wrapped_image_handle_map;
        // Map of wrapped descriptor pools to set of wrapped descriptor sets allocated from each pool
        vvl::unordered_map<VkDescriptorPool, vvl::unordered_set<VkDescriptorSet>> pool_descriptor_sets_map;


        // Unwrap a handle.
        template <typename HandleType>
        HandleType Unwrap(HandleType wrappedHandle) {
            if (wrappedHandle == (HandleType)VK_NULL_HANDLE) return wrappedHandle;
            auto iter = unique_id_mapping.find(CastToUint64(wrappedHandle));
            if (iter == unique_id_mapping.end())
                return (HandleType)0;
            return (HandleType)iter->second;
        }

        // Wrap a newly created handle with a new unique ID, and return the new ID.
        template <typename HandleType>
        HandleType WrapNew(HandleType newlyCreatedHandle) {
            if (newlyCreatedHandle == (HandleType)VK_NULL_HANDLE) return newlyCreatedHandle;
            auto unique_id = global_unique_id++;
            unique_id = HashedUint64::hash(unique_id);
            assert(unique_id != 0); // can't be 0, otherwise unwrap will apply special rule for VK_NULL_HANDLE
            unique_id_mapping.insert_or_assign(unique_id, CastToUint64(newlyCreatedHandle));
            return (HandleType)unique_id;
        }

        // Specialized handling for VkDisplayKHR. Adds an entry to enable reverse-lookup.
        VkDisplayKHR WrapDisplay(VkDisplayKHR newlyCreatedHandle, ValidationObject *map_data) {
            auto unique_id = global_unique_id++;
            unique_id = HashedUint64::hash(unique_id);
            unique_id_mapping.insert_or_assign(unique_id, CastToUint64(newlyCreatedHandle));
            map_data->display_id_reverse_mapping.insert_or_assign(newlyCreatedHandle, unique_id);
            return (VkDisplayKHR)unique_id;
        }

        // VkDisplayKHR objects don't have a single point of creation, so we need to see if one already exists in the map before
        // creating another.
        VkDisplayKHR MaybeWrapDisplay(VkDisplayKHR handle, ValidationObject *map_data) {
            // See if this display is already known
            auto it = map_data->display_id_reverse_mapping.find(handle);
            if (it != map_data->display_id_reverse_mapping.end()) return (VkDisplayKHR)it->second;
            // Unknown, so wrap
            return WrapDisplay(handle, map_data);
        }

        // Pre/post hook point declarations
"""

    inline_copyright_message = """
// This file is ***GENERATED***.  Do Not Edit.
// See layer_chassis_generator.py for modifications.

/* Copyright (c) 2015-2023 The Khronos Group Inc.
 * Copyright (c) 2015-2023 Valve Corporation
 * Copyright (c) 2015-2023 LunarG, Inc.
 * Copyright (c) 2015-2023 Google Inc.
 * Copyright (c) 2023-2023 RasterGrid Kft.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */"""

    inline_custom_source_preamble_1 = """

#include <string.h>
#include <mutex>

#include "chassis.h"
#include "layer_options.h"
#include "layer_chassis_dispatch.h"

small_unordered_map<void*, ValidationObject*, 2> layer_data_map;

// Global unique object identifier.
std::atomic<uint64_t> global_unique_id(1ULL);
// Map uniqueID to actual object handle. Accesses to the map itself are
// internally synchronized.
vl_concurrent_unordered_map<uint64_t, uint64_t, 4, HashedUint64> unique_id_mapping;

bool wrap_handles = true;

#define OBJECT_LAYER_NAME "VK_LAYER_KHRONOS_validation"
#define OBJECT_LAYER_DESCRIPTION "khronos_validation"
"""

    inline_custom_source_preamble_2 = """
// Global list of sType,size identifiers
std::vector<std::pair<uint32_t, uint32_t>> custom_stype_info{};

namespace vulkan_layer_chassis {

static const VkLayerProperties global_layer = {
    OBJECT_LAYER_NAME, VK_LAYER_API_VERSION, 1, "LunarG validation Layer",
};

typedef enum ApiFunctionType {
    kFuncTypeInst = 0,
    kFuncTypePdev = 1,
    kFuncTypeDev = 2
} ApiFunctionType;

typedef struct {
    ApiFunctionType function_type;
    void* funcptr;
} function_data;

extern const vvl::unordered_map<std::string, function_data> name_to_funcptr_map;

// Manually written functions

// Check enabled instance extensions against supported instance extension whitelist
static void InstanceExtensionWhitelist(ValidationObject *layer_data, const VkInstanceCreateInfo *pCreateInfo, VkInstance instance) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        // Check for recognized instance extensions
        if (!white_list(pCreateInfo->ppEnabledExtensionNames[i], kInstanceExtensionNames)) {
            layer_data->LogWarning(layer_data->instance, kVUIDUndefined,
                    "Instance Extension %s is not supported by this layer.  Using this extension may adversely affect validation "
                    "results and/or produce undefined behavior.",
                    pCreateInfo->ppEnabledExtensionNames[i]);
        }
    }
}

// Check enabled device extensions against supported device extension whitelist
static void DeviceExtensionWhitelist(ValidationObject *layer_data, const VkDeviceCreateInfo *pCreateInfo, VkDevice device) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        // Check for recognized device extensions
        if (!white_list(pCreateInfo->ppEnabledExtensionNames[i], kDeviceExtensionNames)) {
            layer_data->LogWarning(layer_data->device, kVUIDUndefined,
                    "Device Extension %s is not supported by this layer.  Using this extension may adversely affect validation "
                    "results and/or produce undefined behavior.",
                    pCreateInfo->ppEnabledExtensionNames[i]);
        }
    }
}

static void DeviceExtensionWarnlist(ValidationObject *layer_data, const VkDeviceCreateInfo *pCreateInfo, VkDevice device);

void OutputLayerStatusInfo(ValidationObject *context) {
    std::string list_of_enables;
    std::string list_of_disables;
    for (uint32_t i = 0; i < kMaxEnableFlags; i++) {
        if (context->enabled[i]) {
            if (list_of_enables.size()) list_of_enables.append(", ");
            list_of_enables.append(EnableFlagNameHelper[i]);
        }
    }
    if (list_of_enables.size() == 0) {
        list_of_enables.append("None");
    }
    for (uint32_t i = 0; i < kMaxDisableFlags; i++) {
        if (context->disabled[i]) {
            if (list_of_disables.size()) list_of_disables.append(", ");
            list_of_disables.append(DisableFlagNameHelper[i]);
        }
    }
    if (list_of_disables.size() == 0) {
        list_of_disables.append("None");
    }

    auto settings_info = GetLayerSettingsFileInfo();
    std::string settings_status;
    if (!settings_info->file_found) {
        settings_status = "None. Default location is ";
        settings_status.append(settings_info->location);
        settings_status.append(".");
    } else {
        settings_status = "Found at ";
        settings_status.append(settings_info->location);
        settings_status.append(" specified by ");
        switch (settings_info->source) {
            case kEnvVar:
                settings_status.append("environment variable (VK_LAYER_SETTINGS_PATH).");
                break;
            case kVkConfig:
                settings_status.append("VkConfig application override.");
                break;
            case kLocal:    // Intentionally fall through
            default:
                settings_status.append("default location (current working directory).");
                break;
        }
    }

    // Output layer status information message
    context->LogInfo(context->instance, "UNASSIGNED-CreateInstance-status-message",
        "Khronos Validation Layer Active:\\n    Settings File: %s\\n    Current Enables: %s.\\n    Current Disables: %s.\\n",
        settings_status.c_str(), list_of_enables.c_str(), list_of_disables.c_str());

    // Create warning message if user is running debug layers.
#ifndef NDEBUG
    context->LogPerformanceWarning(context->instance, "UNASSIGNED-CreateInstance-debug-warning",
        "VALIDATION LAYERS WARNING: Using debug builds of the validation layers *will* adversely affect performance.");
#endif
    if (!context->fine_grained_locking) {
        context->LogPerformanceWarning(context->instance, "UNASSIGNED-CreateInstance-locking-warning",
                                       "Fine-grained locking is disabled, this will adversely affect performance of multithreaded applications.");
    }
}

// Non-code-generated chassis API functions

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *funcName) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    if (!ApiParentExtensionEnabled(funcName, &layer_data->device_extensions)) {
        return nullptr;
    }
    const auto &item = name_to_funcptr_map.find(funcName);
    if (item != name_to_funcptr_map.end()) {
        if (item->second.function_type != kFuncTypeDev) {
            return nullptr;
        } else {
            return reinterpret_cast<PFN_vkVoidFunction>(item->second.funcptr);
        }
    }
    auto &table = layer_data->device_dispatch_table;
    if (!table.GetDeviceProcAddr) return nullptr;
    return table.GetDeviceProcAddr(device, funcName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *funcName) {
    const auto &item = name_to_funcptr_map.find(funcName);
    if (item != name_to_funcptr_map.end()) {
        return reinterpret_cast<PFN_vkVoidFunction>(item->second.funcptr);
    }
    auto layer_data = GetLayerDataPtr(get_dispatch_key(instance), layer_data_map);
    auto &table = layer_data->instance_dispatch_table;
    if (!table.GetInstanceProcAddr) return nullptr;
    return table.GetInstanceProcAddr(instance, funcName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetPhysicalDeviceProcAddr(VkInstance instance, const char *funcName) {
    const auto &item = name_to_funcptr_map.find(funcName);
    if (item != name_to_funcptr_map.end()) {
        if (item->second.function_type != kFuncTypePdev) {
            return nullptr;
        } else {
            return reinterpret_cast<PFN_vkVoidFunction>(item->second.funcptr);
        }
    }
    auto layer_data = GetLayerDataPtr(get_dispatch_key(instance), layer_data_map);
    auto &table = layer_data->instance_dispatch_table;
    if (!table.GetPhysicalDeviceProcAddr) return nullptr;
    return table.GetPhysicalDeviceProcAddr(instance, funcName);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties) {
    return util_GetLayerProperties(1, &global_layer, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                              VkLayerProperties *pProperties) {
    return util_GetLayerProperties(1, &global_layer, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                    VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, global_layer.layerName))
        return util_GetExtensionProperties(ARRAY_SIZE(instance_extensions), instance_extensions, pCount, pProperties);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                  uint32_t *pCount, VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, global_layer.layerName)) return util_GetExtensionProperties(ARRAY_SIZE(device_extensions), device_extensions, pCount, pProperties);
    assert(physicalDevice);
    auto layer_data = GetLayerDataPtr(get_dispatch_key(physicalDevice), layer_data_map);
    return layer_data->instance_dispatch_table.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                              VkInstance *pInstance) {
    VkLayerInstanceCreateInfo* chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (fpCreateInstance == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;
    uint32_t specified_version = (pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0);
    APIVersion api_version = VK_MAKE_API_VERSION(VK_API_VERSION_VARIANT(specified_version), VK_API_VERSION_MAJOR(specified_version), VK_API_VERSION_MINOR(specified_version), 0);

    auto report_data = new debug_report_data{};
    report_data->instance_pnext_chain = SafePnextCopy(pCreateInfo->pNext);
    ActivateInstanceDebugCallbacks(report_data);

    // Set up enable and disable features flags
    CHECK_ENABLED local_enables {};
    CHECK_DISABLED local_disables {};
    bool lock_setting;
    ConfigAndEnvSettings config_and_env_settings_data {OBJECT_LAYER_DESCRIPTION, pCreateInfo->pNext, local_enables, local_disables,
        report_data->filter_message_ids, &report_data->duplicate_message_limit, &lock_setting};
    ProcessConfigAndEnvSettings(&config_and_env_settings_data);
    layer_debug_messenger_actions(report_data, OBJECT_LAYER_DESCRIPTION);

    // Create temporary dispatch vector for pre-calls until instance is created
    std::vector<ValidationObject*> local_object_dispatch = CreateObjectDispatch(local_enables, local_disables);

    // If handle wrapping is disabled via the ValidationFeatures extension, override build flag
    if (local_disables[handle_wrapping]) {
        wrap_handles = false;
    }

    // Initialize the validation objects
    for (auto* intercept : local_object_dispatch) {
        intercept->api_version = api_version;
        intercept->report_data = report_data;
    }

    // Define logic to cleanup everything in case of an error
    auto cleanup_allocations = [report_data, &local_object_dispatch](){
        DeactivateInstanceDebugCallbacks(report_data);
        FreePnextChain(report_data->instance_pnext_chain);
        LayerDebugUtilsDestroyInstance(report_data);
        for (ValidationObject* object : local_object_dispatch) {
            delete object;
        }
    };

    // Init dispatch array and call registration functions
    bool skip = false;
    for (const ValidationObject* intercept : local_object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateInstance(pCreateInfo, pAllocator, pInstance);
        if (skip) {
            cleanup_allocations();
            return VK_ERROR_VALIDATION_FAILED_EXT;
        }
    }
    for (ValidationObject* intercept : local_object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateInstance(pCreateInfo, pAllocator, pInstance);
    }

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        cleanup_allocations();
        return result;
    }
    auto framework = GetLayerDataPtr(get_dispatch_key(*pInstance), layer_data_map);

    framework->object_dispatch = local_object_dispatch;
    framework->container_type = LayerObjectTypeInstance;
    framework->disabled = local_disables;
    framework->enabled = local_enables;
    framework->fine_grained_locking = lock_setting;

    framework->instance = *pInstance;
    layer_init_instance_dispatch_table(*pInstance, &framework->instance_dispatch_table, fpGetInstanceProcAddr);
    framework->report_data = report_data;
    framework->api_version = api_version;
    framework->instance_extensions.InitFromInstanceCreateInfo(specified_version, pCreateInfo);

    // We need to call this to properly check which device extensions have been promoted when validating query functions
    // that take as input a physical device, which can be called before a logical device has been created.
    framework->device_extensions.InitFromDeviceCreateInfo(&framework->instance_extensions, specified_version);

    OutputLayerStatusInfo(framework);

    for (auto* intercept : framework->object_dispatch) {
        intercept->instance_dispatch_table = framework->instance_dispatch_table;
        intercept->enabled = framework->enabled;
        intercept->disabled = framework->disabled;
        intercept->fine_grained_locking = framework->fine_grained_locking;
        intercept->instance = *pInstance;
    }

    for (ValidationObject* intercept : framework->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateInstance(pCreateInfo, pAllocator, pInstance, result);
    }

    InstanceExtensionWhitelist(framework, pCreateInfo, *pInstance);
    DeactivateInstanceDebugCallbacks(report_data);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(instance);
    auto layer_data = GetLayerDataPtr(key, layer_data_map);
    ActivateInstanceDebugCallbacks(layer_data->report_data);

    """ + precallvalidate_loop + """
        auto lock = intercept->ReadLock();
        intercept->PreCallValidateDestroyInstance(instance, pAllocator);
    }
    """ + precallrecord_loop + """
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordDestroyInstance(instance, pAllocator);
    }

    layer_data->instance_dispatch_table.DestroyInstance(instance, pAllocator);

    """ + postcallrecord_loop + """
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordDestroyInstance(instance, pAllocator);
    }

    DeactivateInstanceDebugCallbacks(layer_data->report_data);
    FreePnextChain(layer_data->report_data->instance_pnext_chain);

    LayerDebugUtilsDestroyInstance(layer_data->report_data);

    for (auto item = layer_data->object_dispatch.begin(); item != layer_data->object_dispatch.end(); item++) {
        delete *item;
    }
    FreeLayerDataPtr(key, layer_data_map);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    auto instance_interceptor = GetLayerDataPtr(get_dispatch_key(gpu), layer_data_map);

    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(instance_interceptor->instance, "vkCreateDevice");
    if (fpCreateDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    // Get physical device limits for device
    VkPhysicalDeviceProperties device_properties = {};
    instance_interceptor->instance_dispatch_table.GetPhysicalDeviceProperties(gpu, &device_properties);

    // Setup the validation tables based on the application API version from the instance and the capabilities of the device driver
    auto effective_api_version = std::min(APIVersion(device_properties.apiVersion), instance_interceptor->api_version);

    DeviceExtensions device_extensions = {};
    device_extensions.InitFromDeviceCreateInfo(&instance_interceptor->instance_extensions, effective_api_version, pCreateInfo);
    for (auto item : instance_interceptor->object_dispatch) {
        item->device_extensions = device_extensions;
    }

    safe_VkDeviceCreateInfo modified_create_info(pCreateInfo);

    bool skip = false;
    for (const ValidationObject* intercept : instance_interceptor->object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : instance_interceptor->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateDevice(gpu, pCreateInfo, pAllocator, pDevice, &modified_create_info);
    }

    VkResult result = fpCreateDevice(gpu, reinterpret_cast<VkDeviceCreateInfo *>(&modified_create_info), pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    auto device_interceptor = GetLayerDataPtr(get_dispatch_key(*pDevice), layer_data_map);
    device_interceptor->container_type = LayerObjectTypeDevice;

    // Save local info in device object
    device_interceptor->phys_dev_properties.properties = device_properties;
    device_interceptor->api_version = device_interceptor->device_extensions.InitFromDeviceCreateInfo(
        &instance_interceptor->instance_extensions, effective_api_version, pCreateInfo);
    device_interceptor->device_extensions = device_extensions;

    layer_init_device_dispatch_table(*pDevice, &device_interceptor->device_dispatch_table, fpGetDeviceProcAddr);

    device_interceptor->device = *pDevice;
    device_interceptor->physical_device = gpu;
    device_interceptor->instance = instance_interceptor->instance;
    device_interceptor->report_data = instance_interceptor->report_data;

    InitDeviceObjectDispatch(instance_interceptor, device_interceptor);

    // Initialize all of the objects with the appropriate data
    for(auto* object : device_interceptor->object_dispatch) {
        object->device = device_interceptor->device;
        object->physical_device = device_interceptor->physical_device;
        object->instance = instance_interceptor->instance;
        object->report_data = instance_interceptor->report_data;
        object->device_dispatch_table = device_interceptor->device_dispatch_table;
        object->api_version = device_interceptor->api_version;
        object->disabled = instance_interceptor->disabled;
        object->enabled = instance_interceptor->enabled;
        object->fine_grained_locking = instance_interceptor->fine_grained_locking;
        object->instance_dispatch_table = instance_interceptor->instance_dispatch_table;
        object->instance_extensions = instance_interceptor->instance_extensions;
        object->device_extensions = device_interceptor->device_extensions;
    }

    for (ValidationObject* intercept : instance_interceptor->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateDevice(gpu, pCreateInfo, pAllocator, pDevice, result);
    }

    device_interceptor->InitObjectDispatchVectors();

    DeviceExtensionWhitelist(device_interceptor, pCreateInfo, *pDevice);
    DeviceExtensionWarnlist(device_interceptor, pCreateInfo, *pDevice);

    return result;
}

// NOTE: Do _not_ skip the dispatch call when destroying a device. Whether or not there was a validation error,
//       the loader will destroy the device, and know nothing about future references to this device making it
//       impossible for the caller to use this device handle further. IOW, this is our _only_ chance to (potentially)
//       dispatch the driver's DestroyDevice function.
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    dispatch_key key = get_dispatch_key(device);
    auto layer_data = GetLayerDataPtr(key, layer_data_map);
    """ + precallvalidate_loop + """
        auto lock = intercept->ReadLock();
        intercept->PreCallValidateDestroyDevice(device, pAllocator);
    }
    """ + precallrecord_loop + """
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordDestroyDevice(device, pAllocator);
    }

    layer_data->device_dispatch_table.DestroyDevice(device, pAllocator);

    """ + postcallrecord_loop + """
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordDestroyDevice(device, pAllocator);
    }

    for (auto item = layer_data->object_dispatch.begin(); item != layer_data->object_dispatch.end(); item++) {
        delete *item;
    }
    FreeLayerDataPtr(key, layer_data_map);
}


// Special-case APIs for which core_validation needs custom parameter lists and/or modifies parameters

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkGraphicsPipelineCreateInfo*         pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_graphics_pipeline_api_state cgpl_state[LayerObjectTypeMaxEnum]{};

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        cgpl_state[intercept->container_type].pCreateInfos = pCreateInfos;
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, &(cgpl_state[intercept->container_type]));
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, &(cgpl_state[intercept->container_type]));
    }

    auto usepCreateInfos = (!cgpl_state[LayerObjectTypeGpuAssisted].pCreateInfos) ? pCreateInfos : cgpl_state[LayerObjectTypeGpuAssisted].pCreateInfos;
    if (cgpl_state[LayerObjectTypeDebugPrintf].pCreateInfos) usepCreateInfos = cgpl_state[LayerObjectTypeDebugPrintf].pCreateInfos;

    VkResult result = DispatchCreateGraphicsPipelines(device, pipelineCache, createInfoCount, usepCreateInfos, pAllocator, pPipelines);

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result, &(cgpl_state[intercept->container_type]));
    }
    return result;
}

// This API saves some core_validation pipeline state state on the stack for performance purposes
VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkComputePipelineCreateInfo*          pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_compute_pipeline_api_state ccpl_state[LayerObjectTypeMaxEnum]{};

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        ccpl_state[intercept->container_type].pCreateInfos = pCreateInfos;
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, &(ccpl_state[intercept->container_type]));
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, &(ccpl_state[intercept->container_type]));
    }

    auto usepCreateInfos = (!ccpl_state[LayerObjectTypeGpuAssisted].pCreateInfos) ? pCreateInfos : ccpl_state[LayerObjectTypeGpuAssisted].pCreateInfos;
    if (ccpl_state[LayerObjectTypeDebugPrintf].pCreateInfos) usepCreateInfos = ccpl_state[LayerObjectTypeDebugPrintf].pCreateInfos;

    VkResult result = DispatchCreateComputePipelines(device, pipelineCache, createInfoCount, usepCreateInfos, pAllocator, pPipelines);

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result, &(ccpl_state[intercept->container_type]));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesNV(
    VkDevice                                    device,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkRayTracingPipelineCreateInfoNV*     pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_ray_tracing_pipeline_api_state crtpl_state[LayerObjectTypeMaxEnum]{};

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        crtpl_state[intercept->container_type].pCreateInfos = pCreateInfos;
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos,
                                                                      pAllocator, pPipelines, &(crtpl_state[intercept->container_type]));
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                            pPipelines, &(crtpl_state[intercept->container_type]));
    }

    VkResult result = DispatchCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                             pPipelines, result, &(crtpl_state[intercept->container_type]));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_ray_tracing_pipeline_khr_api_state crtpl_state[LayerObjectTypeMaxEnum]{};

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        crtpl_state[intercept->container_type].pCreateInfos = pCreateInfos;
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos,
                                                                       pAllocator, pPipelines, &(crtpl_state[intercept->container_type]));
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                            pPipelines, &(crtpl_state[intercept->container_type]));
    }

    VkResult result = DispatchCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
                                                             pPipelines, result, &(crtpl_state[intercept->container_type]));
    }
    return result;
}

// This API needs the ability to modify a down-chain parameter
VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(
    VkDevice                                    device,
    const VkPipelineLayoutCreateInfo*           pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkPipelineLayout*                           pPipelineLayout) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_pipeline_layout_api_state cpl_state{};
    cpl_state.modified_create_info = *pCreateInfo;

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout, &cpl_state);
    }
    VkResult result = DispatchCreatePipelineLayout(device, &cpl_state.modified_create_info, pAllocator, pPipelineLayout);
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout, result);
    }
    return result;
}

// This API needs some local stack data for performance reasons and also may modify a parameter
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
    VkDevice                                    device,
    const VkShaderModuleCreateInfo*             pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkShaderModule*                             pShaderModule) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_shader_module_api_state csm_state{};
    csm_state.instrumented_create_info = *pCreateInfo;

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule, &csm_state);
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule, &csm_state);
    }
    VkResult result = DispatchCreateShaderModule(device, &csm_state.instrumented_create_info, pAllocator, pShaderModule);
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule, result, &csm_state);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(
    VkDevice                                    device,
    const VkDescriptorSetAllocateInfo*          pAllocateInfo,
    VkDescriptorSet*                            pDescriptorSets) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    cvdescriptorset::AllocateDescriptorSetsData ads_state[LayerObjectTypeMaxEnum];

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        ads_state[intercept->container_type].Init(pAllocateInfo->descriptorSetCount);
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets, &(ads_state[intercept->container_type]));
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
    }
    VkResult result = DispatchAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets,
            result, &(ads_state[intercept->container_type]));
    }
    return result;
}

// This API needs the ability to modify a down-chain parameter
VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(
    VkDevice                                    device,
    const VkBufferCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkBuffer*                                   pBuffer) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    bool skip = false;

    create_buffer_api_state cb_state{};
    cb_state.modified_create_info = *pCreateInfo;

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordCreateBuffer(device, pCreateInfo, pAllocator, pBuffer, &cb_state);
    }
    VkResult result = DispatchCreateBuffer(device, &cb_state.modified_create_info, pAllocator, pBuffer);
    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordCreateBuffer(device, pCreateInfo, pAllocator, pBuffer, result);
    }
    return result;
}


// Handle tooling queries manually as this is a request for layer information

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceToolPropertiesEXT(
    VkPhysicalDevice                            physicalDevice,
    uint32_t*                                   pToolCount,
    VkPhysicalDeviceToolPropertiesEXT*          pToolProperties) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(physicalDevice), layer_data_map);
    bool skip = false;

    static const VkPhysicalDeviceToolPropertiesEXT khronos_layer_tool_props = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES_EXT,
        nullptr,
        "Khronos Validation Layer",
        STRINGIFY(VK_HEADER_VERSION),
        VK_TOOL_PURPOSE_VALIDATION_BIT_EXT | VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT_EXT | VK_TOOL_PURPOSE_DEBUG_REPORTING_BIT_EXT | VK_TOOL_PURPOSE_DEBUG_MARKERS_BIT_EXT,
        "Khronos Validation Layer",
        OBJECT_LAYER_NAME
    };

    auto original_pToolProperties = pToolProperties;


    if (pToolProperties != nullptr) {
        *pToolProperties = khronos_layer_tool_props;
        pToolProperties = ((*pToolCount > 1) ? &pToolProperties[1] : nullptr);
        (*pToolCount)--;
    }

    for (const ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->ReadLock();
        skip |= intercept->PreCallValidateGetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);
        if (skip) return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PreCallRecordGetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);
    }

    VkResult result = DispatchGetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties);

    if (original_pToolProperties != nullptr) {
        pToolProperties = original_pToolProperties;
    }
    (*pToolCount)++;

    for (ValidationObject* intercept : layer_data->object_dispatch) {
        auto lock = intercept->WriteLock();
        intercept->PostCallRecordGetPhysicalDeviceToolPropertiesEXT(physicalDevice, pToolCount, pToolProperties, result);
    }
    return result;
}


// ValidationCache APIs do not dispatch

VKAPI_ATTR VkResult VKAPI_CALL CreateValidationCacheEXT(
    VkDevice                                    device,
    const VkValidationCacheCreateInfoEXT*       pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkValidationCacheEXT*                       pValidationCache) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkResult result = VK_SUCCESS;

    ValidationObject *validation_data = layer_data->GetValidationObject(layer_data->object_dispatch, LayerObjectTypeCoreValidation);
    if (validation_data) {
        auto lock = validation_data->WriteLock();
        result = validation_data->CoreLayerCreateValidationCacheEXT(device, pCreateInfo, pAllocator, pValidationCache);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyValidationCacheEXT(
    VkDevice                                    device,
    VkValidationCacheEXT                        validationCache,
    const VkAllocationCallbacks*                pAllocator) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);

    ValidationObject *validation_data = layer_data->GetValidationObject(layer_data->object_dispatch, LayerObjectTypeCoreValidation);
    if (validation_data) {
        auto lock = validation_data->WriteLock();
        validation_data->CoreLayerDestroyValidationCacheEXT(device, validationCache, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL MergeValidationCachesEXT(
    VkDevice                                    device,
    VkValidationCacheEXT                        dstCache,
    uint32_t                                    srcCacheCount,
    const VkValidationCacheEXT*                 pSrcCaches) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkResult result = VK_SUCCESS;

    ValidationObject *validation_data = layer_data->GetValidationObject(layer_data->object_dispatch, LayerObjectTypeCoreValidation);
    if (validation_data) {
        auto lock = validation_data->WriteLock();
        result = validation_data->CoreLayerMergeValidationCachesEXT(device, dstCache, srcCacheCount, pSrcCaches);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetValidationCacheDataEXT(
    VkDevice                                    device,
    VkValidationCacheEXT                        validationCache,
    size_t*                                     pDataSize,
    void*                                       pData) {
    auto layer_data = GetLayerDataPtr(get_dispatch_key(device), layer_data_map);
    VkResult result = VK_SUCCESS;

    ValidationObject *validation_data = layer_data->GetValidationObject(layer_data->object_dispatch, LayerObjectTypeCoreValidation);
    if (validation_data) {
        auto lock = validation_data->WriteLock();
        result = validation_data->CoreLayerGetValidationCacheDataEXT(device, validationCache, pDataSize, pData);
    }
    return result;

}"""
    extension_warn_function = """
static void DeviceExtensionWarnlist(ValidationObject *layer_data, const VkDeviceCreateInfo *pCreateInfo, VkDevice device) {
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        // Check for recognized device extensions
        if (white_list(pCreateInfo->ppEnabledExtensionNames[i], kDeviceWarnExtensionNames)) {
            layer_data->LogWarning(layer_data->device, kVUIDUndefined,
                    "Device Extension %s validation support is incomplete, incorrect results are possible.",
                    pCreateInfo->ppEnabledExtensionNames[i]);
        }
    }
}
"""

    inline_custom_validation_class_definitions = """
        virtual VkResult CoreLayerCreateValidationCacheEXT(VkDevice device, const VkValidationCacheCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkValidationCacheEXT* pValidationCache) { return VK_SUCCESS; };
        virtual void CoreLayerDestroyValidationCacheEXT(VkDevice device, VkValidationCacheEXT validationCache, const VkAllocationCallbacks* pAllocator) {};
        virtual VkResult CoreLayerMergeValidationCachesEXT(VkDevice device, VkValidationCacheEXT dstCache, uint32_t srcCacheCount, const VkValidationCacheEXT* pSrcCaches)  { return VK_SUCCESS; };
        virtual VkResult CoreLayerGetValidationCacheDataEXT(VkDevice device, VkValidationCacheEXT validationCache, size_t* pDataSize, void* pData)  { return VK_SUCCESS; };

        // Allow additional state parameter for CreateGraphicsPipelines
        virtual bool PreCallValidateCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* cgpl_state) const {
            return PreCallValidateCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PreCallRecordCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* cgpl_state) {
            PreCallRecordCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PostCallRecordCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, VkResult result, void* cgpl_state) {
            PostCallRecordCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result);
        };

        // Allow additional state parameter for CreateComputePipelines
        virtual bool PreCallValidateCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* pipe_state) const {
            return PreCallValidateCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PreCallRecordCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* ccpl_state) {
            PreCallRecordCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PostCallRecordCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, VkResult result, void* pipe_state) {
            PostCallRecordCreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result);
        };

        // Allow additional state parameter for CreateRayTracingPipelinesNV
        virtual bool PreCallValidateCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoNV* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* pipe_state) const {
            return PreCallValidateCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PreCallRecordCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoNV* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* ccpl_state) {
            PreCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PostCallRecordCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoNV* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, VkResult result, void* pipe_state) {
            PostCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result);
        };

        // Allow additional state parameter for CreateRayTracingPipelinesKHR
        virtual bool PreCallValidateCreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* pipe_state) const {
            return PreCallValidateCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PreCallRecordCreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, void* ccpl_state) {
            PreCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
        };
        virtual void PostCallRecordCreateRayTracingPipelinesKHR(VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkRayTracingPipelineCreateInfoKHR* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines, VkResult result, void* pipe_state) {
            PostCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines, result);
        };

        // Allow modification of a down-chain parameter for CreatePipelineLayout
        virtual void PreCallRecordCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout, void *cpl_state) {
            PreCallRecordCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
        };

        // Enable the CreateShaderModule API to take an extra argument for state preservation and paramter modification
        virtual bool PreCallValidateCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule, void* csm_state) const {
            return PreCallValidateCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
        };
        virtual void PreCallRecordCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule, void* csm_state) {
            PreCallRecordCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
        };
        virtual void PostCallRecordCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule, VkResult result, void* csm_state) {
            PostCallRecordCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule, result);
        };

        // Allow AllocateDescriptorSets to use some local stack storage for performance purposes
        virtual bool PreCallValidateAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets, void* ads_state) const {
            return PreCallValidateAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets);
        };
        virtual void PostCallRecordAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets, VkResult result, void* ads_state)  {
            PostCallRecordAllocateDescriptorSets(device, pAllocateInfo, pDescriptorSets, result);
        };

        // Allow modification of a down-chain parameter for CreateBuffer
        virtual void PreCallRecordCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer, void *cb_state) {
            PreCallRecordCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
        };

        // Modify a parameter to CreateDevice
        virtual void PreCallRecordCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice, void *modified_create_info) {
            PreCallRecordCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        };

        template <typename T>
        std::vector<T> ValidParamValues() const;
"""

    inline_custom_source_postamble = """

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char *funcName) {
    return vulkan_layer_chassis::GetPhysicalDeviceProcAddr(instance, funcName);
}

#if defined(__GNUC__) && __GNUC__ >= 4
#define VVL_EXPORT __attribute__((visibility("default")))
#else
#define VVL_EXPORT
#endif

// The following functions need to match the `/DEF` and `--version-script` files
// for consistency across platforms that don't accept those linker options.
extern "C" {

VVL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return vulkan_layer_chassis::GetInstanceProcAddr(instance, funcName);
}

VVL_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    return vulkan_layer_chassis::GetDeviceProcAddr(dev, funcName);
}

VVL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties) {
    return vulkan_layer_chassis::EnumerateInstanceLayerProperties(pCount, pProperties);
}

VVL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProperties) {
    return vulkan_layer_chassis::EnumerateInstanceExtensionProperties(pLayerName, pCount, pProperties);
}

VVL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    assert(pVersionStruct != nullptr);
    assert(pVersionStruct->sType == LAYER_NEGOTIATE_INTERFACE_STRUCT);

    // Fill in the function pointers if our version is at least capable of having the structure contain them.
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = vk_layerGetPhysicalDeviceProcAddr;
    }

    return VK_SUCCESS;
}

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
VVL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount, VkLayerProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice == VK_NULL_HANDLE);
    return vulkan_layer_chassis::EnumerateDeviceLayerProperties(VK_NULL_HANDLE, pCount, pProperties);
}

VVL_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice == VK_NULL_HANDLE);
    return vulkan_layer_chassis::EnumerateDeviceExtensionProperties(VK_NULL_HANDLE, pLayerName, pCount, pProperties);
}
#endif

}  // extern "C"
"""

    vk_init_object_dispatch_vector = """
#define BUILD_DISPATCH_VECTOR(name) \\
    init_object_dispatch_vector(InterceptId ## name, \\
                                typeid(&ValidationObject::name), \\
                                typeid(&ThreadSafety::name), \\
                                typeid(&StatelessValidation::name), \\
                                typeid(&ObjectLifetimes::name), \\
                                typeid(&CoreChecks::name), \\
                                typeid(&BestPractices::name), \\
                                typeid(&GpuAssisted::name), \\
                                typeid(&DebugPrintf::name), \\
                                typeid(&SyncValidator::name));

    auto init_object_dispatch_vector = [this](InterceptId id,
                                              const std::type_info& vo_typeid,
                                              const std::type_info& tt_typeid,
                                              const std::type_info& tpv_typeid,
                                              const std::type_info& tot_typeid,
                                              const std::type_info& tcv_typeid,
                                              const std::type_info& tbp_typeid,
                                              const std::type_info& tga_typeid,
                                              const std::type_info& tdp_typeid,
                                              const std::type_info& tsv_typeid) {
        for (auto item : this->object_dispatch) {
            auto intercept_vector = &this->intercept_vectors[id];
            switch (item->container_type) {
            case LayerObjectTypeThreading:
                if (tt_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeParameterValidation:
                if (tpv_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeObjectTracker:
                if (tot_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeCoreValidation:
                if (tcv_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeBestPractices:
                if (tbp_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeGpuAssisted:
                if (tga_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeDebugPrintf:
                if (tdp_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeSyncValidation:
                if (tsv_typeid != vo_typeid) intercept_vector->push_back(item);
                break;
            case LayerObjectTypeInstance:
            case LayerObjectTypeDevice:
                break;
            default:
                /* Chassis codegen needs to be updated for unknown validation object type */
                assert(0);
            }
        }
    };"""

    def __init__(self,
                 errFile = sys.stderr,
                 warnFile = sys.stderr,
                 diagFile = sys.stdout):
        OutputGenerator.__init__(self, errFile, warnFile, diagFile)
        # Internal state - accumulators for different inner block text
        self.sections = dict([(section, []) for section in self.ALL_SECTIONS])
        # We need to manually add an entry for vk_layerGetPhysicalDeviceProcAddr because it isn't in the xml,
        # but it must be queryable from vkGetInstanceProcAddr()
        self.intercepts = [ '    {"%s", {%s, (void*)%s}},' % ("vk_layerGetPhysicalDeviceProcAddr", "kFuncTypeInst", "GetPhysicalDeviceProcAddr") ]
        self.intercept_enums = ''
        self.dispatch_vector_fcns = ''
        self.virtual_fcn_defs = ''

    # Check if the parameter passed in is a pointer to an array
    def paramIsArray(self, param):
        return param.attrib.get('len') is not None

    # Check if the parameter passed in is a pointer
    def paramIsPointer(self, param):
        ispointer = False
        for elem in param:
            if elem.tag == 'type' and elem.tail is not None and '*' in elem.tail:
                ispointer = True
        return ispointer

    #
    # Get layer data list
    def getLayerList(self):
        return self.vk_validation_layers

    #
    # Get instance extension list
    def getInstanceExtensions(self):
        return self.vk_instance_extensions

    #
    # Get device extension list
    def getDeviceExtensions(self):
        return self.vk_device_extensions

    #
    # Generate chassis source includes
    def genChassisSourceIncludes(self):
        includes = []
        includes.append('// Include layer validation object definitions')
        # Add #include directives for the used layers
        for layer in self.getLayerList():
            includes.append('#include "{}"'.format(layer['include']))
        includes.append('')
        includes.append('// This header file must be included after the above validation object class definitions')
        includes.append('#include "chassis_dispatch_helper.h"')
        includes.append('')
        return '\n'.join(includes)

    #
    # Generate extension lists exposed by the validation layer
    def genExtensionLists(self):
        extensions = []
        extensions.append('static const VkExtensionProperties instance_extensions[] = {')
        for instance_extension in self.getInstanceExtensions():
            extensions.append('    {{{0}_EXTENSION_NAME, {0}_SPEC_VERSION}},'.format(instance_extension.upper()))
        extensions.append('};')
        extensions.append('')
        extensions.append('static const VkExtensionProperties device_extensions[] = {')
        for device_extension in self.getDeviceExtensions():
            extensions.append('    {{{0}_EXTENSION_NAME, {0}_SPEC_VERSION}},'.format(device_extension.upper()))
        extensions.append('};')
        extensions.append('')
        return '\n'.join(extensions)

    #
    # Generate init object dispatch vector
    def genInitObjectDispatchVector(self):
        return self.vk_init_object_dispatch_vector

    #
    # Generate layer registration code
    def genRegisterLayers(self):
        contents = []

        contents.append(
            'static std::vector<ValidationObject*> CreateObjectDispatch(const CHECK_ENABLED &enables, const CHECK_DISABLED &disables) {\n' +
            '    std::vector<ValidationObject*> object_dispatch{};\n\n'
            '    // Add VOs to dispatch vector. Order here will be the validation dispatch order!')
        for layer in self.getLayerList():
            constructor = layer['class']
            if layer['class'] == 'ThreadSafety':
                constructor += '(nullptr)'
            contents.append(('    if ({0}) {{\n' +
                '        object_dispatch.emplace_back(new {1});\n' +
                '    }}').format(layer['enabled'], constructor))
        contents.append('    return object_dispatch;')
        contents.append('}\n')

        contents.append(
            'static void InitDeviceObjectDispatch(ValidationObject *instance_interceptor, ValidationObject *device_interceptor) {\n' +
            '    auto disables = instance_interceptor->disabled;\n' +
            '    auto enables = instance_interceptor->enabled;\n\n' +
            '    // Note that this DEFINES THE ORDER IN WHICH THE LAYER VALIDATION OBJECTS ARE CALLED')
        for layer in self.getLayerList():
            constructor = layer['class']
            if layer['class'] == 'ThreadSafety':
                constructor += ('(static_cast<ThreadSafety *>(\n' +
                    '            instance_interceptor->GetValidationObject(instance_interceptor->object_dispatch, LayerObjectTypeThreading)))')
            contents.append(('    if ({0}) {{\n' +
                '        device_interceptor->object_dispatch.emplace_back(new {1});\n' +
                '    }}').format(layer['enabled'], constructor))
        contents.append('}')

        return '\n'.join(contents)

    #
    #
    def beginFile(self, genOpts):
        OutputGenerator.beginFile(self, genOpts)
        # Output Copyright
        write(self.inline_copyright_message, file=self.outFile)
        # Multiple inclusion protection
        self.chassis_header = False
        self.helper_header = False
        self.chassis_source = False
        if ('layer_chassis_header' == self.genOpts.helper_file_type):
            self.chassis_header = True
            write('#pragma once', file=self.outFile)
            self.newline()
            write(self.inline_custom_header_preamble, file=self.outFile)
        elif ('layer_chassis_helper_header' == self.genOpts.helper_file_type):
            self.helper_header = True
            write('#pragma once', file=self.outFile)
        else:
            self.chassis_source = True
            write(self.inline_custom_source_preamble_1, file=self.outFile)
            write(self.genChassisSourceIncludes(), file=self.outFile)
            write(self.genExtensionLists(), file=self.outFile)
            write(self.genRegisterLayers(), file=self.outFile)
            write(self.inline_custom_source_preamble_2, file=self.outFile)
            write("static const std::set<std::string> kDeviceWarnExtensionNames {", file=self.outFile)
            for ext in genOpts.warnExtensions:
                write('    "{}",'.format(ext), file=self.outFile)
            write("};", file=self.outFile)
            write(self.extension_warn_function, file=self.outFile)

    #
    #
    def endFile(self):
        # Finish C++ namespace and multiple inclusion protection
        self.newline()
        if self.chassis_source:
            # Record intercepted procedures
            write('// Map of intercepted ApiName to its associated function data', file=self.outFile)
            write('#ifdef _MSC_VER', file=self.outFile)
            write('#pragma warning( suppress: 6262 ) // VS analysis: this uses more than 16 kiB, which is fine here at global scope', file=self.outFile)
            write('#endif', file=self.outFile)
            write('const vvl::unordered_map<std::string, function_data> name_to_funcptr_map = {', file=self.outFile)
            write('\n'.join(self.intercepts), file=self.outFile)
            write('};\n', file=self.outFile)
            self.newline()
            write('} // namespace vulkan_layer_chassis', file=self.outFile)
            write(self.inline_custom_source_postamble, file=self.outFile)
        elif self.chassis_header:
            self.newline()
            chassis_hdr_content = ''
            chassis_hdr_content += self.inline_custom_header_class_definition
            chassis_hdr_content += self.virtual_fcn_defs
            chassis_hdr_content += self.inline_custom_validation_class_definitions
            chassis_hdr_content += '};\n\n'
            chassis_hdr_content += 'extern small_unordered_map<void*, ValidationObject*, 2> layer_data_map;'
            chassis_hdr_content += f'\n#include "{ParameterValidationOutputGenerator.VALID_PARAM_VALUES_PATH}"'
            write(chassis_hdr_content, file=self.outFile)
        elif self.helper_header:
            self.newline()
            helper_content =  '// This source code creates dispatch vectors for each chassis api intercept,\n'
            helper_content += '// i.e., PreCallValidateFoo, PreCallRecordFoo, PostCallRecordFoo, etc., ensuring that \n'
            helper_content += '// each vector contains only the validation objects that override that particular base \n'
            helper_content += '// class virtual function. Preventing non-overridden calls from reaching the default\n'
            helper_content += '// functions saved about 5% in multithreaded applications.\n\n'
            helper_content += 'typedef enum InterceptId{\n'
            helper_content += self.intercept_enums
            helper_content += '    InterceptIdCount,\n'
            helper_content += '} InterceptId;\n\n'
            helper_content += 'void ValidationObject::InitObjectDispatchVectors() {\n'
            helper_content += self.genInitObjectDispatchVector()
            helper_content += '\n\n'
            helper_content += '    intercept_vectors.resize(InterceptIdCount);\n\n'
            helper_content += self.dispatch_vector_fcns;
            helper_content += '};\n'
            write(helper_content, file=self.outFile)

        # Finish processing in superclass
        OutputGenerator.endFile(self)

    def beginFeature(self, interface, emit):
        # Start processing in superclass
        OutputGenerator.beginFeature(self, interface, emit)
        # Get feature extra protect
        self.featureExtraProtect = GetFeatureProtect(interface)
        # Accumulate includes, defines, types, enums, function pointer typedefs, end function prototypes separately for this
        # feature. They're only printed in endFeature().
        self.sections = dict([(section, []) for section in self.ALL_SECTIONS])

    def endFeature(self):
        # Actually write the interface to the output file.
        if not self.helper_header:
            if (self.emit):
                self.newline()
                # If type declarations are needed by other features based on this one, it may be necessary to suppress the ExtraProtect,
                # or move it below the 'for section...' loop.
                if (self.featureExtraProtect != None):
                    write('#ifdef', self.featureExtraProtect, file=self.outFile)
                for section in self.TYPE_SECTIONS:
                    contents = self.sections[section]
                    if contents:
                        write('\n'.join(contents), file=self.outFile)
                        self.newline()
                if (self.sections['command']):
                    write('\n'.join(self.sections['command']), end=u'', file=self.outFile)
                    self.newline()
                if (self.featureExtraProtect != None):
                    write('#endif //', self.featureExtraProtect, file=self.outFile)
        # Finish processing in superclass
        OutputGenerator.endFeature(self)
    #
    # Append a definition to the specified section
    def appendSection(self, section, text):
        self.sections[section].append(text)
    #
    # Type generation
    def genType(self, typeinfo, name, alias):
        pass
    #
    # Struct (e.g. C "struct" type) generation. This is a special case of the <type> tag where the contents are
    # interpreted as a set of <member> tags instead of freeform C type declarations. The <member> tags are just like <param>
    # tags - they are a declaration of a struct or union member. Only simple member declarations are supported (no nested
    # structs etc.)
    def genStruct(self, typeinfo, typeName):
        OutputGenerator.genStruct(self, typeinfo, typeName)
        body = 'typedef ' + typeinfo.elem.get('category') + ' ' + typeName + ' {\n'
        # paramdecl = self.makeCParamDecl(typeinfo.elem, self.genOpts.alignFuncParam)
        for member in typeinfo.elem.findall('.//member'):
            body += self.makeCParamDecl(member, self.genOpts.alignFuncParam)
            body += ';\n'
        body += '} ' + typeName + ';\n'
        self.appendSection('struct', body)
    #
    # Group (e.g. C "enum" type) generation. These are concatenated together with other types.
    def genGroup(self, groupinfo, groupName, alias):
        pass
    # Enumerant generation
    # <enum> tags may specify their values in several ways, but are usually just integers.
    def genEnum(self, enuminfo, name, alias):
        pass
    #
    # Customize Cdecl for layer factory base class
    def BaseClassCdecl(self, elem, name):
        raw = self.makeCDecls(elem)[1]

        # Toss everything before the undecorated name
        prototype = raw.split("VKAPI_PTR *PFN_vk")[1]
        prototype = prototype.replace(")", "", 1)
        prototype = prototype.replace(";", " {};")

        # Build up pre/post call virtual function declarations
        pre_call_validate = 'virtual bool PreCallValidate' + prototype
        pre_call_validate = pre_call_validate.replace("{}", "const { return false; }")
        pre_call_record = 'virtual void PreCallRecord' + prototype
        post_call_record = 'virtual void PostCallRecord' + prototype
        resulttype = elem.find('proto/type')
        if resulttype.text == 'VkResult':
            post_call_record = post_call_record.replace(')', ', VkResult result)')
        elif resulttype.text == 'VkDeviceAddress':
            post_call_record = post_call_record.replace(')', ', VkDeviceAddress result)')
        return '        %s\n        %s\n        %s\n' % (pre_call_validate, pre_call_record, post_call_record)
    #
    # Command generation
    def genCmd(self, cmdinfo, name, alias):
        ignore_functions = [
            'vkEnumerateInstanceVersion',
            ]

        if name in ignore_functions:
            return

        dispatchable_type = cmdinfo.elem.find('param/type').text

        if self.chassis_header: # In the header declare all intercepts
            self.appendSection('command', '')
            self.appendSection('command', self.makeCDecls(cmdinfo.elem)[0])
            if (self.featureExtraProtect != None):
                self.virtual_fcn_defs += '#ifdef %s\n' % self.featureExtraProtect
            # Update base class with virtual function declarations
            if 'ValidationCache' not in name:
                self.virtual_fcn_defs += self.BaseClassCdecl(cmdinfo.elem, name)
            if (self.featureExtraProtect != None):
                self.virtual_fcn_defs += '#endif\n'
        elif self.helper_header:
            if (self.featureExtraProtect != None):
                self.dispatch_vector_fcns += '#ifdef %s\n' % self.featureExtraProtect
            if name not in self.manual_functions and dispatchable_type != 'VkInstance' and dispatchable_type != 'VkPhysicalDevice':
                fcn_name = name[2:]
                self.intercept_enums += '    InterceptIdPreCallValidate%s,\n' % fcn_name
                self.intercept_enums += '    InterceptIdPreCallRecord%s,\n' % fcn_name
                self.intercept_enums += '    InterceptIdPostCallRecord%s,\n' % fcn_name

                for prefix in ['PreCallValidate', 'PreCallRecord', 'PostCallRecord']:
                    self.dispatch_vector_fcns += '    BUILD_DISPATCH_VECTOR(%s%s);\n' % (prefix, name[2:])
            if (self.featureExtraProtect != None):
                self.dispatch_vector_fcns += '#endif\n'
        elif self.chassis_source:
            special_case_instance_APIs = [
                'vkCreateInstance',
                'vkEnumerateInstanceVersion',
                'vkEnumerateInstanceLayerProperties',
                'vkEnumerateInstanceExtensionProperties',
                ]
            if dispatchable_type == 'VkInstance' or name in special_case_instance_APIs:
                function_type = 'kFuncTypeInst'
            elif dispatchable_type == 'VkPhysicalDevice':
                function_type = 'kFuncTypePdev'
            else:
                function_type = 'kFuncTypeDev'

            if name in self.manual_functions:
                if (self.featureExtraProtect != None):
                    self.intercepts += [ '#ifdef %s' % self.featureExtraProtect ]
                self.intercepts += [ '    {"%s", {%s, (void*)%s}},' % (name, function_type, name[2:]) ]
                if (self.featureExtraProtect != None):
                    self.intercepts += [ '#endif' ]
                return
            # Record that the function will be intercepted
            if (self.featureExtraProtect != None):
                self.intercepts += [ '#ifdef %s' % self.featureExtraProtect ]
            self.intercepts += [ '    {"%s", {%s, (void*)%s}},' % (name, function_type, name[2:]) ]
            if (self.featureExtraProtect != None):
                self.intercepts += [ '#endif' ]
            OutputGenerator.genCmd(self, cmdinfo, name, alias)
            #
            decls = self.makeCDecls(cmdinfo.elem)
            self.appendSection('command', '')
            self.appendSection('command', '%s {' % decls[0][:-1])
            # Setup common to call wrappers. First parameter is always dispatchable
            dispatchable_name = cmdinfo.elem.find('param/name').text
            self.appendSection('command', '    auto layer_data = GetLayerDataPtr(get_dispatch_key(%s), layer_data_map);' % (dispatchable_name))
            api_function_name = cmdinfo.elem.attrib.get('name')
            params = cmdinfo.elem.findall('param/name')
            paramstext = ', '.join([str(param.text) for param in params])
            API = api_function_name.replace('vk','Dispatch') + '('

            # Declare result variable, if any.
            return_map = {
                'PFN_vkVoidFunction': 'return nullptr;',
                'VkBool32': 'return VK_FALSE;',
                'VkDeviceAddress': 'return 0;',
                'VkDeviceSize': 'return 0;',
                'VkResult': 'return VK_ERROR_VALIDATION_FAILED_EXT;',
                'void': 'return;',
                'uint32_t': 'return 0;',
                'uint64_t': 'return 0;'
                }
            resulttype = cmdinfo.elem.find('proto/type')
            assignresult = ''
            if (resulttype.text != 'void'):
                assignresult = resulttype.text + ' result = '

            # Set up skip and locking
            self.appendSection('command', '    bool skip = false;')

            # Generate pre-call validation source code
            if dispatchable_type != 'VkInstance' and dispatchable_type != 'VkPhysicalDevice':
                self.appendSection('command', '    for (const ValidationObject* intercept : layer_data->intercept_vectors[InterceptIdPreCallValidate%s]) {' % api_function_name[2:])
            else:
                self.appendSection('command', '    for (const ValidationObject* intercept : layer_data->object_dispatch) {')
            self.appendSection('command', '        auto lock = intercept->ReadLock();')
            self.appendSection('command', '        skip |= intercept->PreCallValidate%s(%s);' % (api_function_name[2:], paramstext))
            self.appendSection('command', '        if (skip) %s' % return_map[resulttype.text])
            self.appendSection('command', '    }')

            # Generate pre-call state recording source code
            if dispatchable_type != 'VkInstance' and dispatchable_type != 'VkPhysicalDevice':
                self.appendSection('command', '    for (ValidationObject* intercept : layer_data->intercept_vectors[InterceptIdPreCallRecord%s]) {' % api_function_name[2:])
            else:
                self.appendSection('command', '    for (ValidationObject* intercept : layer_data->object_dispatch) {')
            self.appendSection('command', '        auto lock = intercept->WriteLock();')
            self.appendSection('command', '        intercept->PreCallRecord%s(%s);' % (api_function_name[2:], paramstext))
            self.appendSection('command', '    }')

            # Insert pre-dispatch debug utils function call
            if name in self.pre_dispatch_debug_utils_functions:
                self.appendSection('command', '    %s' % self.pre_dispatch_debug_utils_functions[name])

            # Output dispatch (down-chain) function call
            self.appendSection('command', '    ' + assignresult + API + paramstext + ');')

            # Insert post-dispatch debug utils function call
            if name in self.post_dispatch_debug_utils_functions:
                self.appendSection('command', '    %s' % self.post_dispatch_debug_utils_functions[name])

            # Generate post-call object processing source code
            if dispatchable_type != 'VkInstance' and dispatchable_type != 'VkPhysicalDevice':
                self.appendSection('command', '    for (ValidationObject* intercept : layer_data->intercept_vectors[InterceptIdPostCallRecord%s]) {' % api_function_name[2:])
            else:
                self.appendSection('command', '    for (ValidationObject* intercept : layer_data->object_dispatch) {')
            returnparam = ''
            if (resulttype.text == 'VkResult' or resulttype.text == 'VkDeviceAddress'):
                returnparam = ', result'
            self.appendSection('command', '        auto lock = intercept->WriteLock();')
            self.appendSection('command', '        intercept->PostCallRecord%s(%s%s);' % (api_function_name[2:], paramstext, returnparam))
            self.appendSection('command', '    }')
            # Return result variable, if any.
            if (resulttype.text != 'void'):
                self.appendSection('command', '    return result;')
            self.appendSection('command', '}')
    #
    # Override makeProtoName to drop the "vk" prefix
    def makeProtoName(self, name, tail):
        return self.genOpts.apientry + name[2:] + tail
