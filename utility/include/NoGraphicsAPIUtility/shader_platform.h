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
        get { return reinterpret<type>(nga_root_words); }                                                                                                      \
    }

// Encoded NoGraphicsAPI address: [63:40] internal descriptor index, [39:0] byte offset.
// A heap reserves an adjacent descriptor pair: the even index is its ByteAddressBuffer SRV,
// the odd index its RWByteAddressBuffer UAV.
struct GpuPtr<T>
{
    uint64_t address;

    __subscript(uint index)->T
    {
        get
        {
            ByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(uint(address >> 40))];
            return buffer.Load<T>(uint(address & 0xffffffffffull) + index * uint(sizeof(T)));
        }
        [nonmutating] set
        {
            RWByteAddressBuffer buffer = ResourceDescriptorHeap[NonUniformResourceIndex(uint(address >> 40) + 1)];
            buffer.Store<T>(uint(address & 0xffffffffffull) + index * uint(sizeof(T)), newValue);
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
