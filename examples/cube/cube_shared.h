#pragma once

#include <NoGraphicsAPIUtility/shader_platform.h>
#include <NoGraphicsAPIUtility/shader_types.h>

struct CubeVertex
{
    float4 position;
    float2 uv;
};

struct CubeRootArguments
{
    GPU_PTR(CubeVertex) vertices;
    float4x4 transform;
};
