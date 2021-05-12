#include "mantle_internal.h"

static void clearDescriptorSetSlot(
    GrDevice* grDevice,
    DescriptorSetSlot* slot)
{
    if (slot->type == SLOT_TYPE_MEMORY_VIEW && slot->memoryView.vkBufferView != VK_NULL_HANDLE) {
        // FIXME track references
        VKD.vkDestroyBufferView(grDevice->device, slot->memoryView.vkBufferView, NULL);
        slot->memoryView.vkBufferView = VK_NULL_HANDLE;
    }

    slot->type = SLOT_TYPE_NONE;
}

// Descriptor Set Functions

GR_RESULT grCreateDescriptorSet(
    GR_DEVICE device,
    const GR_DESCRIPTOR_SET_CREATE_INFO* pCreateInfo,
    GR_DESCRIPTOR_SET* pDescriptorSet)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pDescriptorSet);
    GrDevice* grDevice = (GrDevice*)device;

    if (grDevice == NULL) {
        return GR_ERROR_INVALID_HANDLE;
    } else if (GET_OBJ_TYPE(grDevice) != GR_OBJ_TYPE_DEVICE) {
        return GR_ERROR_INVALID_OBJECT_TYPE;
    } else if (pCreateInfo == NULL || pDescriptorSet == NULL) {
        return GR_ERROR_INVALID_POINTER;
    }
    GR_RESULT res = GR_SUCCESS;
    VkDescriptorPool hostPool = VK_NULL_HANDLE;
    VkDescriptorSet hostDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding* bindings = NULL;
    if (grDevice->mutableDescriptorsSupported) {
        bindings = (VkDescriptorSetLayoutBinding*)malloc(sizeof(VkDescriptorSetLayoutBinding) * (pCreateInfo->slots + 2) + sizeof(VkMutableDescriptorTypeListVALVE) * pCreateInfo->slots);
        if (bindings == NULL) {
            LOGE("failed to allocate memory for bindings\n");
            res = GR_ERROR_OUT_OF_MEMORY;
            goto bail;
        }
        const VkDescriptorType mutableTypes[] = {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
        const VkMutableDescriptorTypeListVALVE typeList = {
            .descriptorTypeCount = COUNT_OF(mutableTypes),
            .pDescriptorTypes = mutableTypes,
        };
        VkMutableDescriptorTypeListVALVE* typeLists = (((void*)bindings) + sizeof(VkDescriptorSetLayoutBinding) * (pCreateInfo->slots + 2));
        const VkMutableDescriptorTypeCreateInfoVALVE mutableTypeCi = {
            .sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_VALVE,
            .pNext = NULL,
            .mutableDescriptorTypeListCount = pCreateInfo->slots,
            .pMutableDescriptorTypeLists = typeLists,
        };
        const VkDescriptorSetLayoutBinding typeBinding = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_VALVE,
            .descriptorCount = 1,
            .pImmutableSamplers = NULL,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        for (unsigned i = 0; i < pCreateInfo->slots;++i) {
            memcpy(&bindings[i], &typeBinding, sizeof(VkDescriptorSetLayoutBinding));
            bindings[i].binding = i;
            memcpy(&typeLists[i], &typeList, sizeof(VkMutableDescriptorTypeCreateInfoVALVE));
        }
        bindings[pCreateInfo->slots] = (VkDescriptorSetLayoutBinding) {
            .binding = pCreateInfo->slots,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = pCreateInfo->slots,
            .pImmutableSamplers = NULL,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        bindings[pCreateInfo->slots + 1] = (VkDescriptorSetLayoutBinding) {
            .binding = 1 + pCreateInfo->slots,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = pCreateInfo->slots,
            .pImmutableSamplers = NULL,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        const VkDescriptorSetLayoutCreateInfo layoutCi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &mutableTypeCi,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_VALVE,
            .bindingCount = pCreateInfo->slots + 2,
            .pBindings = bindings,
        };
        VkResult vkRes = VKD.vkCreateDescriptorSetLayout(grDevice->device, &layoutCi, NULL, &layout);
        if (vkRes != VK_SUCCESS) {
            LOGE("failed to create descriptor set layout %d\n", vkRes);
            res = getGrResult(vkRes);
            goto bail;
        }

        const VkDescriptorPoolSize poolSizes[] = {
            {
                .type = VK_DESCRIPTOR_TYPE_MUTABLE_VALVE,
                .descriptorCount = pCreateInfo->slots,
            },
            {
                .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = pCreateInfo->slots,
            },
            {
                .type = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = pCreateInfo->slots,
            }
        };
        const VkDescriptorPoolCreateInfo poolCi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .pNext = &mutableTypeCi,
            .flags = VK_DESCRIPTOR_POOL_CREATE_HOST_ONLY_BIT_VALVE,
            .maxSets = 1,
            .poolSizeCount = COUNT_OF(poolSizes),
            .pPoolSizes = poolSizes,
        };
        vkRes = VKD.vkCreateDescriptorPool(grDevice->device, &poolCi, NULL, &hostPool);
        if (vkRes != VK_SUCCESS) {
            LOGE("failed to create descriptor pool, error %d\n", vkRes);
            res = getGrResult(vkRes);
            goto bail;
        }
        const VkDescriptorSetAllocateInfo descSetAllocInfo = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = hostPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout,
        };
        vkRes = VKD.vkAllocateDescriptorSets(grDevice->device, &descSetAllocInfo, &hostDescriptorSet);
        if (vkRes != VK_SUCCESS) {
            LOGE("failed to allocate descriptor set, error %d\n", vkRes);
            res = getGrResult(vkRes);
            goto bail;
        }
    }
    GrDescriptorSet* grDescriptorSet = malloc(sizeof(GrDescriptorSet));
    if (grDescriptorSet == NULL) {
        LOGE("failed to allocate memory for struct\n");
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }
    *grDescriptorSet = (GrDescriptorSet) {
        .grObj = { GR_OBJ_TYPE_DESCRIPTOR_SET, grDevice },
        .slotCount = pCreateInfo->slots,
        .slots = calloc(pCreateInfo->slots, sizeof(DescriptorSetSlot)),
        .hostDescSetLayout = layout,
        .hostPool = hostPool,
        .hostDescriptorSet = hostDescriptorSet,
    };
    if (grDescriptorSet->slots == NULL) {
        LOGE("failed to allocate memory for struct slots\n");
        res = GR_ERROR_OUT_OF_MEMORY;
        goto bail;
    }
    *pDescriptorSet = (GR_DESCRIPTOR_SET)grDescriptorSet;
    if (bindings != NULL) {
        free(bindings);
    }
    return GR_SUCCESS;
bail:
    if (hostPool != VK_NULL_HANDLE) {
        VKD.vkDestroyDescriptorPool(grDevice->device, hostPool, NULL);
    }
    if (layout != VK_NULL_HANDLE) {
        VKD.vkDestroyDescriptorSetLayout(grDevice->device, layout, NULL);
    }
    if (bindings != NULL) {
        free(bindings);
    }
    if (grDescriptorSet != NULL) {
        if (grDescriptorSet->slots != NULL) {
            free(grDescriptorSet->slots);
        }
        free(grDescriptorSet);
    }
    return res;
}

GR_VOID grBeginDescriptorSetUpdate(
    GR_DESCRIPTOR_SET descriptorSet)
{
    LOGT("%p\n", descriptorSet);

    // TODO dirty tracking
}

GR_VOID grEndDescriptorSetUpdate(
    GR_DESCRIPTOR_SET descriptorSet)
{
    LOGT("%p\n", descriptorSet);

    // TODO update descriptors of bound pipelines
}

GR_VOID grAttachSamplerDescriptors(
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT startSlot,
    GR_UINT slotCount,
    const GR_SAMPLER* pSamplers)
{
    LOGT("%p %u %u %p\n", descriptorSet, startSlot, slotCount, pSamplers);
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    GrDevice* grDevice = GET_OBJ_DEVICE(grDescriptorSet);
    bool useMutableDescriptors = grDevice->mutableDescriptorsSupported;

    for (unsigned i = 0; i < slotCount; i++) {
        DescriptorSetSlot* slot = &grDescriptorSet->slots[startSlot + i];
        const GrSampler* grSampler = (GrSampler*)pSamplers[i];

        clearDescriptorSetSlot(grDevice, slot);
        if (useMutableDescriptors) {
            VkDescriptorImageInfo imageInfo = {
                .imageLayout = 0,
                .imageView = VK_NULL_HANDLE,
                .sampler = grSampler->sampler,
            };
            VkWriteDescriptorSet writeSet = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = NULL,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .dstBinding = grDescriptorSet->slotCount + 1,
                .dstArrayElement = startSlot + i,
                .dstSet = grDescriptorSet->hostDescriptorSet,
                .pImageInfo = &imageInfo,
                .pBufferInfo = NULL,
                .pTexelBufferView = NULL,
            };
            VKD.vkUpdateDescriptorSets(grDevice->device, 1, &writeSet, 0, NULL);
        }
        *slot = (DescriptorSetSlot) {
            .type = SLOT_TYPE_SAMPLER,
            .sampler.vkSampler = grSampler->sampler,
        };
    }
}

GR_VOID grAttachImageViewDescriptors(
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT startSlot,
    GR_UINT slotCount,
    const GR_IMAGE_VIEW_ATTACH_INFO* pImageViews)
{
    LOGT("%p %u %u %p\n", descriptorSet, startSlot, slotCount, pImageViews);
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    GrDevice* grDevice = GET_OBJ_DEVICE(grDescriptorSet);
    bool useMutableDescriptors = grDevice->mutableDescriptorsSupported;

    for (unsigned i = 0; i < slotCount; i++) {
        DescriptorSetSlot* slot = &grDescriptorSet->slots[startSlot + i];
        const GR_IMAGE_VIEW_ATTACH_INFO* info = &pImageViews[i];
        const GrImageView* grImageView = (GrImageView*)info->view;

        clearDescriptorSetSlot(grDevice, slot);
        VkImageLayout imageLayout = getVkImageLayout(info->state);
        if (useMutableDescriptors) {
            VkDescriptorImageInfo imageInfo = {
                .imageLayout = imageLayout,
                .imageView = grImageView->imageView,
                .sampler = VK_NULL_HANDLE,
            };
            VkWriteDescriptorSet writeSet = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = NULL,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = 1,
                .dstBinding = startSlot + i,
                .dstArrayElement = 0,
                .dstSet = grDescriptorSet->hostDescriptorSet,
                .pImageInfo = &imageInfo,
                .pBufferInfo = NULL,
                .pTexelBufferView = NULL,
            };
            VKD.vkUpdateDescriptorSets(grDevice->device, 1, &writeSet, 0, NULL);
        }
        *slot = (DescriptorSetSlot) {
            .type = SLOT_TYPE_IMAGE_VIEW,
            .imageView = {
                .vkImageView = grImageView->imageView,
                .vkImageLayout = imageLayout,
            },
        };
    }
}

GR_VOID grAttachMemoryViewDescriptors(
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT startSlot,
    GR_UINT slotCount,
    const GR_MEMORY_VIEW_ATTACH_INFO* pMemViews)
{
    LOGT("%p %u %u %p\n", descriptorSet, startSlot, slotCount, pMemViews);
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    GrDevice* grDevice = GET_OBJ_DEVICE(grDescriptorSet);
    bool useMutableDescriptors = grDevice->mutableDescriptorsSupported;

    for (unsigned i = 0; i < slotCount; i++) {
        DescriptorSetSlot* slot = &grDescriptorSet->slots[startSlot + i];
        const GR_MEMORY_VIEW_ATTACH_INFO* info = &pMemViews[i];
        GrGpuMemory* grGpuMemory = (GrGpuMemory*)info->mem;

        grGpuMemoryBindBuffer(grGpuMemory);
        clearDescriptorSetSlot(grDevice, slot);
        VkBufferView bufferView = VK_NULL_HANDLE;
        VkFormat format = getVkFormat(info->format);
        if (format != VK_FORMAT_UNDEFINED) {
            VkBufferViewCreateInfo bufferViewCreateInfo = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                .pNext = NULL,
                .flags = 0,
                .buffer = grGpuMemory->buffer,
                .format = format,
                .offset = info->offset,
                .range = info->range,
            };
            if (VKD.vkCreateBufferView(grDescriptorSet->grObj.grDevice->device, &bufferViewCreateInfo, NULL,
                                       &bufferView) != VK_SUCCESS) {
                LOGE("vkCreateBufferView failed\n");
                assert(false);
            }
        }

        if (useMutableDescriptors) {
            VkDescriptorBufferInfo bufferInfo = {
                .buffer = grGpuMemory->buffer,
                .offset = info->offset,
                .range  = info->range,
            };
            VkWriteDescriptorSet writeSets[2] = {
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = NULL,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .descriptorCount = 1,
                    .dstBinding = grDescriptorSet->slotCount,
                    .dstArrayElement = startSlot + i,
                    .dstSet = grDescriptorSet->hostDescriptorSet,
                    .pImageInfo = NULL,
                    .pBufferInfo = &bufferInfo,
                    .pTexelBufferView = NULL,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .pNext = NULL,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                    .descriptorCount = 1,
                    .dstBinding = startSlot + i,
                    .dstArrayElement = 0,
                    .dstSet = grDescriptorSet->hostDescriptorSet,
                    .pImageInfo = NULL,
                    .pBufferInfo = NULL,
                    .pTexelBufferView = &bufferView,
                }
            };
            VKD.vkUpdateDescriptorSets(grDevice->device, 1 + ((bufferView != VK_NULL_HANDLE) ? 1 : 0), writeSets, 0, NULL);
        }

        *slot = (DescriptorSetSlot) {
            .type = SLOT_TYPE_MEMORY_VIEW,
            .memoryView = {
                .vkBuffer = grGpuMemory->buffer,
                .vkBufferView = bufferView,
                .vkFormat = format,
                .offset = info->offset,
                .range = info->range,
            },
        };
    }
}

GR_VOID grAttachNestedDescriptors(
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT startSlot,
    GR_UINT slotCount,
    const GR_DESCRIPTOR_SET_ATTACH_INFO* pNestedDescriptorSets)
{
    LOGT("%p %u %u %p\n", descriptorSet, startSlot, slotCount, pNestedDescriptorSets);
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    GrDevice* grDevice = GET_OBJ_DEVICE(grDescriptorSet);

    for (unsigned i = 0; i < slotCount; i++) {
        DescriptorSetSlot* slot = &grDescriptorSet->slots[startSlot + i];
        const GR_DESCRIPTOR_SET_ATTACH_INFO* info = &pNestedDescriptorSets[i];

        clearDescriptorSetSlot(grDevice, slot);

        *slot = (DescriptorSetSlot) {
            .type = SLOT_TYPE_NESTED,
            .nested = {
                .nextSet = (GrDescriptorSet*)info->descriptorSet,
                .slotOffset = info->slotOffset,
            },
        };
    }
}

GR_VOID grClearDescriptorSetSlots(
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT startSlot,
    GR_UINT slotCount)
{
    LOGT("%p %u %u\n", descriptorSet, startSlot, slotCount);
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    GrDevice* grDevice = GET_OBJ_DEVICE(grDescriptorSet);

    for (unsigned i = 0; i < slotCount; i++) {
        DescriptorSetSlot* slot = &grDescriptorSet->slots[startSlot + i];

        clearDescriptorSetSlot(grDevice, slot);
    }
}
