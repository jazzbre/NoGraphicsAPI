#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>

struct Dx12TestRoot
{
    GPU_PTR(uint32) output;
    uint32 descriptor_index;
    uint32 operation;
};
