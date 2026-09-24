#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

struct UploadQueueRoot
{
    GPU_PTR(uint32) source;
    GPU_PTR(uint32) destination;
    uint32 count;
};

static const uint32 upload_queue_thread_count = 64;
