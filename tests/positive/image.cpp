/*
 * Copyright (c) 2015-2023 The Khronos Group Inc.
 * Copyright (c) 2015-2023 Valve Corporation
 * Copyright (c) 2015-2023 LunarG, Inc.
 * Copyright (c) 2015-2023 Google, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "../framework/layer_validation_tests.h"
#include "generated/vk_extension_helper.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

#include "utils/vk_layer_utils.h"

class PositiveImage : public VkPositiveLayerTest {};

TEST_F(PositiveImage, OwnershipTranfersImage) {
    TEST_DESCRIPTION("Valid image ownership transfers that shouldn't create errors");
    ASSERT_NO_FATAL_FAILURE(Init(nullptr, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT));

    const std::optional<uint32_t> no_gfx = m_device->QueueFamilyWithoutCapabilities(VK_QUEUE_GRAPHICS_BIT);
    if (!no_gfx) {
        GTEST_SKIP() << "Required queue families not present (non-graphics non-compute capable required)";
    }
    VkQueueObj *no_gfx_queue = m_device->queue_family_queues(no_gfx.value())[0].get();

    VkCommandPoolObj no_gfx_pool(m_device, no_gfx.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBufferObj no_gfx_cb(m_device, &no_gfx_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, no_gfx_queue);

    // Create an "exclusive" image owned by the graphics queue.
    VkImageObj image(m_device);
    VkFlags image_use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.Init(32, 32, 1, VK_FORMAT_B8G8R8A8_UNORM, image_use, VK_IMAGE_TILING_OPTIMAL, 0);
    ASSERT_TRUE(image.initialized());
    auto image_subres = image.subresource_range(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    auto image_barrier = image.image_memory_barrier(0, 0, image.Layout(), image.Layout(), image_subres);
    image_barrier.srcQueueFamilyIndex = m_device->graphics_queue_node_index_;
    image_barrier.dstQueueFamilyIndex = no_gfx.value();

    ValidOwnershipTransfer(m_errorMonitor, m_commandBuffer, &no_gfx_cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, nullptr, &image_barrier);

    // Change layouts while changing ownership
    image_barrier.srcQueueFamilyIndex = no_gfx.value();
    image_barrier.dstQueueFamilyIndex = m_device->graphics_queue_node_index_;
    image_barrier.oldLayout = image.Layout();
    // Make sure the new layout is different from the old
    if (image_barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        image_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    ValidOwnershipTransfer(m_errorMonitor, &no_gfx_cb, m_commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, nullptr, &image_barrier);
}

TEST_F(PositiveImage, UncompressedToCompressedImageCopy) {
    TEST_DESCRIPTION("Image copies between compressed and uncompressed images");
    ASSERT_NO_FATAL_FAILURE(Init());

    // Verify format support
    // Size-compatible (64-bit) formats. Uncompressed is 64 bits per texel, compressed is 64 bits per 4x4 block (or 4bpt).
    if (!ImageFormatAndFeaturesSupported(gpu(), VK_FORMAT_R16G16B16A16_UINT, VK_IMAGE_TILING_OPTIMAL,
                                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR) ||
        !ImageFormatAndFeaturesSupported(gpu(), VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_IMAGE_TILING_OPTIMAL,
                                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT_KHR | VK_FORMAT_FEATURE_TRANSFER_DST_BIT_KHR)) {
        GTEST_SKIP() << "Required formats/features not supported - UncompressedToCompressedImageCopy";
    }

    VkImageObj uncomp_10x10t_image(m_device);       // Size = 10 * 10 * 64 = 6400
    VkImageObj comp_10x10b_40x40t_image(m_device);  // Size = 40 * 40 * 4  = 6400

    uncomp_10x10t_image.Init(10, 10, 1, VK_FORMAT_R16G16B16A16_UINT,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_TILING_OPTIMAL);
    comp_10x10b_40x40t_image.Init(40, 40, 1, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_TILING_OPTIMAL);

    if (!uncomp_10x10t_image.initialized() || !comp_10x10b_40x40t_image.initialized()) {
        GTEST_SKIP() << "Unable to initialize surfaces - UncompressedToCompressedImageCopy";
    }

    // Both copies represent the same number of bytes. Bytes Per Texel = 1 for bc6, 16 for uncompressed
    // Copy compressed to uncompressed
    VkImageCopy copy_region = {};
    copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.srcSubresource.mipLevel = 0;
    copy_region.dstSubresource.mipLevel = 0;
    copy_region.srcSubresource.baseArrayLayer = 0;
    copy_region.dstSubresource.baseArrayLayer = 0;
    copy_region.srcSubresource.layerCount = 1;
    copy_region.dstSubresource.layerCount = 1;
    copy_region.srcOffset = {0, 0, 0};
    copy_region.dstOffset = {0, 0, 0};

    m_commandBuffer->begin();

    // Copy from uncompressed to compressed
    copy_region.extent = {10, 10, 1};  // Dimensions in (uncompressed) texels
    vk::CmdCopyImage(m_commandBuffer->handle(), uncomp_10x10t_image.handle(), VK_IMAGE_LAYOUT_GENERAL,
                     comp_10x10b_40x40t_image.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy_region);
    // The next copy swaps source and dest s.t. we need an execution barrier on for the prior source and an access barrier for
    // prior dest
    auto image_barrier = LvlInitStruct<VkImageMemoryBarrier>();
    image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    image_barrier.image = comp_10x10b_40x40t_image.handle();
    vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &image_barrier);

    // And from compressed to uncompressed
    copy_region.extent = {40, 40, 1};  // Dimensions in (compressed) texels
    vk::CmdCopyImage(m_commandBuffer->handle(), comp_10x10b_40x40t_image.handle(), VK_IMAGE_LAYOUT_GENERAL,
                     uncomp_10x10t_image.handle(), VK_IMAGE_LAYOUT_GENERAL, 1, &copy_region);

    m_commandBuffer->end();
}

TEST_F(PositiveImage, AliasedMemoryTracking) {
    TEST_DESCRIPTION(
        "Create a buffer, allocate memory, bind memory, destroy the buffer, create an image, and bind the same memory to it");

    ASSERT_NO_FATAL_FAILURE(Init());

    auto buffer = std::unique_ptr<VkBufferObj>(new VkBufferObj());
    VkDeviceSize buff_size = 256;
    buffer->init_no_mem(*DeviceObj(), VkBufferObj::create_info(buff_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));

    VkImageCreateInfo image_create_info = LvlInitStruct<VkImageCreateInfo>();
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;  // mandatory format
    image_create_info.extent.width = 64;                  // at least 4096x4096 is supported
    image_create_info.extent.height = 64;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    image_create_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageObj image(DeviceObj());
    image.init_no_mem(*DeviceObj(), image_create_info);

    const auto buffer_memory_requirements = buffer->memory_requirements();
    const auto image_memory_requirements = image.memory_requirements();

    vk_testing::DeviceMemory mem;
    VkMemoryAllocateInfo alloc_info = LvlInitStruct<VkMemoryAllocateInfo>();
    alloc_info.allocationSize = (std::max)(buffer_memory_requirements.size, image_memory_requirements.size);
    bool has_memtype =
        m_device->phy().set_memory_type(buffer_memory_requirements.memoryTypeBits & image_memory_requirements.memoryTypeBits,
                                        &alloc_info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (!has_memtype) {
        GTEST_SKIP() << "Failed to find a host visible memory type for both a buffer and an image";
    }
    mem.init(*DeviceObj(), alloc_info);

    auto pData = mem.map();
    std::memset(pData, 0xCADECADE, static_cast<size_t>(buff_size));
    mem.unmap();

    buffer->bind_memory(mem, 0);

    // NOW, destroy the buffer. Obviously, the resource no longer occupies this
    // memory. In fact, it was never used by the GPU.
    // Just be sure, wait for idle.
    buffer.reset(nullptr);
    vk::DeviceWaitIdle(m_device->device());

    // VALIDATION FAILURE:
    image.bind_memory(mem, 0);
}

TEST_F(PositiveImage, CreateImageViewFollowsParameterCompatibilityRequirements) {
    TEST_DESCRIPTION("Verify that creating an ImageView with valid usage does not generate validation errors.");

    ASSERT_NO_FATAL_FAILURE(Init());

    VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 nullptr,
                                 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                 VK_IMAGE_TYPE_2D,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 {128, 128, 1},
                                 1,
                                 1,
                                 VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE,
                                 0,
                                 nullptr,
                                 VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageObj image(m_device);
    image.init(&imgInfo);
    ASSERT_TRUE(image.initialized());
    image.targetView(VK_FORMAT_R8G8B8A8_UNORM);
}

TEST_F(PositiveImage, BasicUsage) {
    TEST_DESCRIPTION("Verify that creating an image view from an image with valid usage doesn't generate validation errors");

    ASSERT_NO_FATAL_FAILURE(Init());

    // Verify that we can create a view with usage INPUT_ATTACHMENT
    VkImageObj image(m_device);
    image.Init(128, 128, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_TILING_OPTIMAL, 0);
    ASSERT_TRUE(image.initialized());
    VkImageViewCreateInfo ivci = LvlInitStruct<VkImageViewCreateInfo>();
    ivci.image = image.handle();
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.layerCount = 1;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

    vk_testing::ImageView view(*m_device, ivci);
}

TEST_F(PositiveImage, BarrierLayoutToImageUsage) {
    TEST_DESCRIPTION("Ensure barriers' new and old VkImageLayout are compatible with their images' VkImageUsageFlags");

    ASSERT_NO_FATAL_FAILURE(Init());
    auto depth_format = FindSupportedDepthStencilFormat(gpu());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkImageMemoryBarrier img_barrier = LvlInitStruct<VkImageMemoryBarrier>();
    img_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    img_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    img_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    img_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    img_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    img_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    img_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    img_barrier.subresourceRange.baseArrayLayer = 0;
    img_barrier.subresourceRange.baseMipLevel = 0;
    img_barrier.subresourceRange.layerCount = 1;
    img_barrier.subresourceRange.levelCount = 1;

    {
        VkImageObj img_color(m_device);
        img_color.Init(128, 128, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_color.initialized());

        VkImageObj img_ds1(m_device);
        img_ds1.Init(128, 128, 1, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_ds1.initialized());

        VkImageObj img_ds2(m_device);
        img_ds2.Init(128, 128, 1, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_ds2.initialized());

        VkImageObj img_xfer_src(m_device);
        img_xfer_src.Init(128, 128, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_xfer_src.initialized());

        VkImageObj img_xfer_dst(m_device);
        img_xfer_dst.Init(128, 128, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_xfer_dst.initialized());

        VkImageObj img_sampled(m_device);
        img_sampled.Init(32, 32, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_sampled.initialized());

        VkImageObj img_input(m_device);
        img_input.Init(128, 128, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_TILING_OPTIMAL);
        ASSERT_TRUE(img_input.initialized());

        const struct {
            VkImageObj &image_obj;
            VkImageLayout old_layout;
            VkImageLayout new_layout;
        } buffer_layouts[] = {
            // clang-format off
            {img_color,    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         VK_IMAGE_LAYOUT_GENERAL},
            {img_ds1,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL},
            {img_ds2,      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,  VK_IMAGE_LAYOUT_GENERAL},
            {img_sampled,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,         VK_IMAGE_LAYOUT_GENERAL},
            {img_input,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,         VK_IMAGE_LAYOUT_GENERAL},
            {img_xfer_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,             VK_IMAGE_LAYOUT_GENERAL},
            {img_xfer_dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,             VK_IMAGE_LAYOUT_GENERAL},
            // clang-format on
        };
        const uint32_t layout_count = sizeof(buffer_layouts) / sizeof(buffer_layouts[0]);

        m_commandBuffer->begin();
        for (uint32_t i = 0; i < layout_count; ++i) {
            img_barrier.image = buffer_layouts[i].image_obj.handle();
            const VkImageUsageFlags usage = buffer_layouts[i].image_obj.usage();
            img_barrier.subresourceRange.aspectMask = (usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                                          ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                          : VK_IMAGE_ASPECT_COLOR_BIT;

            img_barrier.oldLayout = buffer_layouts[i].old_layout;
            img_barrier.newLayout = buffer_layouts[i].new_layout;
            vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &img_barrier);

            img_barrier.oldLayout = buffer_layouts[i].new_layout;
            img_barrier.newLayout = buffer_layouts[i].old_layout;
            vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &img_barrier);
        }
        m_commandBuffer->end();

        img_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        img_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
}

TEST_F(PositiveImage, FormatCompatibility) {
    TEST_DESCRIPTION("Test format compatibility");

    AddRequiredExtensions(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework());
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }

    ASSERT_NO_FATAL_FAILURE(InitState());

    VkFormat format = VK_FORMAT_R12X4G12X4_UNORM_2PACK16;

    VkImageFormatListCreateInfo format_list = LvlInitStruct<VkImageFormatListCreateInfo>();
    format_list.viewFormatCount = 1;
    format_list.pViewFormats = &format;

    VkImageCreateInfo image_create_info = LvlInitStruct<VkImageCreateInfo>(&format_list);
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_create_info.extent.width = 32;
    image_create_info.extent.height = 32;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_create_info.flags = 0;

    VkImage image;
    vk::CreateImage(m_device->device(), &image_create_info, nullptr, &image);

    vk::DestroyImage(m_device->device(), image, nullptr);
}

TEST_F(PositiveImage, FramebufferFrom3DImage) {
    TEST_DESCRIPTION("Validate creating a framebuffer from a 3D image.");

    AddRequiredExtensions(VK_KHR_MAINTENANCE_1_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework());
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }
    ASSERT_NO_FATAL_FAILURE(InitState());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    VkImageCreateInfo image_ci = LvlInitStruct<VkImageCreateInfo>();
    image_ci.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    image_ci.imageType = VK_IMAGE_TYPE_3D;
    image_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_ci.extent.width = 32;
    image_ci.extent.height = 32;
    image_ci.extent.depth = 4;
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageObj image(m_device);
    image.init(&image_ci);

    VkImageViewCreateInfo dsvci = LvlInitStruct<VkImageViewCreateInfo>();
    dsvci.image = image.handle();
    dsvci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dsvci.format = VK_FORMAT_B8G8R8A8_UNORM;
    dsvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dsvci.subresourceRange.baseMipLevel = 0;
    dsvci.subresourceRange.layerCount = 4;
    dsvci.subresourceRange.baseArrayLayer = 0;
    dsvci.subresourceRange.levelCount = 1;
    vk_testing::ImageView view(*m_device, dsvci);

    VkFramebufferCreateInfo fci = LvlInitStruct<VkFramebufferCreateInfo>();
    fci.renderPass = m_renderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &view.handle();
    fci.width = 32;
    fci.height = 32;
    fci.layers = 4;
    vk_testing::Framebuffer fb(*m_device, fci);
}

TEST_F(PositiveImage, SubresourceLayout) {
    ASSERT_NO_FATAL_FAILURE(Init());

    auto image_ci = vk_testing::Image::create_info();
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.extent.width = 64;
    image_ci.extent.height = 64;
    image_ci.mipLevels = 7;
    image_ci.arrayLayers = 6;
    image_ci.format = VK_FORMAT_R8_UINT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vk_testing::Image image;
    image.init(*m_device, image_ci);

    m_commandBuffer->begin();
    const auto subresource_range = image.subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
    auto barrier = image.image_memory_barrier(0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, subresource_range);
    vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.subresourceRange.baseMipLevel = 1;
    barrier.subresourceRange.levelCount = 1;
    vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.subresourceRange.baseMipLevel = 2;
    vk::CmdPipelineBarrier(m_commandBuffer->handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    m_commandBuffer->end();
    m_commandBuffer->QueueCommandBuffer();
}

TEST_F(PositiveImage, ImagelessLayoutTracking) {
    TEST_DESCRIPTION("Test layout tracking on imageless framebuffers");
    AddSurfaceExtension();
    AddRequiredExtensions(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
    SetTargetApiVersion(VK_API_VERSION_1_2);
    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));

    if (DeviceValidationVersion() < VK_API_VERSION_1_2) {
        GTEST_SKIP() << "At least Vulkan version 1.2 is required";
    }

    if (IsDriver(VK_DRIVER_ID_MESA_RADV)) {
        // According to valid usage, VkBindImageMemoryInfo-memory should be NULL. But RADV will crash if memory is NULL, "
        GTEST_SKIP() << "This test should not be run on the RADV driver";
    }

    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported.";
    }

    VkPhysicalDeviceImagelessFramebufferFeaturesKHR physicalDeviceImagelessFramebufferFeatures =
        LvlInitStruct<VkPhysicalDeviceImagelessFramebufferFeaturesKHR>();
    physicalDeviceImagelessFramebufferFeatures.imagelessFramebuffer = VK_TRUE;
    VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 =
        LvlInitStruct<VkPhysicalDeviceFeatures2>(&physicalDeviceImagelessFramebufferFeatures);

    uint32_t physical_device_group_count = 0;
    vk::EnumeratePhysicalDeviceGroups(instance(), &physical_device_group_count, nullptr);

    if (physical_device_group_count == 0) {
        GTEST_SKIP() << "physical_device_group_count is 0";
    }
    std::vector<VkPhysicalDeviceGroupProperties> physical_device_group(physical_device_group_count,
                                                                       {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES});
    vk::EnumeratePhysicalDeviceGroups(instance(), &physical_device_group_count, physical_device_group.data());
    VkDeviceGroupDeviceCreateInfo create_device_pnext = LvlInitStruct<VkDeviceGroupDeviceCreateInfo>();
    create_device_pnext.physicalDeviceCount = physical_device_group[0].physicalDeviceCount;
    create_device_pnext.pPhysicalDevices = physical_device_group[0].physicalDevices;
    create_device_pnext.pNext = &physicalDeviceFeatures2;

    ASSERT_NO_FATAL_FAILURE(InitState(nullptr, &create_device_pnext, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT));
    if (!InitSwapchain(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        GTEST_SKIP() << "Cannot create surface or swapchain";
    }
    uint32_t attachmentWidth = m_surface_capabilities.minImageExtent.width;
    uint32_t attachmentHeight = m_surface_capabilities.minImageExtent.height;
    VkFormat attachmentFormat = m_surface_formats[0].format;
    VkAttachmentDescription attachmentDescription[] = {{0, attachmentFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR}};
    VkAttachmentReference attachmentReference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpasses[] = {
        {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &attachmentReference, nullptr, nullptr, 0, nullptr},
    };
    VkRenderPassCreateInfo renderPassCreateInfo = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, attachmentDescription, 1, subpasses, 0, nullptr};
    vk_testing::RenderPass renderPass(*m_device, renderPassCreateInfo);

    // Create an image to use in an imageless framebuffer.  Bind swapchain memory to it.
    auto image_swapchain_create_info = LvlInitStruct<VkImageSwapchainCreateInfoKHR>();
    image_swapchain_create_info.swapchain = m_swapchain;
    VkImageCreateInfo imageCreateInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                         &image_swapchain_create_info,
                                         0,
                                         VK_IMAGE_TYPE_2D,
                                         attachmentFormat,
                                         {attachmentWidth, attachmentHeight, 1},
                                         1,
                                         1,
                                         VK_SAMPLE_COUNT_1_BIT,
                                         VK_IMAGE_TILING_OPTIMAL,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                         VK_SHARING_MODE_EXCLUSIVE,
                                         0,
                                         nullptr,
                                         VK_IMAGE_LAYOUT_UNDEFINED};

    VkImageObj image(m_device);
    image.init_no_mem(*m_device, imageCreateInfo);

    auto bind_devicegroup_info = LvlInitStruct<VkBindImageMemoryDeviceGroupInfo>();
    bind_devicegroup_info.deviceIndexCount = 1;
    std::array<uint32_t, 2> deviceIndices = {{0}};
    bind_devicegroup_info.pDeviceIndices = deviceIndices.data();
    bind_devicegroup_info.splitInstanceBindRegionCount = 0;
    bind_devicegroup_info.pSplitInstanceBindRegions = nullptr;

    auto bind_swapchain_info = LvlInitStruct<VkBindImageMemorySwapchainInfoKHR>(&bind_devicegroup_info);
    bind_swapchain_info.swapchain = m_swapchain;
    bind_swapchain_info.imageIndex = 0;

    auto bind_info = LvlInitStruct<VkBindImageMemoryInfo>(&bind_swapchain_info);
    bind_info.image = image.image();
    bind_info.memory = VK_NULL_HANDLE;
    bind_info.memoryOffset = 0;

    vk::BindImageMemory2(m_device->device(), 1, &bind_info);

    uint32_t swapchain_images_count = 0;
    vk::GetSwapchainImagesKHR(device(), m_swapchain, &swapchain_images_count, nullptr);
    std::vector<VkImage> swapchain_images;
    swapchain_images.resize(swapchain_images_count);
    vk::GetSwapchainImagesKHR(device(), m_swapchain, &swapchain_images_count, swapchain_images.data());
    uint32_t current_buffer;
    VkSemaphoreCreateInfo semaphore_create_info = LvlInitStruct<VkSemaphoreCreateInfo>();
    vk_testing::Semaphore image_acquired(*m_device, semaphore_create_info);
    vk::AcquireNextImageKHR(device(), m_swapchain, kWaitTimeout, image_acquired, VK_NULL_HANDLE, &current_buffer);

    VkImageView imageView = image.targetView(attachmentFormat);
    VkFramebufferAttachmentImageInfoKHR framebufferAttachmentImageInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO_KHR,
                                                                          nullptr,
                                                                          0,
                                                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                                          attachmentWidth,
                                                                          attachmentHeight,
                                                                          1,
                                                                          1,
                                                                          &attachmentFormat};
    VkFramebufferAttachmentsCreateInfoKHR framebufferAttachmentsCreateInfo = LvlInitStruct<VkFramebufferAttachmentsCreateInfoKHR>();
    framebufferAttachmentsCreateInfo.attachmentImageInfoCount = 1;
    framebufferAttachmentsCreateInfo.pAttachmentImageInfos = &framebufferAttachmentImageInfo;
    VkFramebufferCreateInfo framebufferCreateInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                     &framebufferAttachmentsCreateInfo,
                                                     VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT_KHR,
                                                     renderPass.handle(),
                                                     1,
                                                     reinterpret_cast<const VkImageView *>(1),
                                                     attachmentWidth,
                                                     attachmentHeight,
                                                     1};
    vk_testing::Framebuffer framebuffer(*m_device, framebufferCreateInfo);

    VkRenderPassAttachmentBeginInfoKHR renderPassAttachmentBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO_KHR,
                                                                        nullptr, 1, &imageView};
    VkRenderPassBeginInfo renderPassBeginInfo =
        LvlInitStruct<VkRenderPassBeginInfo>(&renderPassAttachmentBeginInfo, renderPass.handle(), framebuffer.handle(),
                                             VkRect2D{{0, 0}, {attachmentWidth, attachmentHeight}}, 0u, nullptr);

    // RenderPass should change the image layout of both the swapchain image and the aliased image to PRESENT_SRC_KHR
    m_commandBuffer->begin();
    m_commandBuffer->BeginRenderPass(renderPassBeginInfo);
    m_commandBuffer->EndRenderPass();
    m_commandBuffer->end();

    VkFenceObj fence;
    fence.init(*m_device, VkFenceObj::create_info());
    m_commandBuffer->QueueCommandBuffer(fence);

    VkPresentInfoKHR present = LvlInitStruct<VkPresentInfoKHR>();
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image_acquired.handle();
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &current_buffer;
    present.swapchainCount = 1;
    vk::QueuePresentKHR(m_device->m_queue, &present);
    vk::QueueWaitIdle(m_device->m_queue);
}

TEST_F(PositiveImage, ExtendedUsageWithDifferentFormatViews) {
    TEST_DESCRIPTION("Create views with different formats of an image with extended usage");

    AddRequiredExtensions(VK_KHR_MAINTENANCE_2_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(Init());
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }

    auto image_ci = LvlInitStruct<VkImageCreateInfo>();
    image_ci.flags =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_BC3_UNORM_BLOCK;
    image_ci.extent = {
        64,  // width
        64,  // height
        1,   // depth
    };
    image_ci.mipLevels = 1;
    image_ci.arrayLayers = 1;
    image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_ci.queueFamilyIndexCount = 0;
    image_ci.pQueueFamilyIndices = nullptr;

    VkImageFormatProperties image_properties;
    VkResult err = vk::GetPhysicalDeviceImageFormatProperties(gpu(), image_ci.format, image_ci.imageType, image_ci.tiling,
                                                              image_ci.usage, image_ci.flags, &image_properties);
    // Test not supported by driver
    if (err != VK_SUCCESS) {
        GTEST_SKIP() << "Image format not valid for format, type, tiling, usage and flags combination.";
    }

    vk_testing::Image image(*m_device, image_ci);
    ASSERT_TRUE(image.handle() != VK_NULL_HANDLE);

    // Since the format is compatible with all image's usage, there's no need to restrict usage
    auto iv_ci = LvlInitStruct<VkImageViewCreateInfo>();
    iv_ci.image = image.handle();
    iv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv_ci.format = VK_FORMAT_R32G32B32A32_UINT;
    iv_ci.subresourceRange.layerCount = 1;
    iv_ci.subresourceRange.baseMipLevel = 0;
    iv_ci.subresourceRange.levelCount = 1;
    iv_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_testing::ImageView view(*m_device, iv_ci);
    ASSERT_TRUE(view.handle() != VK_NULL_HANDLE);

    // Since usage is inherited from the image, we need to restrict the usage to a subset
    // Compressed images do not support storage, but we want to sample from the compressed
    VkImageViewUsageCreateInfo ivu_ci = LvlInitStruct<VkImageViewUsageCreateInfo>();
    ivu_ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    iv_ci.pNext = &ivu_ci;
    vk_testing::ImageView view2(*m_device, iv_ci);
    ASSERT_TRUE(view2.handle() != VK_NULL_HANDLE);
}

TEST_F(PositiveImage, ImageCompressionControl) {
    TEST_DESCRIPTION("Checks for creating fixed rate compression image.");

    SetTargetApiVersion(VK_API_VERSION_1_2);

    AddRequiredExtensions(VK_EXT_IMAGE_COMPRESSION_CONTROL_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework());

    if (DeviceValidationVersion() < VK_API_VERSION_1_2) {
        GTEST_SKIP() << "At least Vulkan version 1.2 is required, skipping test.";
    }

    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }

    auto image_compression_control = LvlInitStruct<VkPhysicalDeviceImageCompressionControlFeaturesEXT>();
    GetPhysicalDeviceFeatures2(image_compression_control);
    if (!image_compression_control.imageCompressionControl) {
        GTEST_SKIP() << "Test requires (unsupported) imageCompressionControl, skipping";
    }

    ASSERT_NO_FATAL_FAILURE(InitState(nullptr, &image_compression_control));

    // Query possible image format with vkGetPhysicalDeviceImageFormatProperties2KHR
    auto image_format_info = LvlInitStruct<VkPhysicalDeviceImageFormatInfo2>();
    image_format_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_format_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_format_info.type = VK_IMAGE_TYPE_2D;
    image_format_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    auto compression_properties = LvlInitStruct<VkImageCompressionPropertiesEXT>();
    auto image_format_properties = LvlInitStruct<VkImageFormatProperties2>(&compression_properties);

    vk::GetPhysicalDeviceImageFormatProperties2(gpu(), &image_format_info, &image_format_properties);

    auto image_ci = vk_testing::Image::create_info();
    image_ci.imageType = VK_IMAGE_TYPE_2D;
    image_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_ci.tiling = VK_IMAGE_TILING_LINEAR;
    image_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Create with fixed rate compression image
    {
        auto compressionControl = LvlInitStruct<VkImageCompressionControlEXT>();
        compressionControl.flags = VK_IMAGE_COMPRESSION_FIXED_RATE_DEFAULT_EXT;
        image_ci.pNext = &compressionControl;

        vk_testing::Image image(*m_device, image_ci);
    }

    // Create with fixed rate compression image
    if (compression_properties.imageCompressionFixedRateFlags != VK_IMAGE_COMPRESSION_FIXED_RATE_NONE_EXT) {
        VkImageCompressionFixedRateFlagsEXT supported_compression_fixed_rate = VK_IMAGE_COMPRESSION_FIXED_RATE_NONE_EXT;

        for (uint32_t index = 0; index < 32; index++) {
            if ((compression_properties.imageCompressionFixedRateFlags & (1 << index)) != 0) {
                supported_compression_fixed_rate = (1 << index);
                break;
            }
        }

        ASSERT_TRUE(supported_compression_fixed_rate != VK_IMAGE_COMPRESSION_FIXED_RATE_NONE_EXT);

        VkImageCompressionFixedRateFlagsEXT fixedRageFlags = supported_compression_fixed_rate;
        auto compressionControl = LvlInitStruct<VkImageCompressionControlEXT>();
        compressionControl.flags = VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT;
        compressionControl.pFixedRateFlags = &fixedRageFlags;
        compressionControl.compressionControlPlaneCount = 1;
        image_ci.pNext = &compressionControl;

        vk_testing::Image image(*m_device, image_ci);
    }
}

TEST_F(PositiveImage, Create3DImageView) {
    TEST_DESCRIPTION("Create a 3D image view");

    ASSERT_NO_FATAL_FAILURE(Init());

    VkImageObj image(m_device);
    auto ci = LvlInitStruct<VkImageCreateInfo>();
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {32, 32, 8};
    ci.mipLevels = 6;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image.init(&ci);
    ASSERT_TRUE(image.initialized());

    auto ivci = LvlInitStruct<VkImageViewCreateInfo>();
    ivci.image = image.handle();
    ivci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.layerCount = 1;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vk_testing::ImageView image_view(*m_device, ivci);
}

TEST_F(PositiveImage, SlicedCreateInfo) {
    TEST_DESCRIPTION("Test VkImageViewSlicedCreateInfoEXT");

    SetTargetApiVersion(VK_API_VERSION_1_1);
    AddRequiredExtensions(VK_EXT_IMAGE_SLICED_VIEW_OF_3D_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework());

    if (DeviceValidationVersion() < VK_API_VERSION_1_1) {
        GTEST_SKIP() << "At least Vulkan version 1.1 is required, skipping test.";
    }

    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }

    {
        auto slice_feature = LvlInitStruct<VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT>();
        GetPhysicalDeviceFeatures2(slice_feature);
        if (slice_feature.imageSlicedViewOf3D == VK_FALSE) {
            GTEST_SKIP() << "Test requires (unsupported) imageSlicedViewOf3D";
        }
        InitState(nullptr, &slice_feature);
    }

    VkImageObj image(m_device);
    auto ci = LvlInitStruct<VkImageCreateInfo>();
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {32, 32, 8};
    ci.mipLevels = 6;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image.init(&ci);
    ASSERT_TRUE(image.initialized());

    auto sliced_info = LvlInitStruct<VkImageViewSlicedCreateInfoEXT>();

    auto ivci = LvlInitStruct<VkImageViewCreateInfo>(&sliced_info);
    ivci.image = image.handle();
    ivci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    ivci.format = ci.format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;

    auto get_effective_depth = [&]() -> uint32_t { return GetEffectiveExtent(ci, ivci.subresourceRange).depth; };

    {
        sliced_info.sliceCount = VK_REMAINING_3D_SLICES_EXT;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.baseMipLevel = 0;
        ASSERT_TRUE(get_effective_depth() == 8);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = VK_REMAINING_3D_SLICES_EXT;
        sliced_info.sliceOffset = 1;
        ivci.subresourceRange.baseMipLevel = 0;
        ASSERT_TRUE(get_effective_depth() == 8);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = 8;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.baseMipLevel = 0;
        ASSERT_TRUE(get_effective_depth() == 8);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = 4;
        sliced_info.sliceOffset = 4;
        ivci.subresourceRange.baseMipLevel = 0;
        ASSERT_TRUE(get_effective_depth() == 8);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = 4;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.baseMipLevel = 1;
        ASSERT_TRUE(get_effective_depth() == 4);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = 2;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.baseMipLevel = 2;
        ASSERT_TRUE(get_effective_depth() == 2);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = VK_REMAINING_3D_SLICES_EXT;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        ivci.subresourceRange.baseMipLevel = 5;
        ASSERT_TRUE(get_effective_depth() == 1);

        vk_testing::ImageView image_view(*m_device, ivci);
    }

    {
        sliced_info.sliceCount = VK_REMAINING_3D_SLICES_EXT;
        sliced_info.sliceOffset = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseMipLevel = 5;
        ASSERT_TRUE(get_effective_depth() == 1);

        vk_testing::ImageView image_view(*m_device, ivci);
    }
}

TEST_F(PositiveImage, DrmFormatModifier) {
    // See https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/2610
    TEST_DESCRIPTION("Create image and imageView using VK_EXT_image_drm_format_modifier");

    SetTargetApiVersion(VK_API_VERSION_1_1);  // for extension dependencies
    AddRequiredExtensions(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported.";
    }

    if (IsPlatform(kMockICD)) {
        GTEST_SKIP() << "Test not supported by MockICD, skipping tests";
    }

    if (DeviceValidationVersion() < VK_API_VERSION_1_1) {
        GTEST_SKIP() << "At least Vulkan version 1.1 is required";
    }

    ASSERT_NO_FATAL_FAILURE(InitState());

    // we just hope that one of these formats supports modifiers
    // for more detailed checking, we could also check multi-planar formats.
    auto format_list = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
    };

    for (auto format : format_list) {
        std::vector<uint64_t> mods;

        // get general features and modifiers
        VkDrmFormatModifierPropertiesListEXT modp = LvlInitStruct<VkDrmFormatModifierPropertiesListEXT>();
        auto fmtp = LvlInitStruct<VkFormatProperties2>(&modp);

        vk::GetPhysicalDeviceFormatProperties2(gpu(), format, &fmtp);

        if (modp.drmFormatModifierCount > 0) {
            // the first call to vkGetPhysicalDeviceFormatProperties2 did only
            // retrieve the number of modifiers, we now have to retrieve
            // the modifiers
            std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(modp.drmFormatModifierCount);
            modp.pDrmFormatModifierProperties = mod_props.data();

            vk::GetPhysicalDeviceFormatProperties2(gpu(), format, &fmtp);

            for (auto i = 0u; i < modp.drmFormatModifierCount; ++i) {
                auto &mod = modp.pDrmFormatModifierProperties[i];
                auto features = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

                if ((mod.drmFormatModifierTilingFeatures & features) != features) {
                    continue;
                }

                mods.push_back(mod.drmFormatModifier);
            }
        }

        if (mods.empty()) {
            continue;
        }

        // create image
        auto ci = LvlInitStruct<VkImageCreateInfo>();
        ci.flags = 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {128, 128, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageDrmFormatModifierListCreateInfoEXT mod_list = LvlInitStruct<VkImageDrmFormatModifierListCreateInfoEXT>();
        mod_list.pDrmFormatModifiers = mods.data();
        mod_list.drmFormatModifierCount = mods.size();
        ci.pNext = &mod_list;

        VkImage image;
        VkResult err = vk::CreateImage(device(), &ci, nullptr, &image);
        ASSERT_VK_SUCCESS(err);

        // bind memory
        VkPhysicalDeviceMemoryProperties phys_mem_props;
        vk::GetPhysicalDeviceMemoryProperties(gpu(), &phys_mem_props);
        VkMemoryRequirements mem_reqs;
        vk::GetImageMemoryRequirements(device(), image, &mem_reqs);
        VkDeviceMemory mem_obj = VK_NULL_HANDLE;
        VkMemoryPropertyFlagBits mem_props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        for (uint32_t type = 0; type < phys_mem_props.memoryTypeCount; type++) {
            if ((mem_reqs.memoryTypeBits & (1 << type)) &&
                ((phys_mem_props.memoryTypes[type].propertyFlags & mem_props) == mem_props)) {
                VkMemoryAllocateInfo alloc_info = LvlInitStruct<VkMemoryAllocateInfo>();
                alloc_info.allocationSize = mem_reqs.size;
                alloc_info.memoryTypeIndex = type;
                ASSERT_VK_SUCCESS(vk::AllocateMemory(device(), &alloc_info, nullptr, &mem_obj));
                break;
            }
        }

        ASSERT_NE((VkDeviceMemory)VK_NULL_HANDLE, mem_obj);
        ASSERT_VK_SUCCESS(vk::BindImageMemory(device(), image, mem_obj, 0));

        // create image view
        VkImageViewCreateInfo ivci = {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0,
            image,
            VK_IMAGE_VIEW_TYPE_2D,
            format,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
             VK_COMPONENT_SWIZZLE_IDENTITY},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };

        CreateImageViewTest(*this, &ivci);

        // for more detailed checking, we could export the image to dmabuf
        // and then import it again (using VkImageDrmFormatModifierExplicitCreateInfoEXT)

        vk::FreeMemory(device(), mem_obj, nullptr);
        vk::DestroyImage(device(), image, nullptr);
    }
}

TEST_F(PositiveImage, CreateDrmImageWithExternalMemory) {
    // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/5649
    TEST_DESCRIPTION(
        "Create image with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and VkExternalMemoryImageCreateInfo in the pNext chain");

    SetTargetApiVersion(VK_API_VERSION_1_1);  // for extension dependencies
    AddRequiredExtensions(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    AddRequiredExtensions(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported.";
    }

    if (DeviceValidationVersion() < VK_API_VERSION_1_1) {
        GTEST_SKIP() << "At least Vulkan version 1.1 is required";
    }

    if (IsPlatform(kMockICD)) {
        GTEST_SKIP() << "Test not supported by MockICD, skipping tests";
    }

    ASSERT_NO_FATAL_FAILURE(InitState());

    std::vector<uint64_t> mods;

    const auto format = VK_FORMAT_R8G8B8A8_UNORM;

    // Get info needed to fill out VkImageDrmFormatModifierListCreateInfoEXT
    {
        auto modp = LvlInitStruct<VkDrmFormatModifierPropertiesListEXT>();
        auto fmtp = LvlInitStruct<VkFormatProperties2>(&modp);
        vk::GetPhysicalDeviceFormatProperties2(gpu(), format, &fmtp);
        if (modp.drmFormatModifierCount == 0) {
            GTEST_SKIP() << "drmFormatModifierCount equal to 0";
        }
        std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(modp.drmFormatModifierCount);
        modp.pDrmFormatModifierProperties = mod_props.data();

        vk::GetPhysicalDeviceFormatProperties2(gpu(), format, &fmtp);

        for (uint32_t i = 0; i < modp.drmFormatModifierCount; ++i) {
            auto &mod = modp.pDrmFormatModifierProperties[i];
            auto features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

            if ((mod.drmFormatModifierTilingFeatures & features) != features) {
                continue;
            }

            mods.push_back(mod.drmFormatModifier);
        }
    }

    if (mods.empty()) {
        GTEST_SKIP() << "Skip test";
    }

    auto external_info = LvlInitStruct<VkExternalMemoryImageCreateInfo>();
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    // handleTypes needs to be assigned to trigger the behavior we want
    assert(external_info.handleTypes);

    auto drm_info = LvlInitStruct<VkImageDrmFormatModifierListCreateInfoEXT>(&external_info);
    drm_info.drmFormatModifierCount = size32(mods);
    drm_info.pDrmFormatModifiers = mods.data();

    auto ci = LvlInitStruct<VkImageCreateInfo>(&drm_info);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {128, 128, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    {
        auto drm_format_modifier = LvlInitStruct<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>();
        drm_format_modifier.sharingMode = ci.sharingMode;
        drm_format_modifier.queueFamilyIndexCount = ci.queueFamilyIndexCount;
        drm_format_modifier.pQueueFamilyIndices = ci.pQueueFamilyIndices;
        auto external_image_info = LvlInitStruct<VkPhysicalDeviceExternalImageFormatInfo>(&drm_format_modifier);
        external_image_info.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(external_info.handleTypes);
        auto image_info = LvlInitStruct<VkPhysicalDeviceImageFormatInfo2>(&external_image_info);
        image_info.format = ci.format;
        image_info.type = ci.imageType;
        image_info.tiling = ci.tiling;
        image_info.usage = ci.usage;
        image_info.flags = ci.flags;

        auto external_image_properties = LvlInitStruct<VkExternalImageFormatProperties>();
        auto image_properties = LvlInitStruct<VkImageFormatProperties2>(&external_image_properties);

        if (const auto result = vk::GetPhysicalDeviceImageFormatProperties2(gpu(), &image_info, &image_properties);
            result != VK_SUCCESS) {
            GTEST_SKIP() << "Unable to create image. VkResult = " << vk_result_string(result);
        }
    }

    CreateImageTest(*this, &ci);
}

TEST_F(PositiveImage, CopyImageSubresource) {
    ASSERT_NO_FATAL_FAILURE(InitFramework());
    ASSERT_NO_FATAL_FAILURE(InitState(nullptr, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT));

    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageObj image(m_device);
    auto image_ci = VkImageObj::ImageCreateInfo2D(128, 128, 2, 5, format, usage, VK_IMAGE_TILING_OPTIMAL);
    image.InitNoLayout(image_ci);
    ASSERT_TRUE(image.initialized());

    VkImageSubresourceLayers src_layer{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageSubresourceLayers dst_layer{VK_IMAGE_ASPECT_COLOR_BIT, 1, 3, 1};
    VkOffset3D zero_offset{0, 0, 0};
    VkExtent3D full_extent{128 / 2, 128 / 2, 1};  // <-- image type is 2D
    VkImageCopy region = {src_layer, zero_offset, dst_layer, zero_offset, full_extent};
    auto init_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    auto dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    auto final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    m_commandBuffer->begin();

    auto cb = m_commandBuffer->handle();

    VkImageSubresourceRange src_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier image_barriers[2];

    image_barriers[0] = LvlInitStruct<VkImageMemoryBarrier>();
    image_barriers[0].srcAccessMask = 0;
    image_barriers[0].dstAccessMask = 0;
    image_barriers[0].image = image.handle();
    image_barriers[0].subresourceRange = src_range;
    image_barriers[0].oldLayout = init_layout;
    image_barriers[0].newLayout = dst_layout;

    vk::CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           image_barriers);
    VkClearColorValue clear_color{};
    vk::CmdClearColorImage(cb, image.handle(), dst_layout, &clear_color, 1, &src_range);
    m_commandBuffer->end();

    auto submit_info = LvlInitStruct<VkSubmitInfo>();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &m_commandBuffer->handle();

    vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
    vk::QueueWaitIdle(m_device->m_queue);

    m_commandBuffer->begin();

    image_barriers[0].oldLayout = dst_layout;
    image_barriers[0].newLayout = src_layout;

    VkImageSubresourceRange dst_range{VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 3, 1};
    image_barriers[1] = LvlInitStruct<VkImageMemoryBarrier>();
    image_barriers[1].srcAccessMask = 0;
    image_barriers[1].dstAccessMask = 0;
    image_barriers[1].image = image.handle();
    image_barriers[1].subresourceRange = dst_range;
    image_barriers[1].oldLayout = init_layout;
    image_barriers[1].newLayout = dst_layout;

    vk::CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           image_barriers);

    vk::CmdCopyImage(cb, image.handle(), src_layout, image.handle(), dst_layout, 1, &region);

    image_barriers[0].oldLayout = src_layout;
    image_barriers[0].newLayout = final_layout;
    image_barriers[1].oldLayout = dst_layout;
    image_barriers[1].newLayout = final_layout;
    vk::CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2,
                           image_barriers);
    m_commandBuffer->end();

    vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
    vk::QueueWaitIdle(m_device->m_queue);
}

TEST_F(PositiveImage, DescriptorSubresourceLayout) {
    AddRequiredExtensions(VK_KHR_MAINTENANCE_2_EXTENSION_NAME);
    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));
    if (!AreRequiredExtensionsEnabled()) {
        GTEST_SKIP() << RequiredExtensionsNotSupported() << " not supported";
    }
    ASSERT_NO_FATAL_FAILURE(InitState(nullptr, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT));

    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    OneOffDescriptorSet descriptor_set(m_device,
                                       {
                                           {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr},
                                       });
    VkDescriptorSet descriptorSet = descriptor_set.set_;

    const VkPipelineLayoutObj pipeline_layout(m_device, {&descriptor_set.layout_});

    // Create image, view, and sampler
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageObj image(m_device);
    auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto image_ci = VkImageObj::ImageCreateInfo2D(128, 128, 1, 5, format, usage, VK_IMAGE_TILING_OPTIMAL);
    image.Init(image_ci);
    ASSERT_TRUE(image.initialized());

    VkImageSubresourceRange view_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 3, 1};
    VkImageSubresourceRange first_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageSubresourceRange full_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 5};
    vk_testing::ImageView view;
    auto image_view_create_info = LvlInitStruct<VkImageViewCreateInfo>();
    image_view_create_info.image = image.handle();
    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_create_info.format = format;
    image_view_create_info.subresourceRange = view_range;

    view.init(*m_device, image_view_create_info);
    ASSERT_TRUE(view.initialized());

    // Create Sampler
    vk_testing::Sampler sampler;
    VkSamplerCreateInfo sampler_ci = SafeSaneSamplerCreateInfo();
    sampler.init(*m_device, sampler_ci);
    ASSERT_TRUE(sampler.initialized());

    // Setup structure for descriptor update with sampler, for update in do_test below
    VkDescriptorImageInfo img_info = {};
    img_info.sampler = sampler.handle();

    VkWriteDescriptorSet descriptor_write = LvlInitStruct<VkWriteDescriptorSet>();
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.pImageInfo = &img_info;

    // Create PSO to be used for draw-time errors below
    VkShaderObj vs(this, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderObj fs(this, bindStateFragSamplerShaderText, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.AddDefaultColorAttachment();
    pipe.CreateVKPipeline(pipeline_layout.handle(), renderPass());

    VkViewport viewport = {0, 0, 16, 16, 0, 1};
    VkRect2D scissor = {{0, 0}, {16, 16}};

    VkCommandBufferObj cmd_buf(m_device, m_commandPool);

    VkSubmitInfo submit_info = LvlInitStruct<VkSubmitInfo>();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf.handle();

    enum TestType {
        kInternal,  // Image layout mismatch is *within* a given command buffer
        kExternal   // Image layout mismatch is with the current state of the image, found at QueueSubmit
    };
    std::array<TestType, 2> test_list = {{kInternal, kExternal}};

    auto do_test = [&](VkImageObj *image, vk_testing::ImageView *view, VkImageAspectFlags aspect_mask,
                       VkImageLayout descriptor_layout) {
        // Set up the descriptor
        img_info.imageView = view->handle();
        img_info.imageLayout = descriptor_layout;
        vk::UpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

        for (TestType test_type : test_list) {
            auto init_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            auto image_barrier = LvlInitStruct<VkImageMemoryBarrier>();

            cmd_buf.begin();
            image_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            image_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            image_barrier.image = image->handle();
            image_barrier.subresourceRange = full_range;
            image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image_barrier.newLayout = init_layout;

            cmd_buf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &image_barrier);

            image_barrier.subresourceRange = first_range;
            image_barrier.oldLayout = init_layout;
            image_barrier.newLayout = descriptor_layout;
            cmd_buf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &image_barrier);

            image_barrier.subresourceRange = view_range;
            image_barrier.oldLayout = init_layout;
            image_barrier.newLayout = descriptor_layout;
            cmd_buf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &image_barrier);

            if (test_type == kExternal) {
                // The image layout is external to the command buffer we are recording to test.  Submit to push to instance scope.
                cmd_buf.end();
                vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
                vk::QueueWaitIdle(m_device->m_queue);
                cmd_buf.begin();
            }

            cmd_buf.BeginRenderPass(m_renderPassBeginInfo);
            vk::CmdBindPipeline(cmd_buf.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
            vk::CmdBindDescriptorSets(cmd_buf.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0, 1,
                                      &descriptorSet, 0, NULL);
            vk::CmdSetViewport(cmd_buf.handle(), 0, 1, &viewport);
            vk::CmdSetScissor(cmd_buf.handle(), 0, 1, &scissor);

            cmd_buf.Draw(1, 0, 0, 0);

            cmd_buf.EndRenderPass();
            cmd_buf.end();

            // Submit cmd buffer
            vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
            vk::QueueWaitIdle(m_device->m_queue);
        }
    };
    do_test(&image, &view, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST_F(VkPositiveLayerTest, ImageDescriptor3D2DSubresourceLayout) {
    TEST_DESCRIPTION("Verify renderpass layout transitions for a 2d ImageView created from a 3d Image.");
    SetTargetApiVersion(VK_API_VERSION_1_1);
    ASSERT_NO_FATAL_FAILURE(InitFramework(m_errorMonitor));
    if (DeviceValidationVersion() < VK_API_VERSION_1_1) {
        GTEST_SKIP() << "At least Vulkan version 1.1 is required";
    }

    ASSERT_NO_FATAL_FAILURE(InitState(nullptr, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT));

    ASSERT_NO_FATAL_FAILURE(InitViewport());
    ASSERT_NO_FATAL_FAILURE(InitRenderTarget());

    OneOffDescriptorSet descriptor_set(m_device,
                                       {
                                           {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr},
                                       });
    VkDescriptorSet descriptorSet = descriptor_set.set_;

    const VkPipelineLayoutObj pipeline_layout(m_device, {&descriptor_set.layout_});

    // Create image, view, and sampler
    const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageObj image_3d(m_device);
    VkImageObj other_image(m_device);
    auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    static const uint32_t kWidth = 128;
    static const uint32_t kHeight = 128;

    auto image_ci_3d = LvlInitStruct<VkImageCreateInfo>();
    image_ci_3d.flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    image_ci_3d.imageType = VK_IMAGE_TYPE_3D;
    image_ci_3d.format = format;
    image_ci_3d.extent.width = kWidth;
    image_ci_3d.extent.height = kHeight;
    image_ci_3d.extent.depth = 8;
    image_ci_3d.mipLevels = 1;
    image_ci_3d.arrayLayers = 1;
    image_ci_3d.samples = VK_SAMPLE_COUNT_1_BIT;
    image_ci_3d.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_ci_3d.usage = usage;
    image_3d.Init(image_ci_3d);
    ASSERT_TRUE(image_3d.initialized());

    other_image.Init(kWidth, kHeight, 1, format, usage, VK_IMAGE_TILING_OPTIMAL, 0);
    ASSERT_TRUE(other_image.initialized());

    // The image view is a 2D slice of the 3D image at depth = 4, which we request by
    // asking for arrayLayer = 4
    VkImageSubresourceRange view_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 4, 1};
    // But, the spec says:
    //    Automatic layout transitions apply to the entire image subresource attached
    //    to the framebuffer. If the attachment view is a 2D or 2D array view of a
    //    3D image, even if the attachment view only refers to a subset of the slices
    //    of the selected mip level of the 3D image, automatic layout transitions apply
    //    to the entire subresource referenced which is the entire mip level in this case.
    VkImageSubresourceRange full_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_testing::ImageView view_2d, other_view;
    auto image_view_create_info = LvlInitStruct<VkImageViewCreateInfo>();
    image_view_create_info.image = image_3d.handle();
    image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    image_view_create_info.format = format;
    image_view_create_info.subresourceRange = view_range;

    view_2d.init(*m_device, image_view_create_info);
    ASSERT_TRUE(view_2d.initialized());

    image_view_create_info.image = other_image.handle();
    image_view_create_info.subresourceRange = full_range;
    other_view.init(*m_device, image_view_create_info);
    ASSERT_TRUE(other_view.initialized());

    std::vector<VkAttachmentDescription> attachments = {
        {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };

    std::vector<VkAttachmentReference> color = {
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };

    VkSubpassDescription subpass = {
        0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, (uint32_t)color.size(), color.data(), nullptr, nullptr, 0, nullptr};

    std::vector<VkSubpassDependency> deps = {
        {VK_SUBPASS_EXTERNAL, 0,
         (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT),
         (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT),
         (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_TRANSFER_WRITE_BIT),
         (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT), 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT), VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT), 0},
    };

    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   nullptr,
                                   0,
                                   (uint32_t)attachments.size(),
                                   attachments.data(),
                                   1,
                                   &subpass,
                                   (uint32_t)deps.size(),
                                   deps.data()};
    // Create Sampler
    vk_testing::Sampler sampler;
    VkSamplerCreateInfo sampler_ci = SafeSaneSamplerCreateInfo();
    sampler.init(*m_device, sampler_ci);
    ASSERT_TRUE(sampler.initialized());

    // Setup structure for descriptor update with sampler, for update in do_test below
    VkDescriptorImageInfo img_info = {};
    img_info.sampler = sampler.handle();

    VkWriteDescriptorSet descriptor_write = LvlInitStruct<VkWriteDescriptorSet>();
    descriptor_write.dstSet = descriptorSet;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorCount = 1;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.pImageInfo = &img_info;

    vk_testing::RenderPass rp(*m_device, rpci);

    // Create PSO to be used for draw-time errors below
    VkShaderObj vs(this, bindStateVertShaderText, VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderObj fs(this, bindStateFragSamplerShaderText, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPipelineObj pipe(m_device);
    pipe.AddShader(&vs);
    pipe.AddShader(&fs);
    pipe.AddDefaultColorAttachment();
    pipe.CreateVKPipeline(pipeline_layout.handle(), rp.handle());

    VkViewport viewport = {0, 0, kWidth, kHeight, 0, 1};
    VkRect2D scissor = {{0, 0}, {kWidth, kHeight}};

    VkCommandBufferObj cmd_buf(m_device, m_commandPool);

    VkSubmitInfo submit_info = LvlInitStruct<VkSubmitInfo>();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd_buf.handle();

    enum TestType {
        kInternal,  // Image layout mismatch is *within* a given command buffer
        kExternal   // Image layout mismatch is with the current state of the image, found at QueueSubmit
    };
    std::array<TestType, 2> test_list = {{kInternal, kExternal}};

    auto do_test = [&](VkImageObj *image, vk_testing::ImageView *view, VkImageObj *o_image, vk_testing::ImageView *o_view,
                       VkImageAspectFlags aspect_mask, VkImageLayout descriptor_layout) {
        // Set up the descriptor
        img_info.imageView = o_view->handle();
        img_info.imageLayout = descriptor_layout;
        vk::UpdateDescriptorSets(m_device->device(), 1, &descriptor_write, 0, NULL);

        for (TestType test_type : test_list) {
            auto image_barrier = LvlInitStruct<VkImageMemoryBarrier>();

            VkFramebufferCreateInfo fbci = {
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp.handle(), 1, &view->handle(), kWidth, kHeight, 1};
            vk_testing::Framebuffer fb(*m_device, fbci);

            cmd_buf.begin();
            image_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            image_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            image_barrier.image = image->handle();
            image_barrier.subresourceRange = full_range;
            image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            image_barrier.newLayout = descriptor_layout;

            cmd_buf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &image_barrier);
            image_barrier.image = o_image->handle();
            cmd_buf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &image_barrier);

            if (test_type == kExternal) {
                // The image layout is external to the command buffer we are recording to test.  Submit to push to instance scope.
                cmd_buf.end();
                vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
                vk::QueueWaitIdle(m_device->m_queue);
                cmd_buf.begin();
            }

            m_renderPassBeginInfo.renderPass = rp.handle();
            m_renderPassBeginInfo.framebuffer = fb.handle();
            m_renderPassBeginInfo.renderArea = {{0, 0}, {kWidth, kHeight}};

            cmd_buf.BeginRenderPass(m_renderPassBeginInfo);
            vk::CmdBindPipeline(cmd_buf.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.handle());
            vk::CmdBindDescriptorSets(cmd_buf.handle(), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout.handle(), 0, 1,
                                      &descriptorSet, 0, NULL);
            vk::CmdSetViewport(cmd_buf.handle(), 0, 1, &viewport);
            vk::CmdSetScissor(cmd_buf.handle(), 0, 1, &scissor);

            cmd_buf.Draw(1, 0, 0, 0);

            cmd_buf.EndRenderPass();
            cmd_buf.end();

            // Submit cmd buffer
            vk::QueueSubmit(m_device->m_queue, 1, &submit_info, VK_NULL_HANDLE);
            vk::QueueWaitIdle(m_device->m_queue);
        }
    };
    do_test(&image_3d, &view_2d, &other_image, &other_view, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
