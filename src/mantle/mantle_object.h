#ifndef GR_OBJECT_H_
#define GR_OBJECT_H_

#include "mantle/mantle.h"
#define VK_NO_PROTOTYPES
#include "vulkan/vulkan.h"

#define MAX_STAGE_COUNT 5 // VS, HS, DS, GS, PS

typedef enum _GrStructType {
    GR_STRUCT_TYPE_COMMAND_BUFFER,
    GR_STRUCT_TYPE_COLOR_BLEND_STATE_OBJECT,
    GR_STRUCT_TYPE_COLOR_TARGET_VIEW,
    GR_STRUCT_TYPE_DEPTH_STENCIL_TARGET_VIEW,
    GR_STRUCT_TYPE_DEPTH_STENCIL_STATE_OBJECT,
    GR_STRUCT_TYPE_DESCRIPTOR_SET,
    GR_STRUCT_TYPE_DEVICE,
    GR_STRUCT_TYPE_FENCE,
    GR_STRUCT_TYPE_EVENT,
    GR_STRUCT_TYPE_QUEUE_SEMAPHORE,
    GR_STRUCT_TYPE_GPU_MEMORY,
    GR_STRUCT_TYPE_IMAGE,
    GR_STRUCT_TYPE_IMAGE_VIEW,
    GR_STRUCT_TYPE_MSAA_STATE_OBJECT,
    GR_STRUCT_TYPE_PHYSICAL_GPU,
    GR_STRUCT_TYPE_PIPELINE,
    GR_STRUCT_TYPE_RASTER_STATE_OBJECT,
    GR_STRUCT_TYPE_SAMPLER,
    GR_STRUCT_TYPE_SHADER,
    GR_STRUCT_TYPE_QUEUE,
    GR_STRUCT_TYPE_VIEWPORT_STATE_OBJECT,
    GR_STRUCT_TYPE_QUERY_POOL,
} GrStructType;

typedef struct _GrColorTargetView GrColorTargetView;
typedef struct _GrDescriptorSet GrDescriptorSet;
typedef struct _GrPipeline GrPipeline;
typedef struct _GrDevice GrDevice;
// Generic object used to read the object type
typedef struct _GrObject {
    GrStructType sType;
} GrObject;

typedef struct _GrCmdBuffer {
    GrStructType sType;
    GrDevice* grDevice;
    VkCommandBuffer commandBuffer;
    VkQueryPool timestampQueryPool;
    GrPipeline* grPipeline;
    GrDescriptorSet* graphicsDescriptorSets[2];
    unsigned graphicsDescriptorSetOffsets[2];
    GrDescriptorSet* computeDescriptorSets[2];
    unsigned computeDescriptorSetOffsets[2];
    unsigned attachmentCount;
    VkImageView attachments[GR_MAX_COLOR_TARGETS + 1]; // Extra depth target
    VkExtent2D minExtent2D;
    uint32_t minLayerCount;
    bool hasActiveRenderPass;
    bool isDirty;
} GrCmdBuffer;

typedef struct _GrColorBlendStateObject {
    GrStructType sType;
    float blendConstants[4];
} GrColorBlendStateObject;

typedef struct _GrImageView {
    GrStructType sType;
    VkImageView imageView;
    VkExtent3D extent;
    uint32_t layerCount;
} GrImageView;

typedef struct _GrColorTargetView {
    GrStructType sType;
    VkImageView imageView;
    VkExtent3D extent;
    uint32_t layerCount;
} GrColorTargetView;

typedef struct _GrDepthTargetView {
    GrStructType sType;
    VkImageView imageView;
    VkExtent3D extent;
    uint32_t layerCount;
} GrDepthTargetView;

typedef struct _GrDepthStencilStateObject {
    GrStructType sType;
    VkBool32 depthTestEnable;
    VkBool32 depthWriteEnable;
    VkCompareOp depthCompareOp;
    VkBool32 depthBoundsTestEnable;
    VkBool32 stencilTestEnable;
    VkStencilOpState front;
    VkStencilOpState back;
    float minDepthBounds;
    float maxDepthBounds;
} GrDepthStencilStateObject;

typedef enum _DescriptorSetSlotType
{
    SLOT_TYPE_NONE = 0,
    SLOT_TYPE_IMAGE_VIEW = 1,
    SLOT_TYPE_MEMORY_VIEW = 2,
    SLOT_TYPE_SAMPLER = 3,
    SLOT_TYPE_NESTED = 4,
} DescriptorSetSlotType;

typedef struct _DescriptorSetSlot
{
    DescriptorSetSlotType type;
    unsigned realDescriptorIndex;
    union {
        VkSampler sampler;
        VkImageView imageView;
        VkDeviceAddress nestedDescriptorSet;
    };
    //better separate this as it is owned by the slot
    VkImageLayout imageLayout;
    VkBufferView bufferView;
    VkBufferViewCreateInfo bufferViewCreateInfo;
} DescriptorSetSlot;

typedef struct _GrDescriptorSet {
    GrStructType sType;
    GrDevice* device;
    DescriptorSetSlot* slots;
    DescriptorSetSlot* tempSlots;
    unsigned slotCount;
    VkBuffer virtualDescriptorSet;
    VkDeviceMemory boundMemory;//TODO: make some buddy allocator or like that
    VkDeviceAddress bufferDevicePtr;
} GrDescriptorSet;

typedef struct _GrGlobalDescriptorSet {
    VkDescriptorSetLayout descriptorTableLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorTable;
    VkSampler* samplers;
    VkSampler* samplerPtr;
    VkBufferView* bufferViews;
    VkBufferView* bufferViewPtr;
    VkImageView* images;
    VkImageView* imagePtr;
    unsigned descriptorCount;
} GrGlobalDescriptorSet;

typedef struct _GrDevice {
    GrStructType sType;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceMemoryProperties memoryProperties;
    unsigned universalQueueIndex;
    VkCommandPool universalCommandPool;
    unsigned computeQueueIndex;
    VkCommandPool computeCommandPool;
    GrGlobalDescriptorSet globalDescriptorSet;
    unsigned vDescriptorSetMemoryTypeIndex;
} GrDevice;

typedef struct _GrFence {
    GrStructType sType;
    VkDevice device;
    VkFence fence;
} GrFence;

typedef struct _GrEvent {
    GrStructType sType;
    VkDevice device;
    VkEvent event;
} GrEvent;

typedef struct _GrSemaphore {
    GrStructType sType;
    VkSemaphore semaphore;
} GrSemaphore;

typedef struct _GrGpuMemory {
    GrStructType sType;
    VkDeviceMemory deviceMemory;
    VkDevice device;
    VkBuffer buffer;
} GrGpuMemory;

typedef struct _GrImage {
    GrStructType sType;
    GrDevice* device;//TODO: add GrDevice pointer everywhere to support GrGetObjectInfo
    VkImage image;
    VkExtent3D extent;
    uint32_t layerCount;
    VkFormat format;
} GrImage;

typedef struct _GrMsaaStateObject {
    GrStructType sType;
} GrMsaaStateObject;

typedef struct _GrPhysicalGpu {
    GrStructType sType;
    VkPhysicalDevice physicalDevice;
} GrPhysicalGpu;

typedef struct _GrNestedDescriptorSetMapping {
    struct _GrNestedDescriptorSetMapping* nestedSet;
    struct _GrNestedDescriptorSetMapping* nextSet;
    unsigned slotIndex;
} GrNestedDescriptorSetMapping;

typedef struct _GrPipeline {
    GrStructType sType;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkRenderPass renderPass;
    GrNestedDescriptorSetMapping nestedDescriptorSets;
    unsigned boundDescriptorSetCount;
} GrPipeline;

typedef struct _GrRasterStateObject {
    GrStructType sType;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    float depthBiasConstantFactor;
    float depthBiasClamp;
    float depthBiasSlopeFactor;
} GrRasterStateObject;

typedef struct _GrSampler {
    GrStructType sType;
    VkSampler sampler;
} GrSampler;

typedef struct _GrShader {
    GrStructType sType;
    GrDevice* device;
    bool isPrecompiledSpv;
    VkShaderModule precompiledModule;
    uint32_t* code;
    uint32_t  codeSize;
} GrShader;

typedef struct _GrQueue {
    GrStructType sType;
    GrDevice* grDevice;
    VkQueue queue;
    unsigned queueIndex;
} GrQueue;

typedef struct _GrViewportStateObject {
    GrStructType sType;
    VkViewport* viewports;
    unsigned viewportCount;
    VkRect2D* scissors;
    unsigned scissorCount;
} GrViewportStateObject;

typedef struct _GrQueryPool {
    GrStructType sType;
    GrDevice *grDevice;
    VkQueryPool pool;
    VkQueryType queryType;
    uint32_t queryCount;
} GrQueryPool;
#endif // GR_OBJECT_H_
