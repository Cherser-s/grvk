#include "mantle_internal.h"

static VkImageSubresourceRange getVkImageSubresourceRange(
    const GR_IMAGE_SUBRESOURCE_RANGE* range)
{
    return (VkImageSubresourceRange) {
        .aspectMask = getVkImageAspectFlags(range->aspect),
        .baseMipLevel = range->baseMipLevel,
        .levelCount = range->mipLevels == GR_LAST_MIP_OR_SLICE ?
                      VK_REMAINING_MIP_LEVELS : range->mipLevels,
        .baseArrayLayer = range->baseArraySlice,
        .layerCount = range->arraySize == GR_LAST_MIP_OR_SLICE ?
                      VK_REMAINING_ARRAY_LAYERS : range->arraySize,
    };
}

static VkFramebuffer getVkFramebuffer(
    VkDevice device,
    VkRenderPass renderPass,
    unsigned attachmentCount,
    const VkImageView* attachments,
    VkExtent2D extent,
    unsigned layerCount
    )
{
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    const VkFramebufferCreateInfo framebufferCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .renderPass = renderPass,
        .attachmentCount = attachmentCount,
        .pAttachments = attachments,
        .width = extent.width,
        .height = extent.height,
        .layers = layerCount,
    };

    if (vki.vkCreateFramebuffer(device, &framebufferCreateInfo, NULL,
                                &framebuffer) != VK_SUCCESS) {
        LOGE("vkCreateFramebuffer failed\n");
        return VK_NULL_HANDLE;
    }

    return framebuffer;
}

static void initCmdBufferResources(
    GrCmdBuffer* grCmdBuffer)
{
    const GrPipeline* grPipeline = grCmdBuffer->grPipeline;
    VkPipelineBindPoint bindPoint = getVkPipelineBindPoint(GR_PIPELINE_BIND_POINT_GRAPHICS);

    vki.vkCmdBindPipeline(grCmdBuffer->commandBuffer, bindPoint, grPipeline->pipeline);

    vki.vkCmdBindDescriptorSets(grCmdBuffer->commandBuffer, bindPoint,
                                grPipeline->pipelineLayout, 0, 1,
                                &grCmdBuffer->grDevice->globalDescriptorSet.descriptorTable, 0, NULL);
    unsigned size = sizeof(uint64_t) * 2;
    uint64_t descSetBuffer[2] = {
        grCmdBuffer->graphicsDescriptorSets[0] == NULL ? 0 : (grCmdBuffer->graphicsDescriptorSets[0]->bufferDevicePtr + sizeof(uint64_t) * grCmdBuffer->graphicsDescriptorSetOffsets[0]),
        grCmdBuffer->graphicsDescriptorSets[1] == NULL ? 0 : (grCmdBuffer->graphicsDescriptorSets[1]->bufferDevicePtr + sizeof(uint64_t) * grCmdBuffer->graphicsDescriptorSetOffsets[1])
    };
    vki.vkCmdPushConstants(grCmdBuffer->commandBuffer, grPipeline->pipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, size, descSetBuffer);
    VkFramebuffer framebuffer =
        getVkFramebuffer(grCmdBuffer->grDevice->device, grPipeline->renderPass,
                         grCmdBuffer->attachmentCount, grCmdBuffer->attachments,
                         grCmdBuffer->minExtent2D, grCmdBuffer->minLayerCount);

    const VkRenderPassBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = NULL,
        .renderPass = grPipeline->renderPass,
        .framebuffer = framebuffer,
        .renderArea = (VkRect2D) {
            .offset = { 0, 0 },
            .extent = grCmdBuffer->minExtent2D,
        },
        .clearValueCount = 0,
        .pClearValues = NULL,
    };

    if (grCmdBuffer->hasActiveRenderPass) {
        vki.vkCmdEndRenderPass(grCmdBuffer->commandBuffer);
    }
    //TODO: cache renderpass calls
    vki.vkCmdBeginRenderPass(grCmdBuffer->commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    grCmdBuffer->hasActiveRenderPass = true;

    grCmdBuffer->isDirty = false;
}

static VkDescriptorSet allocateDynamicBindingSet(GrCmdBuffer* grCmdBuffer, VkPipelineBindPoint bindPoint) {
    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pSetLayouts = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? &grCmdBuffer->grDevice->globalDescriptorSet.graphicsDynamicMemoryLayout : &grCmdBuffer->grDevice->globalDescriptorSet.computeDynamicMemoryLayout, 
    };
    VkDescriptorSet outSet;
    if (grCmdBuffer->dynamicBindingPools == NULL || grCmdBuffer->descriptorPoolCount == 0) {
        goto pool_allocation;
    }
    allocInfo.descriptorPool = grCmdBuffer->dynamicBindingPools[grCmdBuffer->descriptorPoolCount - 1];
    VkResult allocResult = vki.vkAllocateDescriptorSets(grCmdBuffer->grDevice->device, &allocInfo, &outSet);
    if (allocResult == VK_SUCCESS) {
        return outSet;
    }
    else if (allocResult != VK_ERROR_FRAGMENTED_POOL && allocResult != VK_ERROR_OUT_OF_POOL_MEMORY) {
        LOGE("failed to properly allocate descriptor sets for dynamic binding: %d\n", allocResult);
        assert(false);
    }
pool_allocation:
    LOGT("Allocating a new descriptor pool for dynamic binding for buffer %p\n", grCmdBuffer);
    // TODO: make "vector" more efficient
    grCmdBuffer->dynamicBindingPools = (VkDescriptorPool*)realloc(grCmdBuffer->dynamicBindingPools, sizeof(VkDescriptorPool) * grCmdBuffer->descriptorPoolCount);
    const unsigned dynamicDescriptorCount = 128;
    const VkDescriptorPoolSize poolSizes[2] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            .descriptorCount = dynamicDescriptorCount,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            .descriptorCount = dynamicDescriptorCount,
        }
    };
    const VkDescriptorPoolCreateInfo poolCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .maxSets = dynamicDescriptorCount,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    VkDescriptorPool tempPool;
    if (vki.vkCreateDescriptorPool(grCmdBuffer->grDevice->device, &poolCreateInfo, NULL,
                                   &tempPool) != VK_SUCCESS) {
        LOGE("vkCreateDescriptorPool for dynamic binding failed\n");
        assert(false);
    }
    grCmdBuffer->dynamicBindingPools[grCmdBuffer->descriptorPoolCount++] = tempPool;
    allocInfo.descriptorPool = tempPool;
    if (vki.vkAllocateDescriptorSets(grCmdBuffer->grDevice->device, &allocInfo, &outSet) != VK_SUCCESS) {
        LOGE("vkAllocateDescriptorSets failed for created descriptor pool\n");
        assert(false);
    }
    return outSet;
}

static void initDynamicBuffers(GrCmdBuffer* grCmdBuffer, VkPipelineBindPoint bindPoint)
{
    grCmdBuffer->dynamicMemoryViews = (VkBufferView*)realloc(grCmdBuffer->dynamicMemoryViews, sizeof(VkBufferView) * (1 + grCmdBuffer->dynamicBufferViewsCount));
    const VkBufferViewCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .pNext = NULL,
        .buffer = ((GrGpuMemory*)(grCmdBuffer->graphicsBufferInfo.mem))->buffer,
         .format = getVkFormat(grCmdBuffer->graphicsBufferInfo.format),
         .offset = grCmdBuffer->graphicsBufferInfo.offset,
         .range = grCmdBuffer->graphicsBufferInfo.range,
    };
    VkBufferView bufferView;
    assert(vki.vkCreateBufferView(grCmdBuffer->grDevice->device, &createInfo, NULL, &bufferView) == VK_SUCCESS);
    grCmdBuffer->dynamicMemoryViews[grCmdBuffer->dynamicBufferViewsCount] = bufferView;

    bool pushDescriptorsSupported = grCmdBuffer->grDevice->pushDescriptorSetSupported;
    VkDescriptorSet writeSet = pushDescriptorsSupported ? VK_NULL_HANDLE : allocateDynamicBindingSet(grCmdBuffer, bindPoint);
    VkWriteDescriptorSet writeDescriptorSet[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = writeSet,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            .pImageInfo = NULL,
            .pBufferInfo = NULL,
            .pTexelBufferView = &bufferView,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = writeSet,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            .pImageInfo = NULL,
            .pBufferInfo = NULL,
            .pTexelBufferView = &bufferView,
        }
    };
    VkPipelineLayout layout = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? grCmdBuffer->grDevice->pipelineLayouts.graphicsPipelineLayout : grCmdBuffer->grDevice->pipelineLayouts.computePipelineLayout;
    if (pushDescriptorsSupported) {
        vki.vkCmdPushDescriptorSetKHR(grCmdBuffer->commandBuffer, bindPoint,
                                      layout,
                                      1,//TODO: move in define upwards
                                      2, writeDescriptorSet);
    }
    else {
        vki.vkUpdateDescriptorSets(grCmdBuffer->grDevice->device, 2, writeDescriptorSet, 0, NULL);
        vki.vkCmdBindDescriptorSets(grCmdBuffer->commandBuffer, bindPoint,
                                    layout, 1, 1, &writeSet, 0, NULL);
    }
    grCmdBuffer->dynamicBufferViewsCount++;
    grCmdBuffer->isDynamicBufferDirty = false;//TODO:change different flags for compute pipeline
}

// Command Buffer Building Functions

GR_VOID grCmdBindPipeline(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_PIPELINE pipeline)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, pipelineBindPoint, pipeline);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrPipeline* grPipeline = (GrPipeline*)pipeline;

    if (pipelineBindPoint != GR_PIPELINE_BIND_POINT_GRAPHICS) {
        LOGW("unsupported bind point 0x%x\n", pipelineBindPoint);
    }

    grCmdBuffer->grPipeline = grPipeline;
    grCmdBuffer->isDirty = true;
}

GR_VOID grCmdBindStateObject(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM stateBindPoint,
    GR_STATE_OBJECT state)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, stateBindPoint, state);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrViewportStateObject* viewportState = (GrViewportStateObject*)state;
    GrRasterStateObject* rasterState = (GrRasterStateObject*)state;
    GrDepthStencilStateObject* depthStencilState = (GrDepthStencilStateObject*)state;
    GrColorBlendStateObject* colorBlendState = (GrColorBlendStateObject*)state;

    switch ((GR_STATE_BIND_POINT)stateBindPoint) {
    case GR_STATE_BIND_VIEWPORT:
        vki.vkCmdSetViewportWithCountEXT(grCmdBuffer->commandBuffer,
                                         viewportState->viewportCount, viewportState->viewports);
        vki.vkCmdSetScissorWithCountEXT(grCmdBuffer->commandBuffer, viewportState->scissorCount,
                                        viewportState->scissors);
        break;
    case GR_STATE_BIND_RASTER:
        vki.vkCmdSetCullModeEXT(grCmdBuffer->commandBuffer, rasterState->cullMode);
        vki.vkCmdSetFrontFaceEXT(grCmdBuffer->commandBuffer, rasterState->frontFace);
        vki.vkCmdSetDepthBias(grCmdBuffer->commandBuffer, rasterState->depthBiasConstantFactor,
                              rasterState->depthBiasClamp, rasterState->depthBiasSlopeFactor);
        break;
    case GR_STATE_BIND_DEPTH_STENCIL:
        vki.vkCmdSetDepthTestEnableEXT(grCmdBuffer->commandBuffer,
                                       depthStencilState->depthTestEnable);
        vki.vkCmdSetDepthWriteEnableEXT(grCmdBuffer->commandBuffer,
                                        depthStencilState->depthWriteEnable);
        vki.vkCmdSetDepthCompareOpEXT(grCmdBuffer->commandBuffer,
                                      depthStencilState->depthCompareOp);
        vki.vkCmdSetDepthBoundsTestEnableEXT(grCmdBuffer->commandBuffer,
                                             depthStencilState->depthBoundsTestEnable);
        vki.vkCmdSetStencilTestEnableEXT(grCmdBuffer->commandBuffer,
                                         depthStencilState->stencilTestEnable);
        vki.vkCmdSetStencilOpEXT(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                 depthStencilState->front.failOp,
                                 depthStencilState->front.passOp,
                                 depthStencilState->front.depthFailOp,
                                 depthStencilState->front.compareOp);
        vki.vkCmdSetStencilCompareMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                       depthStencilState->front.compareMask);
        vki.vkCmdSetStencilWriteMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                     depthStencilState->front.writeMask);
        vki.vkCmdSetStencilReference(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                     depthStencilState->front.reference);
        vki.vkCmdSetStencilOpEXT(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                 depthStencilState->back.failOp,
                                 depthStencilState->back.passOp,
                                 depthStencilState->back.depthFailOp,
                                 depthStencilState->back.compareOp);
        vki.vkCmdSetStencilCompareMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                       depthStencilState->back.compareMask);
        vki.vkCmdSetStencilWriteMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                     depthStencilState->back.writeMask);
        vki.vkCmdSetStencilReference(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                     depthStencilState->back.reference);
        vki.vkCmdSetDepthBounds(grCmdBuffer->commandBuffer,
                                depthStencilState->minDepthBounds, depthStencilState->maxDepthBounds);
        break;
    case GR_STATE_BIND_COLOR_BLEND:
        vki.vkCmdSetBlendConstants(grCmdBuffer->commandBuffer, colorBlendState->blendConstants);
        break;
    case GR_STATE_BIND_MSAA:
        // TODO
        break;
    }
}

GR_VOID grCmdBindDescriptorSet(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_UINT index,
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT slotOffset)
{
    LOGT("%p 0x%X %u %p %u\n", cmdBuffer, pipelineBindPoint, index,  descriptorSet, slotOffset);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    assert(index < 2);
    if (pipelineBindPoint == GR_PIPELINE_BIND_POINT_GRAPHICS) {
        grCmdBuffer->graphicsDescriptorSetOffsets[index] = slotOffset;
        grCmdBuffer->graphicsDescriptorSets[index] = grDescriptorSet;
        grCmdBuffer->isDirty = true;
    } else {
        grCmdBuffer->computeDescriptorSetOffsets[index] = slotOffset;
        grCmdBuffer->computeDescriptorSets[index] = grDescriptorSet;
    }

}

GR_VOID grCmdBindDynamicMemoryView(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    const GR_MEMORY_VIEW_ATTACH_INFO* pMemView)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, pipelineBindPoint, pMemView);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    if (pipelineBindPoint == GR_PIPELINE_BIND_POINT_GRAPHICS && memcmp(pMemView, &grCmdBuffer->graphicsBufferInfo, sizeof(GR_MEMORY_VIEW_ATTACH_INFO)) != 0) {
        memcpy(&grCmdBuffer->graphicsBufferInfo, pMemView, sizeof(GR_MEMORY_VIEW_ATTACH_INFO));
        grCmdBuffer->isDynamicBufferDirty = true;
    }
    else if (memcmp(pMemView, &grCmdBuffer->computeBufferInfo, sizeof(GR_MEMORY_VIEW_ATTACH_INFO)) != 0) {
        memcpy(&grCmdBuffer->computeBufferInfo, pMemView, sizeof(GR_MEMORY_VIEW_ATTACH_INFO));
        //TODO: add dirty flag
    }
}

GR_VOID grCmdPrepareMemoryRegions(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT transitionCount,
    const GR_MEMORY_STATE_TRANSITION* pStateTransitions)
{
    LOGT("%p %u %p\n", cmdBuffer, transitionCount, pStateTransitions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    for (int i = 0; i < transitionCount; i++) {
        const GR_MEMORY_STATE_TRANSITION* stateTransition = &pStateTransitions[i];

        // TODO use buffer memory barrier
        const VkMemoryBarrier memoryBarrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = getVkAccessFlagsMemory(stateTransition->oldState),
            .dstAccessMask = getVkAccessFlagsMemory(stateTransition->newState),
        };

        // TODO batch
        vki.vkCmdPipelineBarrier(grCmdBuffer->commandBuffer,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                                 0, 1, &memoryBarrier, 0, NULL, 0, NULL);
    }
}

// FIXME what are target states for?
// TODO handle depth target
GR_VOID grCmdBindTargets(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT colorTargetCount,
    const GR_COLOR_TARGET_BIND_INFO* pColorTargets,
    const GR_DEPTH_STENCIL_BIND_INFO* pDepthTarget)
{
    LOGT("%p %u %p %p\n", cmdBuffer, colorTargetCount, pColorTargets, pDepthTarget);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    if (pDepthTarget != NULL) {
        LOGW("unhandled depth target\n");
    }

    // Find minimum extent and layer count
    grCmdBuffer->minExtent2D = (VkExtent2D) { UINT32_MAX, UINT32_MAX };
    grCmdBuffer->minLayerCount = UINT32_MAX;

    for (int i = 0; i < colorTargetCount; i++) {
        const GrColorTargetView* grColorTargetView = (GrColorTargetView*)pColorTargets[i].view;

        if (grColorTargetView != NULL) {
            grCmdBuffer->minExtent2D.width = MIN(grCmdBuffer->minExtent2D.width,
                                                 grColorTargetView->extent.width);
            grCmdBuffer->minExtent2D.height = MIN(grCmdBuffer->minExtent2D.height,
                                                  grColorTargetView->extent.height);
            grCmdBuffer->minLayerCount = MIN(grCmdBuffer->minLayerCount,
                                             grColorTargetView->layerCount);
        }
    }

    // Copy attachments
    grCmdBuffer->attachmentCount = 0;

    for (int i = 0; i < colorTargetCount; i++) {
        const GrColorTargetView* grColorTargetView = (GrColorTargetView*)pColorTargets[i].view;

        if (grColorTargetView != NULL) {
            grCmdBuffer->attachments[grCmdBuffer->attachmentCount] = grColorTargetView->imageView;
            grCmdBuffer->attachmentCount++;
        }
    }
}

GR_VOID grCmdPrepareImages(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT transitionCount,
    const GR_IMAGE_STATE_TRANSITION* pStateTransitions)
{
    LOGT("%p %u %p\n", cmdBuffer, transitionCount, pStateTransitions);
    const GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    for (int i = 0; i < transitionCount; i++) {
        const GR_IMAGE_STATE_TRANSITION* stateTransition = &pStateTransitions[i];
        const GR_IMAGE_SUBRESOURCE_RANGE* range = &stateTransition->subresourceRange;
        GrImage* grImage = (GrImage*)stateTransition->image;

        const VkImageMemoryBarrier imageMemoryBarrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = getVkAccessFlagsImage(stateTransition->oldState),
            .dstAccessMask = getVkAccessFlagsImage(stateTransition->newState),
            .oldLayout = getVkImageLayout(stateTransition->oldState),
            .newLayout = getVkImageLayout(stateTransition->newState),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = grImage->image,
            .subresourceRange = getVkImageSubresourceRange(range),
        };

        // TODO batch
        vki.vkCmdPipelineBarrier(grCmdBuffer->commandBuffer,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                                 0, 0, NULL, 0, NULL, 1, &imageMemoryBarrier);
    }
}

GR_VOID grCmdDraw(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT firstVertex,
    GR_UINT vertexCount,
    GR_UINT firstInstance,
    GR_UINT instanceCount)
{
    LOGT("%p %u %u %u %u\n", cmdBuffer, firstVertex, vertexCount, firstInstance, instanceCount);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    if (grCmdBuffer->isDirty) {
        initCmdBufferResources(grCmdBuffer);
    }
    if (grCmdBuffer->isDynamicBufferDirty) {
        initDynamicBuffers(grCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }
    vki.vkCmdDraw(grCmdBuffer->commandBuffer,
                  vertexCount, instanceCount, firstVertex, firstInstance);
}

GR_VOID grCmdDrawIndexed(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT firstIndex,
    GR_UINT indexCount,
    GR_INT vertexOffset,
    GR_UINT firstInstance,
    GR_UINT instanceCount)
{
    LOGT("%p %u %u %d %u %u\n",
         cmdBuffer, firstIndex, indexCount, vertexOffset, firstInstance, instanceCount);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    if (grCmdBuffer->isDirty) {
        initCmdBufferResources(grCmdBuffer);
    }
    if (grCmdBuffer->isDynamicBufferDirty) {
        initDynamicBuffers(grCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }
    vki.vkCmdDrawIndexed(grCmdBuffer->commandBuffer,
                         indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

GR_VOID grCmdClearColorImage(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    const GR_FLOAT color[4],
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %g %g %g %g %u %p\n",
         cmdBuffer, image, color[0], color[1], color[2], color[3], rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrImage* grImage = (GrImage*)image;

    const VkClearColorValue vkColor = {
        .float32 = { color[0], color[1], color[2], color[3] },
    };

    VkImageSubresourceRange* vkRanges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
    for (int i = 0; i < rangeCount; i++) {
        vkRanges[i] = getVkImageSubresourceRange(&pRanges[i]);
    }

    vki.vkCmdClearColorImage(grCmdBuffer->commandBuffer, grImage->image,
                             getVkImageLayout(GR_IMAGE_STATE_CLEAR),
                             &vkColor, rangeCount, vkRanges);

    free(vkRanges);
}

GR_VOID grCmdClearColorImageRaw(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    const GR_UINT32 color[4],
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %u %u %u %u %u %p\n",
         cmdBuffer, image, color[0], color[1], color[2], color[3], rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrImage* grImage = (GrImage*)image;

    const VkClearColorValue vkColor = {
        .uint32 = { color[0], color[1], color[2], color[3] },
    };

    VkImageSubresourceRange* vkRanges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
    for (int i = 0; i < rangeCount; i++) {
        vkRanges[i] = getVkImageSubresourceRange(&pRanges[i]);
    }

    vki.vkCmdClearColorImage(grCmdBuffer->commandBuffer, grImage->image,
                             getVkImageLayout(GR_IMAGE_STATE_CLEAR),
                             &vkColor, rangeCount, vkRanges);

    free(vkRanges);
}

GR_VOID grCmdClearDepthStencil(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    GR_FLOAT depth,
    GR_UINT8 stencil,
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %f 0x%hhX %u %p\n", cmdBuffer, image, depth, stencil, rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrImage* grImage = (GrImage*)image;

    VkClearDepthStencilValue depthStencil = {
        .depth = depth,
        .stencil = (uint32_t) stencil,
    };
    VkImageSubresourceRange *vkRanges = (VkImageSubresourceRange*)malloc(sizeof(VkImageSubresourceRange) * rangeCount);
    for (GR_UINT i = 0; i < rangeCount; ++i) {
        vkRanges[i] = getVkImageSubresourceRange(&pRanges[i]);
    }
    vki.vkCmdClearDepthStencilImage(grCmdBuffer->commandBuffer, grImage->image, getVkImageLayout(GR_IMAGE_STATE_CLEAR), &depthStencil, rangeCount, vkRanges);
    free(vkRanges);
}

GR_VOID grCmdSetEvent(
    GR_CMD_BUFFER cmdBuffer,
    GR_EVENT event)
{
    LOGT("%p %p\n", cmdBuffer, event);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrEvent* grEvent = (GrEvent*)event;
    vki.vkCmdSetEvent(grCmdBuffer->commandBuffer, grEvent->event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

GR_VOID grCmdResetEvent(
    GR_CMD_BUFFER cmdBuffer,
    GR_EVENT event)
{
    LOGT("%p %p\n", cmdBuffer, event);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrEvent* grEvent = (GrEvent*)event;
    vki.vkCmdResetEvent(grCmdBuffer->commandBuffer, grEvent->event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

GR_VOID grCmdBeginQuery(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT slot,
    GR_FLAGS flags)
{
    LOGT("%p %p %u 0x%X\n", cmdBuffer, queryPool, slot, flags);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrQueryPool* grQueryPool = (GrQueryPool*)queryPool;
    vki.vkCmdBeginQuery(grCmdBuffer->commandBuffer, grQueryPool->pool, slot, (GR_QUERY_IMPRECISE_DATA & flags) ? 0 : VK_QUERY_CONTROL_PRECISE_BIT); // basically inverse of vulkan
}

GR_VOID grCmdEndQuery(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT slot)
{
    LOGT("%p %p %u\n", cmdBuffer, queryPool, slot);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrQueryPool* grQueryPool = (GrQueryPool*)queryPool;
    vki.vkCmdEndQuery(grCmdBuffer->commandBuffer, grQueryPool->pool, slot);
}

GR_VOID grCmdResetQueryPool(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT startQuery,
    GR_UINT queryCount)
{
    LOGT("%p %p %u %u\n", cmdBuffer, queryPool, startQuery, queryCount);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrQueryPool* grQueryPool = (GrQueryPool*)queryPool;
    vki.vkCmdResetQueryPool(grCmdBuffer->commandBuffer, grQueryPool->pool, startQuery, queryCount);
}

GR_VOID grCmdWriteTimestamp(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM timestampType,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset)
{
    LOGT("%p 0x%X %p %lu\n", cmdBuffer, timestampType, destMem, destOffset);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrGpuMemory* grMemory = (GrGpuMemory*)destMem;
    if (grCmdBuffer->timestampQueryPool == VK_NULL_HANDLE) {
        // create a new one lazily
         VkQueryPoolCreateInfo createInfo = {
             .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
             .pNext = NULL,
             .flags = 0,
             .queryType = VK_QUERY_TYPE_TIMESTAMP,
             .queryCount = 1,
             .pipelineStatistics = 0,
         };
         VkResult res = vki.vkCreateQueryPool(grMemory->device, &createInfo, NULL, &grCmdBuffer->timestampQueryPool);
         if (res != VK_SUCCESS) {
             LOGE("Failed to create a VkQueryPool for command buffer %p\n", cmdBuffer);
             grCmdBuffer->timestampQueryPool = VK_NULL_HANDLE;
         }
    }
    if (grCmdBuffer->timestampQueryPool == VK_NULL_HANDLE) {
        // do nothing for now (could have crash the program as well heh)
        return;
    }
    VkPipelineStageFlags stageFlag = timestampType == GR_TIMESTAMP_TOP ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vki.vkCmdWriteTimestamp( grCmdBuffer->commandBuffer, stageFlag, grCmdBuffer->timestampQueryPool, 0);
    vki.vkCmdCopyQueryPoolResults( grCmdBuffer->commandBuffer, grCmdBuffer->timestampQueryPool, 0, 1, grMemory->buffer, destOffset, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
}
