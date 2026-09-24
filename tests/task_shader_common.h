#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

static const uint32 task_test_batch_size = 32;
static const uint32 task_test_capacity = 96;
static const uint32 task_test_batches = task_test_capacity / task_test_batch_size;
static const uint32 task_test_width = task_test_capacity * 4;
static const uint32 task_test_height = 8;

struct TaskTestPayload
{
    uint32 ids[task_test_batch_size];
};

struct TaskTestData
{
    uint32 visibility[task_test_capacity];
    uint32 batch_counts[task_test_batches];
    uint32 visits[task_test_capacity + 1];
};

#if defined(__SLANG__)
static const uint32 task_test_visibility_offset = 0;
static const uint32 task_test_batch_counts_offset = task_test_capacity * 4;
static const uint32 task_test_visits_offset = (task_test_capacity + task_test_batches) * 4;
#if defined(NGA_D3D12)
#define GPU_FIELD_LOAD(pointer, field, index) pointer.load_word(task_test_##field##_offset + (index) * 4)
#define GPU_FIELD_STORE(pointer, field, index, value) pointer.store_word(task_test_##field##_offset + (index) * 4, value)
#define GPU_FIELD_ADD(pointer, field, index, value) pointer.add_word(task_test_##field##_offset + (index) * 4, value)
#else
#define GPU_FIELD_LOAD(pointer, field, index) pointer->field[index]
#define GPU_FIELD_STORE(pointer, field, index, value) pointer->field[index] = value
#define GPU_FIELD_ADD(pointer, field, index, value) InterlockedAdd(pointer->field[index], value)
#endif
#endif
