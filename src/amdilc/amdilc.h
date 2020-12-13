#ifndef AMDILC_H_
#define AMDILC_H_

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <mantle/mantle.h>

enum IlcDescriptorResourceTable {
    TABLE_UNIFORM_TEXEL_BUFFER = 0,
    TABLE_STORAGE_TEXEL_BUFFER = 1,
    TABLE_STORAGE_IMAGE = 2,
    TABLE_SAMPLED_IMAGE = 3,
    TABLE_SAMPLER = 4,
    TABLE_MAX_ID  = 5,
};

uint32_t* ilcCompileShader(
    unsigned* compiledSize,
    const GR_DESCRIPTOR_SET_MAPPING* mappings,
    const void* code,
    unsigned size);

void ilcDisassembleShader(
    FILE* file,
    const void* code,
    unsigned size);

#endif // AMDILC_H_
