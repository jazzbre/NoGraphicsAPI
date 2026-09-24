#pragma once

#if defined(__SLANG__)

#if defined(NGA_VULKAN)

// Push data holds the root structure directly. C layout keeps every field at its C++ offset.
#define NGA_ROOT(type, name) [[vk::push_constant]] ConstantBuffer<type> name

#define GPU_PTR(T) T*

#elif defined(NGA_D3D12)

// Root constants arrive as raw words because HLSL constant-buffer packing places matrices and
// vectors at different offsets than C. Unpacking the words keeps one root ABI across backends.
#define NGA_ROOT(type, name)                                                                                                                                   \
    cbuffer NgaRootConstants : register(b0)                                                                                                                    \
    {                                                                                                                                                          \
        uint4 nga_root_words[16];                                                                                                                              \
    }                                                                                                                                                          \
    property type name                                                                                                                                         \
    {                                                                                                                                                          \
        get                                                                                                                                                    \
        {                                                                                                                                                      \
            return reinterpret<type>(nga_root_words);                                                                                                          \
        }                                                                                                                                                      \
    }

// Encoded NoGraphicsAPI address: [63:40] one-based heap id, [39:0] byte offset.
// Each heap reserves an adjacent descriptor pair above the application descriptors:
// the first is its ByteAddressBuffer SRV, the second its RWByteAddressBuffer UAV.
// The backend places those descriptors at the same index this arithmetic produces.
static const uint nga_internal_descriptor_base = 32768;

struct GpuPtr<T>
{
    uint64_t address;

    uint descriptor_index()
    {
        return nga_internal_descriptor_base + (uint(address >> 40) - 1) * 2;
    }
    uint byte_offset()
    {
        return uint(address & 0xffffffffffull);
    }

    uint load_word(uint offset)
    {
        ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(descriptor_index())];
        return buffer.Load<uint>(byte_offset() + offset);
    }
    void store_word(uint offset, uint value)
    {
        RWByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(descriptor_index() + 1)];
        buffer.Store<uint>(byte_offset() + offset, value);
    }
    void add_word(uint offset, uint value)
    {
        RWByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(descriptor_index() + 1)];
        buffer.InterlockedAdd(byte_offset() + offset, value);
    }

    __subscript(uint index)->T
    {
        get
        {
            ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(descriptor_index())];
            return buffer.Load<T>(byte_offset() + index * uint(sizeof(T)));
        }
        [nonmutating] set {
            RWByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(descriptor_index() + 1)];
            buffer.Store<T>(byte_offset() + index * uint(sizeof(T)), newValue);
        }
    }
}

#define GPU_PTR(T) GpuPtr<T>

#else

#error NoGraphicsAPI shader backend not defined

#endif

#else

#define GPU_PTR(T) T*

#endif
