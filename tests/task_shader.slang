#include "task_shader_shared.h"

NGA_ROOT(TaskShaderRoot, root);

groupshared TaskTestPayload compacted;
groupshared uint visible_count;

[shader("amplification")]
[numthreads(task_test_batch_size, 1, 1)]
void taskMain(uint lane : SV_GroupIndex, uint3 group_id : SV_GroupID)
{
    if (lane == 0) visible_count = 0;
    GroupMemoryBarrierWithGroupSync();
    uint id = group_id.x * task_test_batch_size + lane;
    if (id < root.count && (GPU_FIELD_LOAD(root.data, visibility, id) & root.visible_mask) != 0)
    {
        uint slot;
        InterlockedAdd(visible_count, 1, slot);
        compacted.ids[slot] = id;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) GPU_FIELD_STORE(root.data, batch_counts, group_id.x, visible_count);
    DispatchMesh(visible_count, 1, 1, compacted);
}
