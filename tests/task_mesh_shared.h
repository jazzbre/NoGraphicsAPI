#pragma once

#include "task_shader_common.h"

struct TaskMeshRoot
{
    GPU_PTR(TaskTestData) data;
    uint32 count;
    uint32 visible_mask;
    float4 color;
};
