/*
 * Copyright (c) 2023 Valve Corporation
 * Copyright (c) 2023 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 */

#include "../framework/layer_validation_tests.h"

class NegativeDeviceQueue : public VkLayerTest {};

TEST_F(NegativeDeviceQueue, FamilyIndex) {
    TEST_DESCRIPTION("Create device queue with invalid queue family index.");

    ASSERT_NO_FATAL_FAILURE(InitFramework());

    uint32_t queue_family_count;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_props(queue_family_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, queue_props.data());

    uint32_t queue_family_index = queue_family_count;

    float priority = 1.0f;
    auto device_queue_ci = LvlInitStruct<VkDeviceQueueCreateInfo>();
    device_queue_ci.queueFamilyIndex = queue_family_index;
    device_queue_ci.queueCount = 1;
    device_queue_ci.pQueuePriorities = &priority;

    auto device_ci = LvlInitStruct<VkDeviceCreateInfo>();
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &device_queue_ci;
    device_ci.enabledLayerCount = 0;
    device_ci.enabledExtensionCount = m_device_extension_names.size();
    device_ci.ppEnabledExtensionNames = m_device_extension_names.data();

    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceQueueCreateInfo-queueFamilyIndex-00381");
    VkDevice device;
    vk::CreateDevice(gpu(), &device_ci, nullptr, &device);
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeDeviceQueue, FamilyIndexUsage) {
    TEST_DESCRIPTION("Using device queue with invalid queue family index.");
    bool get_physical_device_properties2 = InstanceExtensionSupported(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (get_physical_device_properties2) {
        m_instance_extension_names.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    ASSERT_NO_FATAL_FAILURE(Init());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());
    VkBufferCreateInfo buffCI = LvlInitStruct<VkBufferCreateInfo>();
    buffCI.size = 1024;
    buffCI.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffCI.queueFamilyIndexCount = 2;
    // Introduce failure by specifying invalid queue_family_index
    uint32_t qfi[2];
    qfi[0] = 777;
    qfi[1] = 0;

    buffCI.pQueueFamilyIndices = qfi;
    buffCI.sharingMode = VK_SHARING_MODE_CONCURRENT;  // qfi only matters in CONCURRENT mode

    const char *vuid = (get_physical_device_properties2) ? "VUID-VkBufferCreateInfo-sharingMode-01419"
                                                         : "VUID-VkBufferCreateInfo-sharingMode-01391";
    // Test for queue family index out of range
    CreateBufferTest(*this, &buffCI, vuid);

    // Test for non-unique QFI in array
    qfi[0] = 0;
    CreateBufferTest(*this, &buffCI, vuid);

    if (m_device->queue_props.size() > 2) {
        m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-vkQueueSubmit-pSubmits-04626");

        // Create buffer shared to queue families 1 and 2, but submitted on queue family 0
        buffCI.queueFamilyIndexCount = 2;
        qfi[0] = 1;
        qfi[1] = 2;
        VkBufferObj ib;
        ib.init(*m_device, buffCI);

        m_commandBuffer->begin();
        vk::CmdFillBuffer(m_commandBuffer->handle(), ib.handle(), 0, 16, 5);
        m_commandBuffer->end();
        m_commandBuffer->QueueCommandBuffer(false);
        m_errorMonitor->VerifyFound();
    }

    // If there is more than one queue family, create a device with a single queue family, then create a buffer
    // with SHARING_MODE_CONCURRENT that uses a non-device PDEV queue family.
    uint32_t queue_count;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_count, NULL);
    std::vector<VkQueueFamilyProperties> queue_props;
    queue_props.resize(queue_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_count, queue_props.data());

    if (queue_count < 3) {
        GTEST_SKIP() << "Multiple queue families are required to run this test.";
    }
    std::vector<float> priorities(queue_props.at(0).queueCount, 1.0f);
    VkDeviceQueueCreateInfo queue_info = LvlInitStruct<VkDeviceQueueCreateInfo>();
    queue_info.queueFamilyIndex = 0;
    queue_info.queueCount = queue_props.at(0).queueCount;
    queue_info.pQueuePriorities = priorities.data();
    VkDeviceCreateInfo dev_info = LvlInitStruct<VkDeviceCreateInfo>();
    dev_info.queueCreateInfoCount = 1;
    dev_info.pQueueCreateInfos = &queue_info;
    dev_info.enabledLayerCount = 0;
    dev_info.enabledExtensionCount = m_device_extension_names.size();
    dev_info.ppEnabledExtensionNames = m_device_extension_names.data();

    // Create a device with a single queue family
    VkDevice second_device;
    ASSERT_VK_SUCCESS(vk::CreateDevice(gpu(), &dev_info, nullptr, &second_device));

    // Select Queue family for CONCURRENT buffer that is not owned by device
    buffCI.queueFamilyIndexCount = 2;
    qfi[1] = 2;
    VkBuffer buffer = VK_NULL_HANDLE;
    vk::CreateBuffer(second_device, &buffCI, NULL, &buffer);
    vk::DestroyBuffer(second_device, buffer, nullptr);
    vk::DestroyDevice(second_device, nullptr);
}

TEST_F(NegativeDeviceQueue, FamilyIndexUnique) {
    TEST_DESCRIPTION("Vulkan 1.0 unique queue detection");
    SetTargetApiVersion(VK_API_VERSION_1_0);

    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));

    // use first queue family with at least 2 queues in it
    bool found_queue = false;
    VkQueueFamilyProperties queue_properties;  // selected queue family used
    uint32_t queue_family_index = 0;
    uint32_t queue_family_count = 0;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, queue_families.data());

    for (size_t i = 0; i < queue_families.size(); i++) {
        if (queue_families[i].queueCount > 1) {
            found_queue = true;
            queue_family_index = i;
            queue_properties = queue_families[i];
            break;
        }
    }

    if (found_queue == false) {
        GTEST_SKIP() << "test requires queue family with 2 queues, not available";
    }

    float queue_priority = 1.0;
    VkDeviceQueueCreateInfo queue_create_info[2];
    queue_create_info[0] = LvlInitStruct<VkDeviceQueueCreateInfo>();
    queue_create_info[0].flags = 0;
    queue_create_info[0].queueFamilyIndex = queue_family_index;
    queue_create_info[0].queueCount = 1;
    queue_create_info[0].pQueuePriorities = &queue_priority;

    // queueFamilyIndex is the same
    queue_create_info[1] = queue_create_info[0];

    VkDevice test_device = VK_NULL_HANDLE;
    VkDeviceCreateInfo device_create_info = LvlInitStruct<VkDeviceCreateInfo>();
    device_create_info.flags = 0;
    device_create_info.pQueueCreateInfos = queue_create_info;
    device_create_info.queueCreateInfoCount = 2;
    device_create_info.pEnabledFeatures = nullptr;
    device_create_info.enabledLayerCount = 0;
    device_create_info.enabledExtensionCount = 0;

    const char *vuid = (DeviceValidationVersion() == VK_API_VERSION_1_0) ? "VUID-VkDeviceCreateInfo-queueFamilyIndex-00372"
                                                                         : "VUID-VkDeviceCreateInfo-queueFamilyIndex-02802";
    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, vuid);
    vk::CreateDevice(gpu(), &device_create_info, nullptr, &test_device);
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeDeviceQueue, MismatchedGlobalPriority) {
    TEST_DESCRIPTION("Create multiple device queues with same queue family index but different global priorty.");

    SetTargetApiVersion(VK_API_VERSION_1_1);
    AddRequiredExtensions(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);

    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));

    if (DeviceValidationVersion() < VK_API_VERSION_1_1) {
        GTEST_SKIP() << "At least Vulkan version 1.1 is required";
    }
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }

    uint32_t queue_family_count;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_props(queue_family_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, queue_props.data());

    uint32_t queue_family_index = queue_family_count;

    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_props[i].queueCount > 1) {
            queue_family_index = i;
            break;
        }
    }
    if (queue_family_index == queue_family_count) {
        GTEST_SKIP() << "Multiple queues from same queue family are required to run this test";
    }

    VkDeviceQueueGlobalPriorityCreateInfoKHR queue_global_priority_ci[2] = {};
    queue_global_priority_ci[0] = LvlInitStruct<VkDeviceQueueGlobalPriorityCreateInfoKHR>();
    queue_global_priority_ci[0].globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
    queue_global_priority_ci[1] = LvlInitStruct<VkDeviceQueueGlobalPriorityCreateInfoKHR>();
    queue_global_priority_ci[1].globalPriority = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;

    float priorities[] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo device_queue_ci[2] = {};
    device_queue_ci[0] = LvlInitStruct<VkDeviceQueueCreateInfo>(&queue_global_priority_ci[0]);
    device_queue_ci[0].queueFamilyIndex = queue_family_index;
    device_queue_ci[0].queueCount = 1;
    device_queue_ci[0].pQueuePriorities = &priorities[0];

    device_queue_ci[1] = LvlInitStruct<VkDeviceQueueCreateInfo>(&queue_global_priority_ci[1]);
    device_queue_ci[1].queueFamilyIndex = queue_family_index;
    device_queue_ci[1].queueCount = 1;
    device_queue_ci[1].pQueuePriorities = &priorities[1];

    auto device_ci = LvlInitStruct<VkDeviceCreateInfo>();
    device_ci.queueCreateInfoCount = 2;
    device_ci.pQueueCreateInfos = device_queue_ci;
    device_ci.enabledLayerCount = 0;
    device_ci.enabledExtensionCount = m_device_extension_names.size();
    device_ci.ppEnabledExtensionNames = m_device_extension_names.data();

    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceCreateInfo-queueFamilyIndex-02802");
    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceCreateInfo-pQueueCreateInfos-06654");
    VkDevice device;
    vk::CreateDevice(gpu(), &device_ci, nullptr, &device);
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeDeviceQueue, QueueCount) {
    TEST_DESCRIPTION("Create device queue with too high of a queueCount.");

    ASSERT_NO_FATAL_FAILURE(InitFramework());

    uint32_t queue_family_count;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_props(queue_family_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, queue_props.data());

    const uint32_t invalid_count = queue_props[0].queueCount + 1;
    std::vector<float> priorities(invalid_count);
    std::fill(priorities.begin(), priorities.end(), 0.0f);

    auto device_queue_ci = LvlInitStruct<VkDeviceQueueCreateInfo>();
    device_queue_ci.queueFamilyIndex = 0;
    device_queue_ci.queueCount = queue_props[0].queueCount + 1;
    device_queue_ci.pQueuePriorities = priorities.data();

    auto device_ci = LvlInitStruct<VkDeviceCreateInfo>();
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &device_queue_ci;
    device_ci.enabledLayerCount = 0;
    device_ci.enabledExtensionCount = m_device_extension_names.size();
    device_ci.ppEnabledExtensionNames = m_device_extension_names.data();

    VkDevice device = VK_NULL_HANDLE;
    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceQueueCreateInfo-queueCount-00382");
    vk::CreateDevice(gpu(), &device_ci, nullptr, &device);
    m_errorMonitor->VerifyFound();
}

TEST_F(NegativeDeviceQueue, QueuePriorities) {
    TEST_DESCRIPTION("Create device queue with invalid QueuePriorities");

    ASSERT_NO_FATAL_FAILURE(InitFramework());

    uint32_t queue_family_count;
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_props(queue_family_count);
    vk::GetPhysicalDeviceQueueFamilyProperties(gpu(), &queue_family_count, queue_props.data());

    auto device_queue_ci = LvlInitStruct<VkDeviceQueueCreateInfo>();
    device_queue_ci.queueFamilyIndex = 0;
    device_queue_ci.queueCount = 1;

    auto device_ci = LvlInitStruct<VkDeviceCreateInfo>();
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &device_queue_ci;
    device_ci.enabledLayerCount = 0;
    device_ci.enabledExtensionCount = m_device_extension_names.size();
    device_ci.ppEnabledExtensionNames = m_device_extension_names.data();

    VkDevice device = VK_NULL_HANDLE;

    const float priority_high = 2.0f;
    device_queue_ci.pQueuePriorities = &priority_high;
    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceQueueCreateInfo-pQueuePriorities-00383");
    vk::CreateDevice(gpu(), &device_ci, nullptr, &device);
    m_errorMonitor->VerifyFound();

    const float priority_low = -1.0f;
    device_queue_ci.pQueuePriorities = &priority_low;
    m_errorMonitor->SetDesiredFailureMsg(kErrorBit, "VUID-VkDeviceQueueCreateInfo-pQueuePriorities-00383");
    vk::CreateDevice(gpu(), &device_ci, nullptr, &device);
    m_errorMonitor->VerifyFound();
}
