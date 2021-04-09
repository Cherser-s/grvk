#include "mantle_internal.h"
#include "amdilc.h"

typedef struct _Stage {
    const GR_PIPELINE_SHADER* shader;
    VkShaderStageFlagBits flags;
} Stage;

static VkRenderPass getVkRenderPass(
    const VkDevice vkDevice,
    const GR_PIPELINE_CB_TARGET_STATE* cbTargets,
    const GR_PIPELINE_DB_STATE* dbTarget)
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkAttachmentDescription descriptions[GR_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference colorReferences[GR_MAX_COLOR_TARGETS];
    VkAttachmentReference depthStencilReference;
    unsigned descriptionCount = 0;
    unsigned colorReferenceCount = 0;
    bool hasDepthStencil = false;

    for (int i = 0; i < GR_MAX_COLOR_TARGETS; i++) {
        const GR_PIPELINE_CB_TARGET_STATE* target = &cbTargets[i];
        VkFormat vkFormat = getVkFormat(target->format);

        if (vkFormat == VK_FORMAT_UNDEFINED) {
            continue;
        }

        descriptions[descriptionCount] = (VkAttachmentDescription) {
            .flags = 0,
            .format = vkFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = (target->channelWriteMask & 0xF) != 0 ?
                       VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        colorReferences[colorReferenceCount] = (VkAttachmentReference) {
            .attachment = descriptionCount,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        descriptionCount++;
        colorReferenceCount++;
    }

    VkFormat dbVkFormat = getVkFormat(dbTarget->format);
    if (dbVkFormat != VK_FORMAT_UNDEFINED) {
        // Table 10 in the API reference
        bool hasDepth = dbTarget->format.channelFormat == GR_CH_FMT_R16 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R32 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R16G8 ||
                        dbTarget->format.channelFormat == GR_CH_FMT_R32G8;
        bool hasStencil = dbTarget->format.channelFormat == GR_CH_FMT_R8 ||
                          dbTarget->format.channelFormat == GR_CH_FMT_R16G8 ||
                          dbTarget->format.channelFormat == GR_CH_FMT_R32G8;

        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (hasDepth && hasStencil) {
            layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else if (hasDepth && !hasStencil) {
            layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        } else if (!hasDepth && hasStencil) {
            layout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
        }

        descriptions[descriptionCount] = (VkAttachmentDescription) {
            .flags = 0,
            .format = dbVkFormat,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = hasDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = hasDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = hasStencil ?
                             VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = hasStencil ?
                             VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = layout,
            .finalLayout = layout,
        };

        depthStencilReference = (VkAttachmentReference) {
            .attachment = descriptionCount,
            .layout = layout,
        };

        descriptionCount++;
        hasDepthStencil = true;
    }

    const VkSubpassDescription subpass = {
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = NULL,
        .colorAttachmentCount = colorReferenceCount,
        .pColorAttachments = colorReferences,
        .pResolveAttachments = NULL,
        .pDepthStencilAttachment = hasDepthStencil ? &depthStencilReference : NULL,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = NULL,
    };

    const VkRenderPassCreateInfo renderPassCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .attachmentCount = descriptionCount,
        .pAttachments = descriptions,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = NULL,
    };

    if (vki.vkCreateRenderPass(vkDevice, &renderPassCreateInfo, NULL, &renderPass) != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed\n");
        return VK_NULL_HANDLE;
    }

    return renderPass;
}

// Shader and Pipeline Functions

GR_RESULT grCreateShader(
    GR_DEVICE device,
    const GR_SHADER_CREATE_INFO* pCreateInfo,
    GR_SHADER* pShader)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pShader);
    GrDevice* grDevice = (GrDevice*)device;
    if (grDevice == NULL) {
        return GR_ERROR_INVALID_HANDLE;
    }
    if (grDevice->sType != GR_STRUCT_TYPE_DEVICE) {
        return GR_ERROR_INVALID_OBJECT_TYPE;
    }
    if (pCreateInfo == NULL || pShader == NULL || pCreateInfo->pCode == NULL) {
        return GR_ERROR_INVALID_POINTER;
    }
    if ((pCreateInfo->flags & GR_SHADER_CREATE_ALLOW_RE_Z) != 0) {
        LOGW("unhandled Re-Z flag\n");
    }
    GrShader* grShader = malloc(sizeof(GrShader));
    grShader->sType = GR_STRUCT_TYPE_SHADER;
    if (grShader == NULL) {
        return GR_ERROR_OUT_OF_MEMORY;
    }
    grShader->isPrecompiledSpv = (pCreateInfo->flags & GR_SHADER_CREATE_SPIRV) != 0;
    if (grShader->isPrecompiledSpv) {
        const VkShaderModuleCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .codeSize = pCreateInfo->codeSize,
            .pCode = (uint32_t*)pCreateInfo->pCode,
        };
        // idk whether to free spirvCode in case if shader compiles correctly
        if (vki.vkCreateShaderModule(grDevice->device, &createInfo, NULL, &grShader->precompiledModule)) {
            free(grShader);
            LOGE("vkCreateShaderModule failed\n");
            return GR_ERROR_OUT_OF_MEMORY;
        }
    } else {
        grShader->codeSize = pCreateInfo->codeSize;
        grShader->code = (uint32_t*)malloc(pCreateInfo->codeSize);

        if (grShader->code == NULL) {
            return GR_ERROR_OUT_OF_MEMORY;
        }
        memcpy(grShader->code, pCreateInfo->pCode, pCreateInfo->codeSize);
    }

    *pShader = (GR_SHADER)grShader;
    return GR_SUCCESS;
}

void calculateDescriptorSetBindingCount(const GR_DESCRIPTOR_SET_MAPPING** mappings, uint32_t mappingCount, uint32_t *outDescriptorCount, uint32_t* outDescriptorSetCount)
{
    uint32_t maxDescriptorCount = 0;
    uint32_t nestedDescriptorSetCount = 0;
    const GR_DESCRIPTOR_SET_MAPPING* nestedDescriptorSets[5];
    for (uint32_t i = 0; i < mappingCount; i++) {
        if (mappings[i]->descriptorCount > maxDescriptorCount) {
            maxDescriptorCount = mappings[i]->descriptorCount;
        }
    }
    *outDescriptorCount = maxDescriptorCount;
    *outDescriptorSetCount = 1;
    for (uint32_t i = 0; i < maxDescriptorCount; ++i) {
        GR_ENUM slotType = GR_SLOT_UNUSED;
        for (unsigned j = 0; j < mappingCount; ++j) {
            if (slotType != GR_SLOT_UNUSED && mappings[j]->pDescriptorInfo[i].slotObjectType != GR_SLOT_UNUSED && mappings[j]->pDescriptorInfo[i].slotObjectType != slotType) {
                LOGE("Descriptor slot %d is different for different stages\n", i);
                return;//TODO: add some errors
            }
            switch (mappings[j]->pDescriptorInfo[i].slotObjectType) {
            case GR_SLOT_NEXT_DESCRIPTOR_SET: 
                nestedDescriptorSets[nestedDescriptorSetCount++] = mappings[j]->pDescriptorInfo[i].pNextLevelSet;
            case GR_SLOT_SHADER_RESOURCE:
            case GR_SLOT_SHADER_UAV:
            case GR_SLOT_SHADER_SAMPLER:
                slotType = mappings[j]->pDescriptorInfo[i].slotObjectType;
                break;
            default:
                break;
            }
        }

        unsigned descriptorCount = 0;
        unsigned descriptorSetCount = 0;
        if (slotType == GR_SLOT_NEXT_DESCRIPTOR_SET) {
            calculateDescriptorSetBindingCount(nestedDescriptorSets, nestedDescriptorSetCount, &descriptorCount, &descriptorSetCount);
            *outDescriptorCount += descriptorCount;
            *outDescriptorSetCount += descriptorSetCount;
        }
    }
}

GR_RESULT grCreateGraphicsPipeline(
    GR_DEVICE device,
    const GR_GRAPHICS_PIPELINE_CREATE_INFO* pCreateInfo,
    GR_PIPELINE* pPipeline)
{
    LOGT("%p %p %p\n", device, pCreateInfo, pPipeline);
    GrDevice* grDevice = (GrDevice*)device;

    // Ignored parameters:
    // - iaState.disableVertexReuse (hint)
    // - tessState.optimalTessFactor (hint)
    Stage stages[MAX_STAGE_COUNT];
    uint32_t stageCount = 0;

    if (pCreateInfo->vs.shader != GR_NULL_HANDLE) {
        stages[stageCount++] = (Stage){
            .shader = &pCreateInfo->vs,
            .flags = VK_SHADER_STAGE_VERTEX_BIT
        };
    }
    if (pCreateInfo->hs.shader != GR_NULL_HANDLE) {
        stages[stageCount++] = (Stage){
            .shader = &pCreateInfo->hs,
            .flags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
        };
    }
    if (pCreateInfo->ds.shader != GR_NULL_HANDLE) {
        stages[stageCount++] = (Stage){
            .shader = &pCreateInfo->ds,
            .flags = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
        };
    }
    if (pCreateInfo->gs.shader != GR_NULL_HANDLE) {
        stages[stageCount++] = (Stage){
            .shader = &pCreateInfo->gs,
            .flags = VK_SHADER_STAGE_GEOMETRY_BIT,
        };
    }
    if (pCreateInfo->ps.shader != GR_NULL_HANDLE) {
        stages[stageCount++] = (Stage){
            .shader = &pCreateInfo->ps,
            .flags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    }

    VkPipelineShaderStageCreateInfo shaderStageCreateInfo[MAX_STAGE_COUNT];

    for (int i = 0; i < stageCount; i++) {
        Stage* stage = &stages[i];

        if (stage->shader->linkConstBufferCount > 0) {
            // TODO implement
            LOGW("link-time constant buffers are not implemented\n");
        }

        if (stage->shader->dynamicMemoryViewMapping.slotObjectType != GR_SLOT_UNUSED) {
            // TODO implement
            LOGW("dynamic memory view mapping is not implemented\n");
        }

        GrShader* grShader = (GrShader*)stage->shader->shader;

        if (grShader->isPrecompiledSpv) {
            shaderStageCreateInfo[i] = (VkPipelineShaderStageCreateInfo) {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = NULL,
                .flags = 0,
                .stage = stage->flags,
                .module = grShader->precompiledModule,
                .pName = "main",
                .pSpecializationInfo = NULL,
            };
        }
        else {
            uint32_t codeSize;
            uint32_t* codeCopy = ilcCompileShader(&codeSize, stage->shader, grShader->code, grShader->codeSize);
            if (codeCopy == NULL) {
                return GR_ERROR_OUT_OF_MEMORY;
            }

            const VkShaderModuleCreateInfo createInfo = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .pNext = NULL,
                .flags = 0,
                .codeSize = codeSize,
                .pCode = codeCopy
            };
            // idk whether to free spirvCode in case if shader compiles correctly
            VkShaderModule module;
            VkResult res = vki.vkCreateShaderModule(grDevice->device, &createInfo, NULL, &module);
            free(codeCopy);
            if (res != VK_SUCCESS) {
                LOGE("vkCreateShaderModule failed\n");
                return GR_ERROR_OUT_OF_MEMORY;
            }
            shaderStageCreateInfo[i] = (VkPipelineShaderStageCreateInfo) {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = NULL,
                .flags = 0,
                .stage = stage->flags,
                .module = module,
                .pName = "main",
                .pSpecializationInfo = NULL,
            };
        }
    }
    const VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = NULL,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = NULL,
    };

    const VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .topology = getVkPrimitiveTopology(pCreateInfo->iaState.topology),
        .primitiveRestartEnable = VK_FALSE,
    };

    // Ignored if no tessellation shaders are present
    const VkPipelineTessellationStateCreateInfo tessellationStateCreateInfo =  {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .patchControlPoints = pCreateInfo->tessState.patchControlPoints,
    };

    const VkPipelineViewportStateCreateInfo viewportStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .viewportCount = 0, // Dynamic state
        .pViewports = NULL, // Dynamic state
        .scissorCount = 0, // Dynamic state
        .pScissors = NULL, // Dynamic state
    };

    const VkPipelineRasterizationDepthClipStateCreateInfoEXT depthClipStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,
        .pNext = NULL,
        .flags = 0,
        .depthClipEnable = pCreateInfo->rsState.depthClipEnable,
    };

    const VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = &depthClipStateCreateInfo,
        .flags = 0,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL, // TODO implement wireframe
        .cullMode = 0, // Dynamic state
        .frontFace = 0, // Dynamic state
        .depthBiasEnable = VK_TRUE,
        .depthBiasConstantFactor = 0.f, // Dynamic state
        .depthBiasClamp = 0.f, // Dynamic state
        .depthBiasSlopeFactor = 0.f, // Dynamic state
        .lineWidth = 1.f,
    };

    const VkPipelineMultisampleStateCreateInfo msaaStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, // TODO implement MSAA
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.f,
        .pSampleMask = NULL, // TODO implement MSAA
        .alphaToCoverageEnable = pCreateInfo->cbState.alphaToCoverageEnable ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    const VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .depthTestEnable = 0, // Dynamic state
        .depthWriteEnable = 0, // Dynamic state
        .depthCompareOp = 0, // Dynamic state
        .depthBoundsTestEnable = 0, // Dynamic state
        .stencilTestEnable = 0, // Dynamic state
        .front = { 0 }, // Dynamic state
        .back = { 0 }, // Dynamic state
        .minDepthBounds = 0.f, // Dynamic state
        .maxDepthBounds = 0.f, // Dynamic state
    };

    // TODO implement
    if (pCreateInfo->cbState.dualSourceBlendEnable) {
        LOGW("dual source blend is not implemented\n");
    }

    unsigned attachmentCount = 0;
    VkPipelineColorBlendAttachmentState attachments[GR_MAX_COLOR_TARGETS];

    for (int i = 0; i < GR_MAX_COLOR_TARGETS; i++) {
        const GR_PIPELINE_CB_TARGET_STATE* target = &pCreateInfo->cbState.target[i];

        if (!target->blendEnable &&
            target->format.channelFormat == GR_CH_FMT_UNDEFINED &&
            target->format.numericFormat == GR_NUM_FMT_UNDEFINED &&
            target->channelWriteMask == 0) {
            continue;
        }

        if (target->blendEnable) {
            // TODO implement blend settings
            attachments[attachmentCount] = (VkPipelineColorBlendAttachmentState) {
                .blendEnable = VK_TRUE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = getVkColorComponentFlags(target->channelWriteMask),
            };
        } else {
            attachments[attachmentCount] = (VkPipelineColorBlendAttachmentState) {
                .blendEnable = VK_FALSE,
                .srcColorBlendFactor = 0, // Ignored
                .dstColorBlendFactor = 0, // Ignored
                .colorBlendOp = 0, // Ignored
                .srcAlphaBlendFactor = 0, // Ignored
                .dstAlphaBlendFactor = 0, // Ignored
                .alphaBlendOp = 0, // Ignored
                .colorWriteMask = getVkColorComponentFlags(target->channelWriteMask),
            };
        }

        attachmentCount++;
    }

    const VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .logicOpEnable = VK_TRUE,
        .logicOp = getVkLogicOp(pCreateInfo->cbState.logicOp),
        .attachmentCount = attachmentCount,
        .pAttachments = attachments,
        .blendConstants = { 0.f }, // Dynamic state
    };

    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE_EXT,
        VK_DYNAMIC_STATE_FRONT_FACE_EXT,
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
        VK_DYNAMIC_STATE_STENCIL_OP_EXT,
    };

    const VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .dynamicStateCount = sizeof(dynamicStates) / sizeof(VkDynamicState),
        .pDynamicStates = dynamicStates,
    };

    VkRenderPass renderPass = getVkRenderPass(grDevice->device,
                                              pCreateInfo->cbState.target, &pCreateInfo->dbState);
    if (renderPass == VK_NULL_HANDLE)
    {
        return GR_ERROR_OUT_OF_MEMORY;
    }

    VkPipeline vkPipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = grDevice->pipelineLayouts.graphicsPipelineLayout;

    const VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = NULL,
        .flags = (pCreateInfo->flags & GR_PIPELINE_CREATE_DISABLE_OPTIMIZATION) != 0 ?
                 VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
        .stageCount = stageCount,
        .pStages = shaderStageCreateInfo,
        .pVertexInputState = &vertexInputStateCreateInfo,
        .pInputAssemblyState = &inputAssemblyStateCreateInfo,
        .pTessellationState = &tessellationStateCreateInfo,
        .pViewportState = &viewportStateCreateInfo,
        .pRasterizationState = &rasterizationStateCreateInfo,
        .pMultisampleState = &msaaStateCreateInfo,
        .pDepthStencilState = &depthStencilStateCreateInfo,
        .pColorBlendState = &colorBlendStateCreateInfo,
        .pDynamicState = &dynamicStateCreateInfo,
        .layout = layout,
        .renderPass = renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    VkResult result = vki.vkCreateGraphicsPipelines(grDevice->device, VK_NULL_HANDLE, 1,
                                                    &pipelineCreateInfo,
                                                    NULL, &vkPipeline);
    //free remaining shader modules, ""compiled"" only for this pipeline
    for (unsigned i = 0; i < stageCount;++i) {
        GrShader* grShader = (GrShader*)stages[i].shader->shader;
        if (!grShader->isPrecompiledSpv) {
            vki.vkDestroyShaderModule(grDevice->device, shaderStageCreateInfo[i].module, NULL);
        }
    }
    if (result != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed\n");
        if (renderPass != VK_NULL_HANDLE) {
            vki.vkDestroyRenderPass(grDevice->device, renderPass, NULL);
        }
        return GR_ERROR_OUT_OF_MEMORY;
    }

    GrPipeline* grPipeline = malloc(sizeof(GrPipeline));
    if (grPipeline == NULL) {
        return GR_ERROR_OUT_OF_MEMORY;
    }
    *grPipeline = (GrPipeline) {
        .sType = GR_STRUCT_TYPE_PIPELINE,
        .pipeline = vkPipeline,
        .pipelineLayout = layout,
        .renderPass = renderPass
    };

    *pPipeline = (GR_PIPELINE)grPipeline;
    return GR_SUCCESS;
}
