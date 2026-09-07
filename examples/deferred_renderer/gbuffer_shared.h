#pragma once

#include "object_data.h"

struct GBufferRoot
{
    GPU_PTR(ObjectData) objects;
    float4x4 view_projection;
    float3x4 orientation;
};

static const uint32 object_grid_width = 512;
