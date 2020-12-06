#include "amdilc_spirv.h"
#include "amdilc_internal.h"

#define MAX_SRC_COUNT       (8)
#define ZERO_LITERAL        (0x00000000)
#define ONE_LITERAL         (0x3F800000)
#define FALSE_LITERAL       (0x00000000)
#define TRUE_LITERAL        (0xFFFFFFFF)
#define COMP_INDEX_X        (0)
#define COMP_INDEX_Y        (1)
#define COMP_INDEX_Z        (2)
#define COMP_INDEX_W        (3)
#define COMP_MASK_X         (1 << COMP_INDEX_X)
#define COMP_MASK_Y         (1 << COMP_INDEX_Y)
#define COMP_MASK_Z         (1 << COMP_INDEX_Z)
#define COMP_MASK_W         (1 << COMP_INDEX_W)
#define COMP_MASK_XY        (COMP_MASK_X | COMP_MASK_Y)
#define COMP_MASK_XYZ       (COMP_MASK_XY | COMP_MASK_Z)
#define COMP_MASK_XYZW      (COMP_MASK_XYZ | COMP_MASK_W)

typedef struct {
    IlcSpvId id;
    IlcSpvId typeId;
    uint32_t ilType; // ILRegType
    uint32_t ilNum;
    uint32_t literalValues[4];
} IlcRegister;

typedef struct {
    IlcSpvId id;
    IlcSpvId typeId;
    uint32_t ilId;
    uint32_t strideId;
    uint32_t ilType;
} IlcResource;

typedef enum {
    BLOCK_IF_ELSE,
    BLOCK_LOOP,
} IlcControlFlowBlockType;

typedef struct {
    IlcSpvId labelElseId;
    IlcSpvId labelEndId;
    bool hasElseBlock;
} IlcIfElseBlock;

typedef struct {
    IlcSpvId labelHeaderId;
    IlcSpvId labelContinueId;
    IlcSpvId labelBreakId;
} IlcLoopBlock;

typedef struct {
    IlcControlFlowBlockType type;
    union {
        IlcIfElseBlock ifElse;
        IlcLoopBlock loop;
    };
} IlcControlFlowBlock;

typedef struct {
    IlcSpvModule* module;
    const Kernel* kernel;
    IlcSpvId entryPointId;
    IlcSpvId intId;
    IlcSpvId int4Id;
    IlcSpvId floatId;
    IlcSpvId float4Id;
    IlcSpvId uintId;
    IlcSpvId zeroUintId;
    IlcSpvId boolId;
    IlcSpvId bool4Id;
    IlcSpvId samplerId;
    unsigned regCount;
    IlcRegister* regs;
    unsigned resourceCount;
    IlcResource* resources;
    IlcSpvId samplerResources[16];
    unsigned controlFlowBlockCount;
    IlcControlFlowBlock* controlFlowBlocks;
    bool isInFunction;
} IlcCompiler;

static IlcSpvId emitVectorVariable(
    IlcCompiler* compiler,
    IlcSpvId* typeId,
    unsigned componentCount,
    IlcSpvId componentTypeId,
    IlcSpvWord storageClass)
{
    *typeId = ilcSpvPutVectorType(compiler->module, componentTypeId, componentCount);
    IlcSpvId pointerId = ilcSpvPutPointerType(compiler->module, storageClass, *typeId);

    return ilcSpvPutVariable(compiler->module, pointerId, storageClass);
}

static IlcSpvId emitZeroOneVector(
    IlcCompiler* compiler, IlcSpvId scalarTypeId, uint32_t additionalElementCount)
{
    assert(additionalElementCount <= 2);
    IlcSpvId vecId = ilcSpvPutVectorType(compiler->module, scalarTypeId, additionalElementCount + 2);

    IlcSpvId consistuentIds[4];
    IlcSpvId zeroLiteralId = ilcSpvPutConstant(compiler->module, scalarTypeId, ZERO_LITERAL);
    for (uint32_t i = 0; i <= additionalElementCount;++i) {
        consistuentIds[i] = zeroLiteralId;
    }
    consistuentIds[additionalElementCount + 1] = ilcSpvPutConstant(compiler->module, scalarTypeId, ONE_LITERAL);
    return ilcSpvPutConstantComposite(compiler->module, vecId, additionalElementCount + 2, consistuentIds);
}

static const IlcRegister* addRegister(
    IlcCompiler* compiler,
    const IlcRegister* reg,
    char prefix)
{
    char name[16];
    snprintf(name, 16, "%c%u", prefix, reg->ilNum);
    ilcSpvPutName(compiler->module, reg->id, name);

    compiler->regCount++;
    compiler->regs = realloc(compiler->regs, sizeof(IlcRegister) * compiler->regCount);
    compiler->regs[compiler->regCount - 1] = *reg;

    return &compiler->regs[compiler->regCount - 1];
}

static const IlcRegister* findRegister(
    IlcCompiler* compiler,
    uint32_t type,
    uint32_t num)
{
    for (int i = 0; i < compiler->regCount; i++) {
        const IlcRegister* reg = &compiler->regs[i];

        if (reg->ilType == type && reg->ilNum == num) {
            return reg;
        }
    }

    return NULL;
}

static const IlcRegister* findOrCreateRegister(
    IlcCompiler* compiler,
    uint32_t type,
    uint32_t num)
{
    const IlcRegister* reg = findRegister(compiler, type, num);

    if (reg == NULL && type == IL_REGTYPE_TEMP) {
        // Create temporary register
        IlcSpvId tempTypeId = 0;
        IlcSpvId tempId = emitVectorVariable(compiler, &tempTypeId, 4, compiler->floatId,
                                             SpvStorageClassPrivate);

        const IlcRegister tempReg = {
            .id = tempId,
            .typeId = tempTypeId,
            .ilType = type,
            .ilNum = num,
        };

        reg = addRegister(compiler, &tempReg, 'r');
    }

    return reg;
}

static const IlcResource* findResource(
    IlcCompiler* compiler,
    uint32_t ilId)
{
    for (int i = 0; i < compiler->resourceCount; i++) {
        const IlcResource* resource = &compiler->resources[i];

        if (resource->ilId == ilId) {
            return resource;
        }
    }

    return NULL;
}

static const IlcResource* addResource(
    IlcCompiler* compiler,
    const IlcResource* resource)
{
    const IlcResource* existingResource = findResource(compiler, resource->ilId);
    if (existingResource != NULL) {
        LOGE("resource %d already present\n", resource->ilId);
        return existingResource;
    }

    char name[32];
    snprintf(name, 32, "resource%u", resource->ilId);
    ilcSpvPutName(compiler->module, resource->id, name);

    compiler->resourceCount++;
    compiler->resources = realloc(compiler->resources,
                                  sizeof(IlcResource) * compiler->resourceCount);
    compiler->resources[compiler->resourceCount - 1] = *resource;
    return compiler->resources + compiler->resourceCount - 1;
}

static void pushControlFlowBlock(
    IlcCompiler* compiler,
    const IlcControlFlowBlock* block)
{
    compiler->controlFlowBlockCount++;
    size_t size = sizeof(IlcControlFlowBlock) * compiler->controlFlowBlockCount;
    compiler->controlFlowBlocks = realloc(compiler->controlFlowBlocks, size);
    compiler->controlFlowBlocks[compiler->controlFlowBlockCount - 1] = *block;
}

static IlcControlFlowBlock popControlFlowBlock(
    IlcCompiler* compiler)
{
    assert(compiler->controlFlowBlockCount > 0);
    IlcControlFlowBlock block = compiler->controlFlowBlocks[compiler->controlFlowBlockCount - 1];

    compiler->controlFlowBlockCount--;
    size_t size = sizeof(IlcControlFlowBlock) * compiler->controlFlowBlockCount;
    compiler->controlFlowBlocks = realloc(compiler->controlFlowBlocks, size);

    return block;
}

static const IlcControlFlowBlock* findControlFlowBlock(
    IlcCompiler* compiler,
    IlcControlFlowBlockType type)
{
    for (int i = compiler->controlFlowBlockCount - 1; i >= 0; i--) {
        const IlcControlFlowBlock* block = &compiler->controlFlowBlocks[i];

        if (block->type == type) {
            return block;
        }
    }

    return NULL;
}

static IlcSpvId loadSource(
    IlcCompiler* compiler,
    const Source* src,
    uint8_t componentMask,
    IlcSpvId typeId)
{
    if (src->hasImmediate) {
        LOGW("unhandled immediate\n");
    }
    if (src->hasRelativeSrc) {
        LOGW("unhandled relative source\n");
    }

    const IlcRegister* reg = findRegister(compiler, src->registerType, src->registerNum);

    if (reg == NULL) {
        LOGE("source register %d %d not found\n", src->registerType, src->registerNum);
        return 0;
    }
    IlcSpvId sourceScalarTypeId, targetScalarTypeId;
    uint32_t sourceComponents = getSpvTypeComponentCount(compiler->module, reg->typeId, &sourceScalarTypeId);
    uint32_t targetComponents = getSpvTypeComponentCount(compiler->module, typeId, &targetScalarTypeId);
    if (sourceComponents == 0 || targetComponents == 0) {
        LOGE("Source or target type is/are have neither vector nor scalar type\n");
        return 0;
    }
    IlcSpvId varId = ilcSpvPutLoad(compiler->module, reg->typeId, reg->id);

    if (sourceScalarTypeId != targetScalarTypeId) {
        // Convert scalar to float vector
        if (sourceComponents == 1) {
            varId = ilcSpvPutBitcast(compiler->module, targetScalarTypeId, varId);
        }
        else if (sourceComponents != targetComponents) {
            IlcSpvId targetVecTypeId = ilcSpvPutVectorType(compiler->module, targetScalarTypeId, sourceComponents);
            varId = ilcSpvPutBitcast(compiler->module, targetVecTypeId, varId);
        }
        else {
            varId = ilcSpvPutBitcast(compiler->module, typeId, varId);
        }

    }

    const uint8_t swizzle[] = {
        componentMask & 1 ? src->swizzle[0] : IL_COMPSEL_0,
        componentMask & 2 ? src->swizzle[1] : IL_COMPSEL_0,
        componentMask & 4 ? src->swizzle[2] : IL_COMPSEL_0,
        componentMask & 8 ? src->swizzle[3] : IL_COMPSEL_0,
    };

    if (sourceComponents > 1 && targetComponents > 1 &&
        (swizzle[0] != IL_COMPSEL_X_R || swizzle[1] != IL_COMPSEL_Y_G ||
         swizzle[2] != IL_COMPSEL_Z_B || swizzle[3] != IL_COMPSEL_W_A)) {
        // Select components from {x, y, z, w, 0.f, 1.f}
        IlcSpvId zeroOneId = emitZeroOneVector(compiler, targetScalarTypeId, 4 - sourceComponents);

        const IlcSpvWord components[] = { swizzle[0], swizzle[1], swizzle[2], swizzle[3] };
        varId = ilcSpvPutVectorShuffle(compiler->module, typeId, varId, zeroOneId,
                                       targetComponents, components);
    }
    else if (targetComponents == 1 && sourceComponents > 1) {
        //extract X
        IlcSpvId elementId;
        if (swizzle[0] == IL_COMPSEL_X_R) { // cache zero literal because faster
            if (compiler->zeroUintId == 0) {
                compiler->zeroUintId = ilcSpvPutConstant(compiler->module, compiler->uintId, ZERO_LITERAL);
            }
            elementId = compiler->zeroUintId;
        }
        else {
            elementId = ilcSpvPutConstant(compiler->module, compiler->uintId, swizzle[0]);
        }
        varId = ilcSpvPutVectorExtractDynamic(compiler->module, typeId, varId, elementId);
    }
    else if (sourceComponents == 1 && targetComponents > 1) {
        //load vector
        IlcSpvId elementId = ilcSpvPutConstant(compiler->module, reg->typeId, 0);//since source type is scalar, just get type without searching
        IlcSpvId consistuents[4] = {varId, elementId, elementId, elementId};
        varId = ilcSpvPutCompositeConstruct(compiler->module, typeId, targetComponents, consistuents);
    }

    // All following operations but `neg` are float only (AMDIL spec, table 2.10)

    if (src->invert) {
        LOGW("unhandled invert flag\n");
    }

    if (src->bias) {
        LOGW("unhandled bias flag\n");
    }

    if (src->x2) {
        LOGW("unhandled x2 flag\n");
    }

    if (src->sign) {
        LOGW("unhandled sign flag\n");
    }

    if (src->divComp != IL_DIVCOMP_NONE) {
        LOGW("unhandled divcomp %d\n", src->divComp);
    }

    if (src->abs) {
        varId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450FAbs, compiler->float4Id, 1, &varId);
    }

    if (src->negate[0] || src->negate[1] || src->negate[2] || src->negate[3]) {
        IlcSpvId negId = 0;

        if (typeId == compiler->float4Id) {
            negId = ilcSpvPutAlu(compiler->module, SpvOpFNegate, compiler->float4Id, 1, &varId);
        } else if (typeId == compiler->int4Id) {
            negId = ilcSpvPutAlu(compiler->module, SpvOpSNegate, compiler->int4Id, 1, &varId);
        } else {
            assert(false);
        }

        if (src->negate[0] && src->negate[1] && src->negate[2] && src->negate[3]) {
            varId = negId;
        } else {
            // Select components from {-x, -y, -z, -w, x, y, z, w}
            const IlcSpvWord components[] = {
                src->negate[0] ? 0 : 4,
                src->negate[1] ? 1 : 5,
                src->negate[2] ? 2 : 6,
                src->negate[3] ? 3 : 7,
            };

            varId = ilcSpvPutVectorShuffle(compiler->module, typeId, negId, varId, 4, components);
        }
    }

    if (src->clamp) {
        LOGW("unhandled clamp flag\n");
    }

    return varId;
}

static void storeDestination(
    IlcCompiler* compiler,
    const Destination* dst,
    IlcSpvId varId)
{
    const IlcRegister* dstReg = findOrCreateRegister(compiler, dst->registerType, dst->registerNum);

    if (dstReg == NULL) {
        LOGE("destination register %d %d not found\n", dst->registerType, dst->registerNum);
        return;
    }

    if (dst->shiftScale != IL_SHIFT_NONE) {
        LOGW("unhandled shift scale %d\n", dst->shiftScale);
    }

    if (dst->clamp) {
        // Clamp to [0.f, 1.f]
        IlcSpvId zeroId = ilcSpvPutConstant(compiler->module, compiler->floatId, ZERO_LITERAL);
        IlcSpvId oneId = ilcSpvPutConstant(compiler->module, compiler->floatId, ONE_LITERAL);
        const IlcSpvId zeroConsistuentIds[] = { zeroId, zeroId, zeroId, zeroId };
        const IlcSpvId oneConsistuentIds[] = { oneId, oneId, oneId, oneId };
        IlcSpvId zeroCompositeId = ilcSpvPutConstantComposite(compiler->module, dstReg->typeId,
                                                              4, zeroConsistuentIds);
        IlcSpvId oneCompositeId = ilcSpvPutConstantComposite(compiler->module, dstReg->typeId,
                                                             4, oneConsistuentIds);

        const IlcSpvId paramIds[] = { varId, zeroCompositeId, oneCompositeId };
        varId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450FClamp, dstReg->typeId, 3, paramIds);
    }

    if (dst->component[0] == IL_MODCOMP_NOWRITE || dst->component[1] == IL_MODCOMP_NOWRITE ||
        dst->component[2] == IL_MODCOMP_NOWRITE || dst->component[3] == IL_MODCOMP_NOWRITE) {
        // Select components from {dst.x, dst.y, dst.z, dst.w, x, y, z, w}
        IlcSpvId origId = ilcSpvPutLoad(compiler->module, dstReg->typeId, dstReg->id);

        const IlcSpvWord components[] = {
            dst->component[0] == IL_MODCOMP_NOWRITE ? 0 : 4,
            dst->component[1] == IL_MODCOMP_NOWRITE ? 1 : 5,
            dst->component[2] == IL_MODCOMP_NOWRITE ? 2 : 6,
            dst->component[3] == IL_MODCOMP_NOWRITE ? 3 : 7,
        };
        varId = ilcSpvPutVectorShuffle(compiler->module, dstReg->typeId, origId, varId,
                                       4, components);
    }

    if ((dst->component[0] == IL_MODCOMP_0 || dst->component[0] == IL_MODCOMP_1) ||
        (dst->component[1] == IL_MODCOMP_0 || dst->component[1] == IL_MODCOMP_1) ||
        (dst->component[2] == IL_MODCOMP_0 || dst->component[2] == IL_MODCOMP_1) ||
        (dst->component[3] == IL_MODCOMP_0 || dst->component[3] == IL_MODCOMP_1)) {
        // Select components from {x, y, z, w, 0.f, 1.f}
        IlcSpvId zeroOneId = emitZeroOneVector(compiler, compiler->floatId, 0);//TODO: adjust types

        const IlcSpvWord components[] = {
            dst->component[0] == IL_MODCOMP_0 ? 4 : (dst->component[0] == IL_MODCOMP_1 ? 5 : 0),
            dst->component[1] == IL_MODCOMP_0 ? 4 : (dst->component[1] == IL_MODCOMP_1 ? 5 : 1),
            dst->component[2] == IL_MODCOMP_0 ? 4 : (dst->component[2] == IL_MODCOMP_1 ? 5 : 2),
            dst->component[3] == IL_MODCOMP_0 ? 4 : (dst->component[3] == IL_MODCOMP_1 ? 5 : 3),
        };
        varId = ilcSpvPutVectorShuffle(compiler->module, dstReg->typeId, varId, zeroOneId,
                                       4, components);
    }

    ilcSpvPutStore(compiler->module, dstReg->id, varId);
}

static void emitGlobalFlags(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    bool refactoringAllowed = GET_BIT(instr->control, 0);
    bool forceEarlyDepthStencil = GET_BIT(instr->control, 1);
    bool enableRawStructuredBuffers = GET_BIT(instr->control, 2);
    bool enableDoublePrecisionFloatOps = GET_BIT(instr->control, 3);

    if (!refactoringAllowed) {
        LOGW("unhandled !refactoringAllowed flag\n");
    }
    if (forceEarlyDepthStencil) {
        LOGW("unhandled forceEarlyDepthStencil flag\n");
    }
    if (enableRawStructuredBuffers) {
        LOGW("unhandled enableRawStructuredBuffers flag\n");
    }
    if (enableDoublePrecisionFloatOps) {
        LOGW("unhandled enableDoublePrecisionFloatOps flag\n");
    }
}

static void emitLiteral(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const Source* src = &instr->srcs[0];

    assert(src->registerType == IL_REGTYPE_LITERAL);

    IlcSpvId literalTypeId = 0;
    IlcSpvId literalId = emitVectorVariable(compiler, &literalTypeId, 4, compiler->floatId,
                                            SpvStorageClassPrivate);

    IlcSpvId consistuentIds[] = {
        ilcSpvPutConstant(compiler->module, compiler->floatId, instr->extras[0]),
        ilcSpvPutConstant(compiler->module, compiler->floatId, instr->extras[1]),
        ilcSpvPutConstant(compiler->module, compiler->floatId, instr->extras[2]),
        ilcSpvPutConstant(compiler->module, compiler->floatId, instr->extras[3]),
    };
    IlcSpvId compositeId = ilcSpvPutConstantComposite(compiler->module, literalTypeId,
                                                      4, consistuentIds);

    ilcSpvPutStore(compiler->module, literalId, compositeId);

    const IlcRegister reg = {
        .id = literalId,
        .typeId = literalTypeId,
        .ilType = src->registerType,
        .ilNum = src->registerNum,
        .literalValues = {instr->extras[0], instr->extras[1], instr->extras[2], instr->extras[3]},
    };

    addRegister(compiler, &reg, 'l');
}

static void emitOutput(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint8_t importUsage = GET_BITS(instr->control, 0, 4);

    assert(instr->dstCount == 1 &&
           instr->srcCount == 0 &&
           instr->extraCount == 0);

    const Destination* dst = &instr->dsts[0];

    assert(dst->registerType == IL_REGTYPE_OUTPUT &&
           !dst->clamp &&
           dst->shiftScale == IL_SHIFT_NONE);

    IlcSpvId outputTypeId = 0;
    IlcSpvId outputId = emitVectorVariable(compiler, &outputTypeId, 4, compiler->floatId,
                                           SpvStorageClassOutput);

    if (importUsage == IL_IMPORTUSAGE_POS) {
        IlcSpvWord builtInType = SpvBuiltInPosition;
        ilcSpvPutDecoration(compiler->module, outputId, SpvDecorationBuiltIn, 1, &builtInType);
    } else if (importUsage == IL_IMPORTUSAGE_GENERIC) {
        IlcSpvWord locationIdx = dst->registerNum;
        ilcSpvPutDecoration(compiler->module, outputId, SpvDecorationLocation, 1, &locationIdx);
    } else {
        LOGW("unhandled import usage %d\n", importUsage);
    }

    const IlcRegister reg = {
        .id = outputId,
        .typeId = outputTypeId,
        .ilType = dst->registerType,
        .ilNum = dst->registerNum,
        .literalValues = {},
    };

    addRegister(compiler, &reg, 'o');
}

static void emitInput(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint8_t importUsage = GET_BITS(instr->control, 0, 4);
    uint8_t interpMode = GET_BITS(instr->control, 5, 7);
    IlcSpvId inputId = 0;
    IlcSpvId inputTypeId = 0;

    assert(instr->dstCount == 1 &&
           instr->srcCount == 0 &&
           instr->extraCount == 0);

    const Destination* dst = &instr->dsts[0];

    assert(dst->registerType == IL_REGTYPE_INPUT &&
           !dst->clamp &&
           dst->shiftScale == IL_SHIFT_NONE);

    if (importUsage == IL_IMPORTUSAGE_GENERIC) {
        inputId = emitVectorVariable(compiler, &inputTypeId, 4, compiler->floatId,
                                     SpvStorageClassInput);

        IlcSpvWord locationIdx = dst->registerNum;
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationLocation, 1, &locationIdx);
    } else if (importUsage == IL_IMPORTUSAGE_VERTEXID ||
               importUsage == IL_IMPORTUSAGE_INSTANCEID) {
        IlcSpvId pointerId = ilcSpvPutPointerType(compiler->module, SpvStorageClassInput,
                                                  compiler->intId);
        inputId = ilcSpvPutVariable(compiler->module, pointerId, SpvStorageClassInput);
        inputTypeId = compiler->intId;

        IlcSpvWord builtInType = importUsage == IL_IMPORTUSAGE_VERTEXID ?
                                 SpvBuiltInVertexIndex : SpvBuiltInInstanceIndex;
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationBuiltIn, 1, &builtInType);
    } else {
        LOGW("unhandled import usage %d\n", importUsage);
    }

    // Handle interpolation modes in pixel shaders
    if (interpMode == IL_INTERPMODE_CONSTANT) {
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationFlat, 0, NULL);
    }
    if (interpMode == IL_INTERPMODE_LINEAR_CENTROID ||
        interpMode == IL_INTERPMODE_LINEAR_NOPERSPECTIVE_CENTROID) {
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationCentroid, 0, NULL);
    }
    if (interpMode == IL_INTERPMODE_LINEAR_NOPERSPECTIVE ||
        interpMode == IL_INTERPMODE_LINEAR_NOPERSPECTIVE_CENTROID ||
        interpMode == IL_INTERPMODE_LINEAR_NOPERSPECTIVE_SAMPLE) {
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationNoPerspective, 0, NULL);
    }
    if (interpMode == IL_INTERPMODE_LINEAR_SAMPLE ||
        interpMode == IL_INTERPMODE_LINEAR_NOPERSPECTIVE_SAMPLE) {
        ilcSpvPutCapability(compiler->module, SpvCapabilitySampleRateShading);
        ilcSpvPutDecoration(compiler->module, inputId, SpvDecorationSample, 0, NULL);
    }

    const IlcRegister reg = {
        .id = inputId,
        .typeId = inputTypeId,
        .ilType = dst->registerType,
        .ilNum = dst->registerNum,
        .literalValues = {},
    };

    addRegister(compiler, &reg, 'v');
}

static SpvImageFormat getSpvImageFormat(size_t formatCount, const uint8_t *ilResourceFmt, const SpvImageFormat* imageFormats)
{
    for (unsigned i = 1; i < formatCount; ++i) {
        if (ilResourceFmt[i] != ilResourceFmt[i - 1]) {
            return imageFormats[i - 1];
        }
    }
    return imageFormats[formatCount - 1];
}

static uint32_t getCoordinateVectorSize(uint8_t type)
{
    switch (type) {
    case IL_USAGE_PIXTEX_1D:
    case IL_USAGE_PIXTEX_BUFFER:
        return 1;
    case IL_USAGE_PIXTEX_1DARRAY:
    case IL_USAGE_PIXTEX_2DMSAA:
    case IL_USAGE_PIXTEX_2D:
        return 2;
    case IL_USAGE_PIXTEX_2DARRAY:
    case IL_USAGE_PIXTEX_2DARRAYMSAA:
    case IL_USAGE_PIXTEX_CUBEMAP:
    case IL_USAGE_PIXTEX_CUBEMAP_ARRAY:
    case IL_USAGE_PIXTEX_3D:
        return 3;
    default:
        LOGE("Unknown PixTexUsage type 0x%X\n", type);
        assert(false);
        return 0;
    }
}

static bool getSpvImage(uint8_t type, const uint8_t *imgFmt, SpvDim* outDim, SpvImageFormat* outImageFormat, IlcSpvWord* isArrayed, IlcSpvWord* isMultiSampled)
{
    *isArrayed = 0;
    *isMultiSampled = 0;
    switch (type) {
    case IL_USAGE_PIXTEX_1DARRAY:
        *isArrayed = 1;
    case IL_USAGE_PIXTEX_1D:
        *outDim = SpvDim1D;
        break;
    case IL_USAGE_PIXTEX_2DARRAY:
    case IL_USAGE_PIXTEX_2DARRAYMSAA:
        *isArrayed = 1;
    case IL_USAGE_PIXTEX_2DMSAA:
    case IL_USAGE_PIXTEX_2D:
        *outDim = SpvDim2D;
        *isMultiSampled = (type == IL_USAGE_PIXTEX_2DMSAA || type == IL_USAGE_PIXTEX_2DARRAYMSAA);
        break;
    case IL_USAGE_PIXTEX_CUBEMAP_ARRAY:
        *isArrayed = 1;
    case IL_USAGE_PIXTEX_CUBEMAP:
        *outDim = SpvDimCube;
        break;
    case IL_USAGE_PIXTEX_3D:
        *outDim = SpvDim3D;
        break;
    case IL_USAGE_PIXTEX_BUFFER:
        *outDim = SpvDimBuffer;
        break;
    default:
        LOGE("Unknown PixTexUsage type 0x%X\n", type);
        assert(false);
        return false;
    }
    const SpvImageFormat floatFormats[4] = {SpvImageFormatR32f, SpvImageFormatRg32f, SpvImageFormatUnknown, SpvImageFormatRgba32f};
    const SpvImageFormat snormFormats[4] = {SpvImageFormatR8Snorm, SpvImageFormatRg8Snorm, SpvImageFormatUnknown, SpvImageFormatRgba8Snorm};
    const SpvImageFormat unormFormats[4] = {SpvImageFormatR8, SpvImageFormatRg8, SpvImageFormatUnknown, SpvImageFormatRgba8};
    const SpvImageFormat uintFormats[4] = {SpvImageFormatR32ui, SpvImageFormatRg32ui, SpvImageFormatUnknown, SpvImageFormatRgba32ui};
    const SpvImageFormat intFormats[4] = {SpvImageFormatR32i, SpvImageFormatRg32i, SpvImageFormatUnknown, SpvImageFormatRgba32i};
    switch (imgFmt[0]) {
    case IL_ELEMENTFORMAT_UNKNOWN:
        *outImageFormat = SpvImageFormatUnknown;
        break;
    case IL_ELEMENTFORMAT_FLOAT:
        *outImageFormat = getSpvImageFormat(4, imgFmt, floatFormats);
        break;
    case IL_ELEMENTFORMAT_SNORM:
        *outImageFormat = getSpvImageFormat(4, imgFmt, snormFormats);
        break;
    case IL_ELEMENTFORMAT_UNORM:
        *outImageFormat = getSpvImageFormat(4, imgFmt, unormFormats);
        break;
    case IL_ELEMENTFORMAT_UINT:
        *outImageFormat = getSpvImageFormat(4, imgFmt, uintFormats);
        break;
    case IL_ELEMENTFORMAT_SINT:
        *outImageFormat = getSpvImageFormat(4, imgFmt, intFormats);
        break;
    case IL_ELEMENTFORMAT_SRGB:
        LOGE("Couldn't find format for IL_ELEMENTFORMAT_SRGB\n");
        assert(false);
        return false;
    default:
        LOGE("Couldn't find format for 0x%X\n", imgFmt[0]);
        assert(false);
        return false;
    }
    return true;
}

static void emitResource(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    assert(instr->dstCount == 0 &&
           instr->srcCount == 0 &&
           instr->extraCount == 1);

    uint8_t id = GET_BITS(instr->control, 0, 7);
    uint8_t type = GET_BITS(instr->control, 8, 11);
    bool unnorm = GET_BIT(instr->control, 31);
    uint8_t fmtx = GET_BITS(instr->extras[0], 20, 22);
    uint8_t fmty = GET_BITS(instr->extras[0], 23, 25);
    uint8_t fmtz = GET_BITS(instr->extras[0], 26, 28);
    uint8_t fmtw = GET_BITS(instr->extras[0], 29, 31);
    uint8_t imgFmt[4] = {fmtx, fmty, fmtz, fmtw};
    LOGT("found resource %d %d\n", id, type, fmtx);
    SpvDim dim;
    SpvImageFormat imageFormat;
    IlcSpvWord isArrayed, isMultiSampled;
    getSpvImage(type, imgFmt, &dim, &imageFormat, &isArrayed, &isMultiSampled);

    if (unnorm) {
        LOGE("unhandled resource type %d %d - can't handle unnormalized image types\n", type, unnorm);
        assert(false);
    }

    IlcSpvId imageId = ilcSpvPutImageType(compiler->module, compiler->floatId, dim,
                                          0 /*depth*/, isArrayed, isMultiSampled, 1, imageFormat);
    IlcSpvId pImageId = ilcSpvPutPointerType(compiler->module, SpvStorageClassUniformConstant,
                                             imageId);
    IlcSpvId resourceId = ilcSpvPutVariable(compiler->module, pImageId,
                                            SpvStorageClassUniformConstant);

    ilcSpvPutCapability(compiler->module, SpvCapabilitySampledBuffer);
    ilcSpvPutName(compiler->module, imageId, "float4Buffer");//TODO: replace name

    IlcSpvWord descriptorSetIdx = compiler->kernel->shaderType;
    ilcSpvPutDecoration(compiler->module, resourceId, SpvDecorationDescriptorSet,
                        1, &descriptorSetIdx);
    IlcSpvWord bindingIdx = id;
    ilcSpvPutDecoration(compiler->module, resourceId, SpvDecorationBinding, 1, &bindingIdx);

    const IlcResource resource = {
        .id = resourceId,
        .typeId = imageId,
        .ilId = id,
        .strideId = 0,
        .ilType = type,
    };

    addResource(compiler, &resource);
}

static void emitStructuredSrv(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint16_t id = GET_BITS(instr->control, 0, 13);

    IlcSpvId imageId = ilcSpvPutImageType(compiler->module, compiler->intId, SpvDimBuffer,
                                          0, 0, 0, 1, SpvImageFormatR32i);
    IlcSpvId pImageId = ilcSpvPutPointerType(compiler->module, SpvStorageClassUniformConstant,
                                             imageId);
    IlcSpvId resourceId = ilcSpvPutVariable(compiler->module, pImageId,
                                            SpvStorageClassUniformConstant);

    ilcSpvPutCapability(compiler->module, SpvCapabilitySampledBuffer);
    ilcSpvPutName(compiler->module, imageId, "structSrv");

    IlcSpvWord descriptorSetIdx = compiler->kernel->shaderType;
    ilcSpvPutDecoration(compiler->module, resourceId, SpvDecorationDescriptorSet,
                        1, &descriptorSetIdx);
    IlcSpvWord bindingIdx = id;
    ilcSpvPutDecoration(compiler->module, resourceId, SpvDecorationBinding, 1, &bindingIdx);

    const IlcResource resource = {
        .id = resourceId,
        .typeId = imageId,
        .ilId = id,
        .strideId = ilcSpvPutConstant(compiler->module, compiler->intId, instr->extras[0]),
    };

    addResource(compiler, &resource);
}

static void emitFunc(
    IlcCompiler* compiler,
    IlcSpvId id)
{
    IlcSpvId voidTypeId = ilcSpvPutVoidType(compiler->module);
    IlcSpvId funcTypeId = ilcSpvPutFunctionType(compiler->module, voidTypeId, 0, NULL);
    ilcSpvPutFunction(compiler->module, voidTypeId, id, SpvFunctionControlMaskNone, funcTypeId);
    ilcSpvPutLabel(compiler->module, 0);
}

static void emitFloatOp(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcSpvId srcIds[MAX_SRC_COUNT] = { 0 };
    IlcSpvId resId = 0;
    uint8_t componentMask = 0;

    switch (instr->opcode) {
    case IL_OP_DP2:
        componentMask = COMP_MASK_XY;
        break;
    case IL_OP_DP3:
        componentMask = COMP_MASK_XYZ;
        break;
    default:
        componentMask = COMP_MASK_XYZW;
        break;
    }

    for (int i = 0; i < instr->srcCount; i++) {
        srcIds[i] = loadSource(compiler, &instr->srcs[i], componentMask, compiler->float4Id);
    }

    switch (instr->opcode) {
    case IL_OP_ABS:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450FAbs, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_ACOS: {
        IlcSpvId acosId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Acos, compiler->float4Id,
                                          instr->srcCount, srcIds);
        // Replicate .w on all components
        const IlcSpvWord components[] = { COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W };
        resId = ilcSpvPutVectorShuffle(compiler->module, compiler->float4Id, acosId, acosId,
                                       4, components);
    }   break;
    case IL_OP_ADD:
        resId = ilcSpvPutAlu(compiler->module, SpvOpFAdd, compiler->float4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_ASIN: {
        IlcSpvId asinId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Asin, compiler->float4Id,
                                          instr->srcCount, srcIds);
        // Replicate .w on all components
        const IlcSpvWord components[] = { COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W };
        resId = ilcSpvPutVectorShuffle(compiler->module, compiler->float4Id, asinId, asinId,
                                       4, components);
    }   break;
    case IL_OP_ATAN: {
        IlcSpvId atanId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Atan, compiler->float4Id,
                                          instr->srcCount, srcIds);
        // Replicate .w on all components
        const IlcSpvWord components[] = { COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W, COMP_INDEX_W };
        resId = ilcSpvPutVectorShuffle(compiler->module, compiler->float4Id, atanId, atanId,
                                       4, components);
    }   break;
    case IL_OP_DIV:
        if (instr->control != IL_ZEROOP_INFINITY) {
            LOGW("unhandled div zero op %d\n", instr->control);
        }
        // FIXME SPIR-V has undefined division by zero
        resId = ilcSpvPutAlu(compiler->module, SpvOpFDiv, compiler->float4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_DP2:
    case IL_OP_DP3:
    case IL_OP_DP4: {
        bool ieee = GET_BIT(instr->control, 0);
        if (!ieee) {
            LOGW("unhandled non-IEEE dot product\n");
        }
        IlcSpvId dotId = ilcSpvPutAlu(compiler->module, SpvOpDot, compiler->floatId,
                                      instr->srcCount, srcIds);
        // Replicate dot product on all components
        const IlcSpvWord constituents[] = { dotId, dotId, dotId, dotId };
        resId = ilcSpvPutCompositeConstruct(compiler->module, compiler->float4Id, 4, constituents);
    }   break;
    case IL_OP_FRC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Fract, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_MAD: {
        bool ieee = GET_BIT(instr->control, 0);
        if (!ieee) {
            LOGW("unhandled non-IEEE mad\n");
        }
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Fma, compiler->float4Id,
                                instr->srcCount, srcIds);
    }   break;
    case IL_OP_MAX: {
        bool ieee = GET_BIT(instr->control, 0);
        if (!ieee) {
            LOGW("unhandled non-IEEE max\n");
        }
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450NMax, compiler->float4Id,
                                instr->srcCount, srcIds);
    }   break;
    case IL_OP_MIN: {
        bool ieee = GET_BIT(instr->control, 0);
        if (!ieee) {
            LOGW("unhandled non-IEEE min\n");
        }
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450NMin, compiler->float4Id,
                                instr->srcCount, srcIds);
    }   break;
    case IL_OP_MOV:
        resId = srcIds[0];
        break;
    case IL_OP_MUL: {
        bool ieee = GET_BIT(instr->control, 0);
        if (!ieee) {
            LOGW("unhandled non-IEEE mul\n");
        }
        resId = ilcSpvPutAlu(compiler->module, SpvOpFMul, compiler->float4Id,
                             instr->srcCount, srcIds);
    }   break;
    case IL_OP_FTOI:
        resId = ilcSpvPutAlu(compiler->module, SpvOpConvertFToS, compiler->int4Id,
                             instr->srcCount, srcIds);
        resId = ilcSpvPutBitcast(compiler->module, compiler->float4Id, resId);
        break;
    case IL_OP_ITOF:
        resId = ilcSpvPutBitcast(compiler->module, compiler->int4Id, srcIds[0]);
        resId = ilcSpvPutAlu(compiler->module, SpvOpConvertSToF, compiler->float4Id, 1, &resId);
        break;
    case IL_OP_ROUND_NEG_INF:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Floor, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_ROUND_PLUS_INF:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Ceil, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_EXP_VEC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Exp, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_LOG_VEC:
        // FIXME handle log(0)
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Log, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_RSQ_VEC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450InverseSqrt, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_SIN_VEC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Sin, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_COS_VEC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Cos, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    case IL_OP_SQRT_VEC:
        resId = ilcSpvPutGLSLOp(compiler->module, GLSLstd450Sqrt, compiler->float4Id,
                                instr->srcCount, srcIds);
        break;
    default:
        assert(false);
        break;
    }

    storeDestination(compiler, &instr->dsts[0], resId);
}

static void emitFloatComparisonOp(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcSpvId srcIds[MAX_SRC_COUNT] = { 0 };
    SpvOp compOp = 0;

    for (int i = 0; i < instr->srcCount; i++) {
        srcIds[i] = loadSource(compiler, &instr->srcs[i], COMP_MASK_XYZW, compiler->float4Id);
    }

    switch (instr->opcode) {
    case IL_OP_EQ:
        compOp = SpvOpFOrdEqual;
        break;
    case IL_OP_GE:
        compOp = SpvOpFOrdGreaterThanEqual;
        break;
    case IL_OP_LT:
        compOp = SpvOpFOrdLessThan;
        break;
    case IL_OP_NE:
        compOp = SpvOpFOrdNotEqual;
        break;
    default:
        assert(false);
        break;
    }

    IlcSpvId condId = ilcSpvPutAlu(compiler->module, compOp, compiler->bool4Id,
                                   instr->srcCount, srcIds);
    IlcSpvId trueId = ilcSpvPutConstant(compiler->module, compiler->floatId, TRUE_LITERAL);
    IlcSpvId falseId = ilcSpvPutConstant(compiler->module, compiler->floatId, FALSE_LITERAL);
    const IlcSpvId trueConsistuentIds[] = { trueId, trueId, trueId, trueId };
    const IlcSpvId falseConsistuentIds[] = { falseId, falseId, falseId, falseId };
    IlcSpvId trueCompositeId = ilcSpvPutConstantComposite(compiler->module, compiler->float4Id,
                                                          4, trueConsistuentIds);
    IlcSpvId falseCompositeId = ilcSpvPutConstantComposite(compiler->module, compiler->float4Id,
                                                           4, falseConsistuentIds);
    IlcSpvId resId = ilcSpvPutSelect(compiler->module, compiler->float4Id, condId,
                                     trueCompositeId, falseCompositeId);

    storeDestination(compiler, &instr->dsts[0], resId);
}

static void emitIntegerOp(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcSpvId srcIds[MAX_SRC_COUNT] = { 0 };
    IlcSpvId resId = 0;

    for (int i = 0; i < instr->srcCount; i++) {
        srcIds[i] = loadSource(compiler, &instr->srcs[i], COMP_MASK_XYZW, compiler->int4Id);
    }

    switch (instr->opcode) {
    case IL_OP_I_NOT:
        resId = ilcSpvPutAlu(compiler->module, SpvOpNot, compiler->int4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_I_OR:
        resId = ilcSpvPutAlu(compiler->module, SpvOpBitwiseOr, compiler->int4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_I_ADD:
        resId = ilcSpvPutAlu(compiler->module, SpvOpIAdd, compiler->int4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_AND:
        resId = ilcSpvPutAlu(compiler->module, SpvOpBitwiseAnd, compiler->int4Id,
                             instr->srcCount, srcIds);
        break;
    case IL_OP_U_BIT_EXTRACT: {
        // FIXME: not sure if the settings are per-component
        // TODO: 0x1F mask
        LOGW("IL_OP_U_BIT_EXTRACT is partially implemented\n");

        IlcSpvWord widthIndex = COMP_INDEX_X;
        IlcSpvId widthId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcIds[0],
                                                     1, &widthIndex);
        IlcSpvWord offsetIndex = COMP_INDEX_X;
        IlcSpvId offsetId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcIds[1],
                                                      1, &offsetIndex);

        const IlcSpvId argIds[] = { srcIds[2], offsetId, widthId };
        resId = ilcSpvPutAlu(compiler->module, SpvOpBitFieldUExtract, compiler->int4Id, 3, argIds);
        break;
    }
    default:
        assert(false);
        break;
    }

    storeDestination(compiler, &instr->dsts[0],
                     ilcSpvPutBitcast(compiler->module, compiler->float4Id, resId));
}

static void emitIntegerComparisonOp(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcSpvId srcIds[MAX_SRC_COUNT] = { 0 };
    SpvOp compOp = 0;

    for (int i = 0; i < instr->srcCount; i++) {
        srcIds[i] = loadSource(compiler, &instr->srcs[i], COMP_MASK_XYZW, compiler->int4Id);
    }

    switch (instr->opcode) {
    case IL_OP_I_EQ:
        compOp = SpvOpIEqual;
        break;
    case IL_OP_I_GE:
        compOp = SpvOpSGreaterThanEqual;
        break;
    case IL_OP_I_LT:
        compOp = SpvOpSLessThan;
        break;
    default:
        assert(false);
        break;
    }

    IlcSpvId condId = ilcSpvPutAlu(compiler->module, compOp, compiler->bool4Id,
                                   instr->srcCount, srcIds);
    IlcSpvId trueId = ilcSpvPutConstant(compiler->module, compiler->floatId, TRUE_LITERAL);
    IlcSpvId falseId = ilcSpvPutConstant(compiler->module, compiler->floatId, FALSE_LITERAL);
    const IlcSpvId trueConsistuentIds[] = { trueId, trueId, trueId, trueId };
    const IlcSpvId falseConsistuentIds[] = { falseId, falseId, falseId, falseId };
    IlcSpvId trueCompositeId = ilcSpvPutConstantComposite(compiler->module, compiler->float4Id,
                                                          4, trueConsistuentIds);
    IlcSpvId falseCompositeId = ilcSpvPutConstantComposite(compiler->module, compiler->float4Id,
                                                           4, falseConsistuentIds);
    IlcSpvId resId = ilcSpvPutSelect(compiler->module, compiler->float4Id, condId,
                                     trueCompositeId, falseCompositeId);

    storeDestination(compiler, &instr->dsts[0], resId);
}

static void emitCmovLogical(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcSpvId srcIds[MAX_SRC_COUNT] = { 0 };

    for (int i = 0; i < instr->srcCount; i++) {
        srcIds[i] = loadSource(compiler, &instr->srcs[i], COMP_MASK_XYZW, compiler->float4Id);
    }

    // For each component, select src1 if src0 has any bit set, otherwise select src2
    IlcSpvId falseId = ilcSpvPutConstant(compiler->module, compiler->intId, FALSE_LITERAL);
    const IlcSpvId falseConsistuentIds[] = { falseId, falseId, falseId, falseId };
    IlcSpvId falseCompositeId = ilcSpvPutConstantComposite(compiler->module, compiler->int4Id,
                                                           4, falseConsistuentIds);
    IlcSpvId castId = ilcSpvPutBitcast(compiler->module, compiler->int4Id, srcIds[0]);
    const IlcSpvId compIds[] = { castId, falseCompositeId };
    IlcSpvId condId = ilcSpvPutAlu(compiler->module, SpvOpINotEqual, compiler->bool4Id, 2, compIds);
    IlcSpvId resId = ilcSpvPutSelect(compiler->module, compiler->float4Id, condId,
                                     srcIds[1], srcIds[2]);

    storeDestination(compiler, &instr->dsts[0], resId);
}

static IlcSpvId emitConditionCheck(
    IlcCompiler* compiler,
    IlcSpvId srcId,
    bool notZero)
{
    IlcSpvWord xIndex = COMP_INDEX_X;
    IlcSpvId xId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcId, 1, &xIndex);
    IlcSpvId falseId = ilcSpvPutConstant(compiler->module, compiler->intId, FALSE_LITERAL);
    const IlcSpvId compIds[] = { xId, falseId };
    return ilcSpvPutAlu(compiler->module, notZero ? SpvOpINotEqual : SpvOpIEqual, compiler->boolId,
                        2, compIds);
}

static void emitIf(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcIfElseBlock ifElseBlock = {
        .labelElseId = ilcSpvAllocId(compiler->module),
        .labelEndId = ilcSpvAllocId(compiler->module),
        .hasElseBlock = false,
    };

    IlcSpvId srcId = loadSource(compiler, &instr->srcs[0], COMP_MASK_XYZW, compiler->int4Id);
    IlcSpvId labelBeginId = ilcSpvAllocId(compiler->module);
    IlcSpvId condId = emitConditionCheck(compiler, srcId, instr->opcode == IL_OP_IF_LOGICALNZ);
    ilcSpvPutSelectionMerge(compiler->module, ifElseBlock.labelEndId);
    ilcSpvPutBranchConditional(compiler->module, condId, labelBeginId, ifElseBlock.labelElseId);
    ilcSpvPutLabel(compiler->module, labelBeginId);

    const IlcControlFlowBlock block = {
        .type = BLOCK_IF_ELSE,
        .ifElse = ifElseBlock,
    };
    pushControlFlowBlock(compiler, &block);
}

static void emitElse(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    IlcControlFlowBlock block = popControlFlowBlock(compiler);
    if (block.type != BLOCK_IF_ELSE) {
        LOGE("no matching if/else block was found\n");
        assert(false);
    }

    ilcSpvPutBranch(compiler->module, block.ifElse.labelEndId);
    ilcSpvPutLabel(compiler->module, block.ifElse.labelElseId);
    block.ifElse.hasElseBlock = true;

    pushControlFlowBlock(compiler, &block);
}

static void emitWhile(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcLoopBlock loopBlock = {
        .labelHeaderId = ilcSpvAllocId(compiler->module),
        .labelContinueId = ilcSpvAllocId(compiler->module),
        .labelBreakId = ilcSpvAllocId(compiler->module),
    };

    ilcSpvPutBranch(compiler->module, loopBlock.labelHeaderId);
    ilcSpvPutLabel(compiler->module, loopBlock.labelHeaderId);

    ilcSpvPutLoopMerge(compiler->module, loopBlock.labelBreakId, loopBlock.labelContinueId);

    IlcSpvId labelBeginId = ilcSpvAllocId(compiler->module);
    ilcSpvPutBranch(compiler->module, labelBeginId);
    ilcSpvPutLabel(compiler->module, labelBeginId);

    const IlcControlFlowBlock block = {
        .type = BLOCK_LOOP,
        .loop = loopBlock,
    };
    pushControlFlowBlock(compiler, &block);
}

static void emitEndIf(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcControlFlowBlock block = popControlFlowBlock(compiler);
    if (block.type != BLOCK_IF_ELSE) {
        LOGE("no matching if/else block was found\n");
        assert(false);
    }

    if (!block.ifElse.hasElseBlock) {
        // If no else block was declared, insert a dummy one
        ilcSpvPutBranch(compiler->module, block.ifElse.labelEndId);
        ilcSpvPutLabel(compiler->module, block.ifElse.labelElseId);
    }

    ilcSpvPutBranch(compiler->module, block.ifElse.labelEndId);
    ilcSpvPutLabel(compiler->module, block.ifElse.labelEndId);
}

static void emitEndLoop(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcControlFlowBlock block = popControlFlowBlock(compiler);
    if (block.type != BLOCK_LOOP) {
        LOGE("no matching loop block was found\n");
        assert(false);
    }

    ilcSpvPutBranch(compiler->module, block.loop.labelContinueId);
    ilcSpvPutLabel(compiler->module, block.loop.labelContinueId);

    ilcSpvPutBranch(compiler->module, block.loop.labelHeaderId);
    ilcSpvPutLabel(compiler->module, block.loop.labelBreakId);
}

static void emitBreak(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcControlFlowBlock* block = findControlFlowBlock(compiler, BLOCK_LOOP);
    if (block == NULL) {
        LOGE("no matching loop block was found\n");
        assert(false);
    }

    IlcSpvId labelId = ilcSpvAllocId(compiler->module);

    if (instr->opcode == IL_OP_BREAK) {
        ilcSpvPutBranch(compiler->module, block->loop.labelBreakId);
    } else if (instr->opcode == IL_OP_BREAK_LOGICALZ ||
               instr->opcode == IL_OP_BREAK_LOGICALNZ) {
        IlcSpvId srcId = loadSource(compiler, &instr->srcs[0], COMP_MASK_XYZW, compiler->int4Id);
        IlcSpvId condId = emitConditionCheck(compiler, srcId,
                                             instr->opcode == IL_OP_BREAK_LOGICALNZ);
        ilcSpvPutBranchConditional(compiler->module, condId, block->loop.labelBreakId, labelId);
    } else {
        assert(false);
    }

    ilcSpvPutLabel(compiler->module, labelId);
}

static void emitContinue(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    const IlcControlFlowBlock* block = findControlFlowBlock(compiler, BLOCK_LOOP);
    if (block == NULL) {
        LOGE("no matching loop block was found\n");
        assert(false);
    }

    IlcSpvId labelId = ilcSpvAllocId(compiler->module);
    ilcSpvPutBranch(compiler->module, block->loop.labelContinueId);
    ilcSpvPutLabel(compiler->module, labelId);
}

static IlcSpvId emitOrGetSampler(
    IlcCompiler* compiler,
    uint8_t ilSamplerId)
{
    if (compiler->samplerResources[ilSamplerId] != 0) {
        return compiler->samplerResources[ilSamplerId];
    }
    if (compiler->samplerId == 0) {
        compiler->samplerId = ilcSpvPutSamplerType(compiler->module);
    }
    IlcSpvId pSamplerId = ilcSpvPutPointerType(compiler->module, SpvStorageClassUniformConstant,
                                                 compiler->samplerId);
    compiler->samplerResources[ilSamplerId] = ilcSpvPutVariable(compiler->module, pSamplerId,
                                                               SpvStorageClassUniformConstant);
    return compiler->samplerResources[ilSamplerId];
}


static bool getOffsetCoordinateType(
    IlcCompiler* compiler,
    unsigned coordinateVecSize,
    IlcSpvId* outTypeId,
    uint32_t* outMask)
{
    if (outTypeId == NULL) {
        return false;
    }
    unsigned mask;
    IlcSpvId coordTypeId;
    switch (coordinateVecSize) {
    case 1:
        coordTypeId = compiler->intId;
        mask = COMP_MASK_X;
        break;
    case 2:
        coordTypeId = ilcSpvPutVectorType(compiler->module, compiler->intId, 2);
        mask = COMP_MASK_XY;
        break;
    case 3:
        coordTypeId = ilcSpvPutVectorType(compiler->module, compiler->intId, 3);
        mask = COMP_MASK_XYZ;
        break;
    case 4:
        coordTypeId = compiler->int4Id;
        mask = COMP_MASK_XYZW;
        break;
    default:
        LOGE("invalid coordinate size\n");
        return false;
    }

    if (outTypeId != NULL) {
        *outTypeId = coordTypeId;
    }

    if (outMask != NULL) {
        *outMask = mask;
    }
    return true;
}

static void emitSample(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint8_t ilResourceId = GET_BITS(instr->control, 0, 7);
    uint8_t ilSamplerId  = GET_BITS(instr->control, 8, 11);
    bool indexedArgs = GET_BIT(instr->control, 12);

    if (indexedArgs) {
        if ((instr->srcs[instr->srcCount - 1].registerType != IL_REGTYPE_LITERAL && instr->srcs[instr->srcCount - 1].registerType != IL_REGTYPE_CONST_INT) ||
            (instr->srcs[instr->srcCount - 2].registerType != IL_REGTYPE_LITERAL && instr->srcs[instr->srcCount - 2].registerType != IL_REGTYPE_CONST_INT)) {
            LOGE("can't handle non-constant resource offsets\n");
            return;
        }
        if (instr->srcs[instr->srcCount - 2].swizzle[0] >= IL_COMPSEL_LAST || instr->srcs[instr->srcCount - 1].swizzle[0] >= IL_COMPSEL_LAST) {
            LOGE("invalid swizzle for resource offset X coordinate\n");
            return;
        }
        // offset extraction
        switch (instr->srcs[instr->srcCount - 2].swizzle[0]) {
        case IL_COMPSEL_0:
            //do nothing :D
            break;
        case IL_COMPSEL_1:
            ilResourceId += 1;
            break;
        default: {
            const IlcRegister* reg = findRegister(compiler, instr->srcs[instr->srcCount - 2].registerType, instr->srcs[instr->srcCount - 2].registerNum);
            if (reg == NULL) {
                LOGE("failed to find register %d", instr->srcs[instr->srcCount - 2].registerNum);
                return;
            }
            ilResourceId += reg->literalValues[instr->srcs[instr->srcCount - 2].swizzle[0]];
        } break;
        }

        switch (instr->srcs[instr->srcCount - 1].swizzle[0]) {
        case IL_COMPSEL_0:
            //do nothing :D
            break;
        case IL_COMPSEL_1:
            ilSamplerId += 1;
            break;
        default: {
            const IlcRegister* reg = findRegister(compiler, instr->srcs[instr->srcCount - 1].registerType, instr->srcs[instr->srcCount - 1].registerNum);
            if (reg == NULL) {
                LOGE("failed to find register %d", instr->srcs[instr->srcCount - 1].registerNum);
                return;
            }
            ilSamplerId += reg->literalValues[instr->srcs[instr->srcCount - 1].swizzle[0]];
        } break;
        }
    }

    const IlcResource* resource = findResource(compiler, ilResourceId);
    if (resource == NULL && indexedArgs) {
        bool unnormalized = GET_BIT(instr->control, 15) ? (GET_BITS(instr->primModifier, 2, 3) == IL_TEXCOORDMODE_UNNORMALIZED) : false;
        if (unnormalized) {
            LOGE("unhandled resource type %d %d - can't handle unnormalized image types\n", instr->resourceFormat, unnormalized);
            assert(false);
        }
        SpvDim dim;
        SpvImageFormat imageFormat;
        IlcSpvWord isArrayed, isMultiSampled;
        uint8_t imgFmt[4] = {IL_ELEMENTFORMAT_UNKNOWN, IL_ELEMENTFORMAT_UNKNOWN, IL_ELEMENTFORMAT_UNKNOWN, IL_ELEMENTFORMAT_UNKNOWN};
        getSpvImage(instr->resourceFormat, imgFmt, &dim, &imageFormat, &isArrayed, &isMultiSampled);
        IlcSpvId imageId = ilcSpvPutImageType(compiler->module, compiler->floatId, dim,
                                              0 /*depth*/, false, false, 1, SpvImageFormatUnknown);
        IlcSpvId pImageId = ilcSpvPutPointerType(compiler->module, SpvStorageClassUniformConstant,
                                                 imageId);
        IlcSpvId resourceId = ilcSpvPutVariable(compiler->module, pImageId,
                                                SpvStorageClassUniformConstant);

        ilcSpvPutCapability(compiler->module, SpvCapabilitySampledBuffer);
        ilcSpvPutName(compiler->module, imageId, "float4Buffer");//TODO: replace name

        IlcSpvWord descriptorSetIdx = compiler->kernel->shaderType;
        ilcSpvPutDecoration(compiler->module, resourceId, SpvDecorationDescriptorSet,
                            1, &descriptorSetIdx);//TODO: replace descriptor sets
        const IlcResource newResource = {
            .id = resourceId,
            .typeId = imageId,
            .ilId = ilResourceId,
            .ilType = instr->resourceFormat,
        };

        resource = addResource(compiler, &newResource);
    }

    if (resource == NULL) {
        LOGE("resource %d not found\n", ilResourceId);
        return;
    }
    uint32_t coordinateVecSize;
    if (resource->ilType == 0) {
        // that shouldn't happen really
        LOGE("ilType of resource is 0\n");
        return;
    }
    else {
        coordinateVecSize = getCoordinateVectorSize(resource->ilType);
    }
    uint32_t mask;
    IlcSpvId coordTypeId;
    switch (coordinateVecSize) {
    case 1:
        coordTypeId = compiler->floatId;
        mask = COMP_MASK_X;
        break;
    case 2:
        coordTypeId = ilcSpvPutVectorType(compiler->module, compiler->floatId, 2);
        mask = COMP_MASK_XY;
        break;
    case 3:
        coordTypeId = ilcSpvPutVectorType(compiler->module, compiler->floatId, 3);
        mask = COMP_MASK_XYZ;
        break;
    case 4:
        coordTypeId = compiler->float4Id;
        mask = COMP_MASK_XYZW;
        break;
    default:
        LOGE("invalid coordinate size\n");
        assert(false);
        return;
    }
    const Destination* dst = &instr->dsts[0];
    //TODO: check mask by image type
    IlcSpvId coordSrcId = loadSource(compiler, &instr->srcs[0], mask, coordTypeId);
    IlcSpvId sampledImageTypeId = ilcSpvPutSampledImageType(compiler->module, resource->typeId);
    IlcSpvId samplerResourceId = emitOrGetSampler(compiler, ilSamplerId);
    IlcSpvId imageResourceId  = ilcSpvPutLoad(compiler->module, resource->typeId, resource->id);
    IlcSpvId samplerId  = ilcSpvPutLoad(compiler->module, compiler->samplerId, samplerResourceId);
    IlcSpvId sampledImageId = ilcSpvPutSampledImage(compiler->module, sampledImageTypeId, imageResourceId, samplerId);
    IlcSpvId drefId = 0;
    IlcSpvId argMask = 0;
    uint32_t argCount = 0;
    IlcSpvId parameters[9];//just in case
    bool depthComparison = false;
    switch (instr->opcode) {
    case IL_OP_SAMPLE_B:
    {
        parameters[argCount] = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);// bias
        argCount++;
        argMask |= SpvImageOperandsBiasMask;
    } break;
    case IL_OP_SAMPLE_G: {
        parameters[argCount] = loadSource(compiler, &instr->srcs[1], mask, coordTypeId);// dividend
        argCount++;
        parameters[argCount] = loadSource(compiler, &instr->srcs[2], mask, coordTypeId);// divisor
        argCount++;
        argMask |= SpvImageOperandsGradMask;
    }  break;
    case IL_OP_SAMPLE_L: {
        parameters[argCount] = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);// bias
        argCount++;
        argMask |= SpvImageOperandsLodMask;
    } break;
    case IL_OP_SAMPLE_C: {
        depthComparison = true;
        drefId = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);
    } break;
    case IL_OP_SAMPLE_C_B:
    {
        depthComparison = true;
        drefId = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);
        parameters[argCount] = loadSource(compiler, &instr->srcs[2], mask, compiler->floatId);// bias
        argCount++;
        argMask |= SpvImageOperandsBiasMask;
    } break;
    case IL_OP_SAMPLE_C_G: {
        depthComparison = true;
        drefId = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);
        parameters[argCount] = loadSource(compiler, &instr->srcs[2], mask, coordTypeId);// dividend
        argCount++;
        parameters[argCount] = loadSource(compiler, &instr->srcs[3], mask, coordTypeId);// divisor
        argCount++;
        argMask |= SpvImageOperandsGradMask;
    }  break;
    case IL_OP_SAMPLE_C_L: {
        depthComparison = true;
        drefId = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);
        parameters[argCount] = loadSource(compiler, &instr->srcs[2], mask, compiler->floatId);// lod
        argCount++;
        argMask |= SpvImageOperandsLodMask;
    } break;
    case IL_OP_SAMPLE_C_LZ: {
        depthComparison = true;
        drefId = loadSource(compiler, &instr->srcs[1], mask, compiler->floatId);
        parameters[argCount] = ilcSpvPutConstant(compiler->module, compiler->floatId, ZERO_LITERAL);// lod is zero float here
        argCount++;
        argMask |= SpvImageOperandsLodMask;
    } break;
    default:
        break;
    }
    if (GET_BIT(instr->control, 13)) {
        IlcSpvId offsetTypeId = 0;
        if (!getOffsetCoordinateType(compiler, coordinateVecSize, &offsetTypeId, NULL)) {
            LOGE("couldn't get type for texture offset\n");
            return;
        }

        IlcSpvId offsetValues[4];//TODO: add support for 3d images
        for (uint32_t i = 0; i < coordinateVecSize; ++i) {
            uint8_t offsetVal = (uint8_t)((instr->addressOffset >> (i * 8)) & 0xFF);
            int literalOffsetVal = (int)(*((int8_t*)&offsetVal) >> 1);
            offsetValues[i] = ilcSpvPutConstant(compiler->module, compiler->intId, *((IlcSpvWord*)&literalOffsetVal));
        }
        argMask |= SpvImageOperandsConstOffsetMask;
        parameters[argCount] = ilcSpvPutConstantComposite(compiler->module, offsetTypeId, coordinateVecSize, offsetValues);
        argCount++;
    }
    IlcSpvId sampleResultId = depthComparison ?
        ilcSpvPutImageSampleDref(
            compiler->module,
            compiler->float4Id,
            sampledImageId,
            coordSrcId,
            drefId,
            argMask,
            parameters) :
        ilcSpvPutImageSample(
            compiler->module,
            compiler->float4Id,
            sampledImageId,
            coordSrcId,
            argMask,
            parameters);
    storeDestination(compiler, dst, sampleResultId);
}

static void emitLoad(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint8_t ilResourceId = GET_BITS(instr->control, 0, 7);
    const IlcResource* resource = findResource(compiler, ilResourceId);

    const Destination* dst = &instr->dsts[0];
    const IlcRegister* dstReg = findOrCreateRegister(compiler, dst->registerType, dst->registerNum);

    if (dstReg == NULL) {
        LOGE("destination register %d %d not found\n", dst->registerType, dst->registerNum);
        return;
    }

    IlcSpvId srcId = loadSource(compiler, &instr->srcs[0], COMP_MASK_XYZW, compiler->int4Id);
    IlcSpvWord addressIndex = COMP_INDEX_X;
    IlcSpvId addressId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcId,
                                                   1, &addressIndex);
    IlcSpvId resourceId = ilcSpvPutLoad(compiler->module, resource->typeId, resource->id);
    IlcSpvId fetchId = ilcSpvPutImageFetch(compiler->module, dstReg->typeId, resourceId, addressId);
    storeDestination(compiler, dst, fetchId);
}

static void emitStructuredSrvLoad(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    uint8_t ilResourceId = GET_BITS(instr->control, 0, 7);
    bool indexedResourceId = GET_BIT(instr->control, 12);
    const IlcResource* resource = findResource(compiler, ilResourceId);

    if (indexedResourceId) {
        LOGW("unhandled indexed resource ID\n");
    }

    const Destination* dst = &instr->dsts[0];
    const IlcRegister* dstReg = findOrCreateRegister(compiler, dst->registerType, dst->registerNum);

    if (dstReg == NULL) {
        LOGE("destination register %d %d not found\n", dst->registerType, dst->registerNum);
        return;
    }

    IlcSpvId srcId  = loadSource(compiler, &instr->srcs[0], COMP_MASK_XY, ilcSpvPutVectorType(compiler->module, compiler->intId, 2));
    IlcSpvWord indexIndex = COMP_INDEX_X;
    IlcSpvId indexId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcId,
                                                 1, &indexIndex);
    IlcSpvWord offsetIndex = COMP_INDEX_Y;
    IlcSpvId offsetId = ilcSpvPutCompositeExtract(compiler->module, compiler->intId, srcId,
                                                  1, &offsetIndex);

    // addr = (index * stride + offset) / 4
    const IlcSpvId mulIds[] = { indexId, resource->strideId };
    IlcSpvId baseId = ilcSpvPutAlu(compiler->module, SpvOpIMul, compiler->intId, 2, mulIds);
    const IlcSpvId addIds[] = { baseId, offsetId };
    IlcSpvId byteAddrId = ilcSpvPutAlu(compiler->module, SpvOpIAdd, compiler->intId, 2, addIds);
    const IlcSpvId divIds[] = {
        byteAddrId, ilcSpvPutConstant(compiler->module, compiler->intId, 4)
    };
    IlcSpvId wordAddrId = ilcSpvPutAlu(compiler->module, SpvOpSDiv, compiler->intId, 2, divIds);

    IlcSpvId resourceId = ilcSpvPutLoad(compiler->module, resource->typeId, resource->id);
    IlcSpvId fetchId = ilcSpvPutImageFetch(compiler->module, compiler->int4Id, resourceId,
                                           wordAddrId);
    storeDestination(compiler, dst,
                     ilcSpvPutBitcast(compiler->module, compiler->float4Id, fetchId));
}

static void emitInstr(
    IlcCompiler* compiler,
    const Instruction* instr)
{
    switch (instr->opcode) {
    case IL_OP_ABS:
    case IL_OP_ACOS:
    case IL_OP_ADD:
    case IL_OP_ASIN:
    case IL_OP_ATAN:
    case IL_OP_DIV:
    case IL_OP_DP3:
    case IL_OP_DP4:
    case IL_OP_FRC:
    case IL_OP_MAD:
    case IL_OP_MAX:
    case IL_OP_MIN:
    case IL_OP_MOV:
    case IL_OP_MUL:
    case IL_OP_FTOI:
    case IL_OP_ITOF:
    case IL_OP_ROUND_NEG_INF:
    case IL_OP_ROUND_PLUS_INF:
    case IL_OP_EXP_VEC:
    case IL_OP_LOG_VEC:
    case IL_OP_RSQ_VEC:
    case IL_OP_SIN_VEC:
    case IL_OP_COS_VEC:
    case IL_OP_SQRT_VEC:
    case IL_OP_DP2:
        emitFloatOp(compiler, instr);
        break;
    case IL_OP_EQ:
    case IL_OP_GE:
    case IL_OP_LT:
    case IL_OP_NE:
        emitFloatComparisonOp(compiler, instr);
        break;
    case IL_OP_I_NOT:
    case IL_OP_I_OR:
    case IL_OP_I_ADD:
    case IL_OP_AND:
    case IL_OP_U_BIT_EXTRACT:
        emitIntegerOp(compiler, instr);
        break;
    case IL_OP_I_EQ:
    case IL_OP_I_GE:
    case IL_OP_I_LT:
        emitIntegerComparisonOp(compiler, instr);
        break;
    case IL_OP_CONTINUE:
        emitContinue(compiler, instr);
        break;
    case IL_OP_ELSE:
        emitElse(compiler, instr);
        break;
    case IL_OP_END:
    case IL_OP_ENDMAIN:
        if (compiler->isInFunction) {
            ilcSpvPutFunctionEnd(compiler->module);
            compiler->isInFunction = false;
        }
        break;
    case IL_OP_ENDIF:
        emitEndIf(compiler, instr);
        break;
    case IL_OP_ENDLOOP:
        emitEndLoop(compiler, instr);
        break;
    case IL_OP_BREAK:
    case IL_OP_BREAK_LOGICALZ:
    case IL_OP_BREAK_LOGICALNZ:
        emitBreak(compiler, instr);
        break;
    case IL_OP_IF_LOGICALZ:
    case IL_OP_IF_LOGICALNZ:
        emitIf(compiler, instr);
        break;
    case IL_OP_WHILE:
        emitWhile(compiler, instr);
        break;
    case IL_OP_RET_DYN:
        ilcSpvPutReturn(compiler->module);
        break;
    case IL_DCL_LITERAL:
        emitLiteral(compiler, instr);
        break;
    case IL_DCL_OUTPUT:
        emitOutput(compiler, instr);
        break;
    case IL_DCL_INPUT:
        emitInput(compiler, instr);
        break;
    case IL_DCL_RESOURCE:
        emitResource(compiler, instr);
        break;
    case IL_OP_LOAD:
        emitLoad(compiler, instr);
        break;
    case IL_OP_SAMPLE:
    case IL_OP_SAMPLE_B:
    case IL_OP_SAMPLE_L:
    case IL_OP_SAMPLE_G:
    case IL_OP_SAMPLE_C:
    case IL_OP_SAMPLE_C_B:
    case IL_OP_SAMPLE_C_L:
    case IL_OP_SAMPLE_C_G:
    case IL_OP_SAMPLE_C_LZ:
        emitSample(compiler, instr);
        break;
    case IL_OP_CMOV_LOGICAL:
        emitCmovLogical(compiler, instr);
        break;
    case IL_OP_DCL_STRUCT_SRV:
        emitStructuredSrv(compiler, instr);
        break;
    case IL_OP_SRV_STRUCT_LOAD:
        emitStructuredSrvLoad(compiler, instr);
        break;
    case IL_DCL_GLOBAL_FLAGS:
        emitGlobalFlags(compiler, instr);
        break;
    default:
        LOGW("unhandled instruction %d\n", instr->opcode);
    }
}

static void emitEntryPoint(
    IlcCompiler* compiler)
{
    IlcSpvWord execution = 0;
    const char* name = "main";

    switch (compiler->kernel->shaderType) {
    case IL_SHADER_VERTEX:
        execution = SpvExecutionModelVertex;
        break;
    case IL_SHADER_PIXEL:
        execution = SpvExecutionModelFragment;
        break;
    case IL_SHADER_GEOMETRY:
        execution = SpvExecutionModelGeometry;
        break;
    case IL_SHADER_COMPUTE:
        execution = SpvExecutionModelGLCompute;
        break;
    case IL_SHADER_HULL:
        execution = SpvExecutionModelTessellationControl;
        break;
    case IL_SHADER_DOMAIN:
        execution = SpvExecutionModelTessellationEvaluation;
        break;
    }

    unsigned samplerCount = 0;
    for (int i = 0; i < 16; ++i) {
        if (compiler->samplerResources[i] != 0) {
            samplerCount++;
        }
    }

    unsigned interfaceCount = compiler->regCount + compiler->resourceCount + samplerCount;
    IlcSpvWord* interfaces = malloc(sizeof(IlcSpvWord) * interfaceCount);
    for (int i = 0; i < compiler->regCount; i++) {
        const IlcRegister* reg = &compiler->regs[i];

        interfaces[i] = reg->id;
    }

    for (int i = 0; i < compiler->resourceCount; i++) {
        const IlcResource* resource = &compiler->resources[i];

        interfaces[compiler->regCount + i] = resource->id;
    }

    unsigned intIndex = compiler->regCount + compiler->resourceCount;
    for (int i = 0; i < 16; i++) {
        if (compiler->samplerResources[i] != 0) {
            interfaces[intIndex++] = compiler->samplerResources[i];
        }
    }
    ilcSpvPutEntryPoint(compiler->module, compiler->entryPointId, execution, name,
                        interfaceCount, interfaces);
    ilcSpvPutName(compiler->module, compiler->entryPointId, name);

    switch (compiler->kernel->shaderType) {
    case IL_SHADER_PIXEL:
        ilcSpvPutExecMode(compiler->module, compiler->entryPointId,
                          SpvExecutionModeOriginUpperLeft);
        break;
    }

    free(interfaces);
}

uint32_t* ilcCompileKernel(
    uint32_t* size,
    const Kernel* kernel)
{
    IlcSpvModule module;

    ilcSpvInit(&module);

    IlcSpvId intId = ilcSpvPutIntType(&module, true);
    IlcSpvId uintId = ilcSpvPutIntType(&module, true);
    IlcSpvId floatId = ilcSpvPutFloatType(&module);
    IlcSpvId boolId = ilcSpvPutBoolType(&module);

    IlcCompiler compiler = {
        .module = &module,
        .kernel = kernel,
        .entryPointId = ilcSpvAllocId(&module),
        .intId = intId,
        .int4Id = ilcSpvPutVectorType(&module, intId, 4),
        .floatId = floatId,
        .float4Id = ilcSpvPutVectorType(&module, floatId, 4),
        .uintId = uintId,
        .zeroUintId = 0,//lazy
        .boolId = boolId,
        .bool4Id = ilcSpvPutVectorType(&module, boolId, 4),
        .samplerId = 0,
        .regCount = 0,
        .regs = NULL,
        .resourceCount = 0,
        .resources = NULL,
        .samplerResources = {},
        .controlFlowBlockCount = 0,
        .controlFlowBlocks = NULL,
        .isInFunction = true,
    };

    emitFunc(&compiler, compiler.entryPointId);
    for (int i = 0; i < kernel->instrCount; i++) {
        emitInstr(&compiler, &kernel->instrs[i]);
    }

    emitEntryPoint(&compiler);

    free(compiler.regs);
    free(compiler.resources);
    free(compiler.controlFlowBlocks);
    ilcSpvFinish(&module);

    *size = sizeof(IlcSpvWord) * module.buffer[ID_MAIN].wordCount;
    return module.buffer[ID_MAIN].words;
}
