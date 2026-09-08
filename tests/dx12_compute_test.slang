#include "dx12_test_shared.h"
NGA_ROOT(Dx12TestRoot, root);

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 thread_id : SV_DispatchThreadID)
{
    uint result = 0;
    switch (root.operation)
    {
    case 0: result = 42 + thread_id.x; break;
    case 1:
    {
        Texture1D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture.Load(int2(0, 0));
        break;
    }
    case 2:
    {
        Texture2D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture.Load(int3(0, 0, 0));
        break;
    }
    case 3:
    {
        Texture3D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture.Load(int4(0, 0, 0, 0));
        break;
    }
    case 4:
    {
        RWTexture1D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture[0];
        texture[0] = result + 1;
        break;
    }
    case 5:
    {
        RWTexture2D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture[uint2(0, 0)];
        texture[uint2(0, 0)] = result + 1;
        break;
    }
    case 6:
    {
        RWTexture3D<uint> texture = ResourceDescriptorHeap[root.descriptor_index];
        result = texture[uint3(0, 0, 0)];
        texture[uint3(0, 0, 0)] = result + 1;
        break;
    }
    case 7:
    {
        TextureCubeArray<float4> texture = ResourceDescriptorHeap[root.descriptor_index];
        SamplerState sampler = SamplerDescriptorHeap[0];
        result = uint(round(texture.SampleLevel(sampler, float4(1, 0, 0, 1), 0).x * 255));
        break;
    }
    }
    root.output[thread_id.x] = result;
}
