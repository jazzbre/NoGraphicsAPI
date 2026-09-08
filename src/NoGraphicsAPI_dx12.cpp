#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Keep assert expressions type-checked in release without evaluating them.
#if defined(NDEBUG)
#undef assert
#define assert(expression) ((void)sizeof(static_cast<bool>(expression)))
#endif

namespace gpu {

    namespace {

        constexpr uint32 format_count = static_cast<uint32>(Format::undefined);
        constexpr uint32 max_color_attachments = 8;
        constexpr uint32 initial_command_context_count = 2;
        constexpr uint32 max_swapchain_images = 8;
        constexpr uint32 gpu_allocation_alignment = 16;
        constexpr uint32 root_constant_count = 64; // 64 DWORDs is the whole D3D12 root signature budget.
        constexpr uint64 max_push_data_size = root_constant_count * sizeof(uint32);

        // Encoded GPU address layout. Heap ids start at one so a null GpuRange stays null.
        constexpr uint32 gpu_address_offset_bits = 40;
        constexpr uint64 gpu_address_offset_mask = (uint64{ 1 } << gpu_address_offset_bits) - 1;
        constexpr uint32 max_heap_id = (1u << 24) - 1;

        // Application descriptors occupy the front of the device heap so shader index zero is the
        // first descriptor the application writes. Internal linear-heap views sit above them at
        // internal_descriptor_base + (heap id - 1) * 2, which is the index GpuPtr computes in
        // shader_platform.h; the two constants must stay equal.
        constexpr uint32 resource_descriptor_capacity = 65536;
        constexpr uint32 internal_descriptor_base = 32768;
        constexpr uint32 sampler_descriptor_capacity = 2048;

        [[nodiscard]] Error error_from_hresult(HRESULT result) noexcept
        {
            switch (result)
            {
            case S_OK: return Error::none;
            case DXGI_ERROR_DEVICE_REMOVED:
            case DXGI_ERROR_DEVICE_RESET:
            case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return Error::device_lost;
            case DXGI_ERROR_UNSUPPORTED:
            case E_NOTIMPL: return Error::unsupported;
            default: return Error::driver_error;
            }
        }

        void drain_debug_messages(ID3D12InfoQueue* info_queue) noexcept;

        ID3D12InfoQueue* active_info_queue = nullptr; // Debug diagnostics only; the API is single-threaded.

        void require_hr(HRESULT result) noexcept
        {
            if (FAILED(result))
            {
#if !defined(NDEBUG)
                drain_debug_messages(active_info_queue);
                fprintf(stderr, "NoGraphicsAPI: D3D12 call failed with 0x%08lx\n", static_cast<unsigned long>(result));
#endif
                abort();
            }
        }

        void assert_hr(HRESULT result) noexcept
        {
#if !defined(NDEBUG)
            if (FAILED(result))
            {
                drain_debug_messages(active_info_queue);
                fprintf(stderr, "NoGraphicsAPI: D3D12 call failed with 0x%08lx\n", static_cast<unsigned long>(result));
            }
#endif
            assert(SUCCEEDED(result));
            (void)result;
        }

        void drain_debug_messages(ID3D12InfoQueue* info_queue) noexcept
        {
            if (!info_queue)
                return;
            const uint64 count = info_queue->GetNumStoredMessages();
            for (uint64 index = 0; index < count; ++index)
            {
                size_t length = 0;
                if (FAILED(info_queue->GetMessage(index, nullptr, &length)) || length == 0)
                    continue;
                D3D12_MESSAGE* message = static_cast<D3D12_MESSAGE*>(malloc(length));
                if (SUCCEEDED(info_queue->GetMessage(index, message, &length)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                {
                    fputs("NoGraphicsAPI validation: ", stderr);
                    fputs(message->pDescription, stderr);
                    fputc('\n', stderr);
                }
                free(message);
            }
            info_queue->ClearStoredMessages();
        }

        template<typename T> void release(T*& object) noexcept
        {
            if (object)
            {
                object->Release();
                object = nullptr;
            }
        }

        [[nodiscard]] constexpr uint64 align_up(uint64 value, uint64 alignment) noexcept
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        struct FormatMapping
        {
            DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN; // Typeless where a depth texture is also sampled.
            DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;     // Fully typed view format.
            DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;    // Depth-stencil view format, if any.
        };

        [[nodiscard]] FormatMapping map_format(Format format) noexcept
        {
            switch (format)
            {
            case Format::r8_srgb: return { .resource = DXGI_FORMAT_R8_UNORM, .view = DXGI_FORMAT_R8_UNORM };
            case Format::rg8_srgb: return { .resource = DXGI_FORMAT_R8G8_UNORM, .view = DXGI_FORMAT_R8G8_UNORM };
            case Format::rgba8_srgb: return { .resource = DXGI_FORMAT_R8G8B8A8_TYPELESS, .view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
            case Format::bgra8_srgb: return { .resource = DXGI_FORMAT_B8G8R8A8_TYPELESS, .view = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
            case Format::rgba4_unorm: return { .resource = DXGI_FORMAT_B4G4R4A4_UNORM, .view = DXGI_FORMAT_B4G4R4A4_UNORM };
            case Format::r5g5b5a1_unorm: return { .resource = DXGI_FORMAT_B5G5R5A1_UNORM, .view = DXGI_FORMAT_B5G5R5A1_UNORM };
            case Format::r5g6b5_unorm: return { .resource = DXGI_FORMAT_B5G6R5_UNORM, .view = DXGI_FORMAT_B5G6R5_UNORM };
            case Format::r8_unorm: return { .resource = DXGI_FORMAT_R8_UNORM, .view = DXGI_FORMAT_R8_UNORM };
            case Format::rg8_unorm: return { .resource = DXGI_FORMAT_R8G8_UNORM, .view = DXGI_FORMAT_R8G8_UNORM };
            case Format::rgba8_unorm: return { .resource = DXGI_FORMAT_R8G8B8A8_TYPELESS, .view = DXGI_FORMAT_R8G8B8A8_UNORM };
            case Format::bgra8_unorm: return { .resource = DXGI_FORMAT_B8G8R8A8_TYPELESS, .view = DXGI_FORMAT_B8G8R8A8_UNORM };
            case Format::r16_unorm: return { .resource = DXGI_FORMAT_R16_UNORM, .view = DXGI_FORMAT_R16_UNORM };
            case Format::rg16_unorm: return { .resource = DXGI_FORMAT_R16G16_UNORM, .view = DXGI_FORMAT_R16G16_UNORM };
            case Format::rgba16_unorm: return { .resource = DXGI_FORMAT_R16G16B16A16_UNORM, .view = DXGI_FORMAT_R16G16B16A16_UNORM };
            case Format::r8_uint: return { .resource = DXGI_FORMAT_R8_UINT, .view = DXGI_FORMAT_R8_UINT };
            case Format::rg8_uint: return { .resource = DXGI_FORMAT_R8G8_UINT, .view = DXGI_FORMAT_R8G8_UINT };
            case Format::rgba8_uint: return { .resource = DXGI_FORMAT_R8G8B8A8_UINT, .view = DXGI_FORMAT_R8G8B8A8_UINT };
            case Format::bgra8_uint: return { .resource = DXGI_FORMAT_R8G8B8A8_UINT, .view = DXGI_FORMAT_R8G8B8A8_UINT };
            case Format::r16_uint: return { .resource = DXGI_FORMAT_R16_UINT, .view = DXGI_FORMAT_R16_UINT };
            case Format::rg16_uint: return { .resource = DXGI_FORMAT_R16G16_UINT, .view = DXGI_FORMAT_R16G16_UINT };
            case Format::rgba16_uint: return { .resource = DXGI_FORMAT_R16G16B16A16_UINT, .view = DXGI_FORMAT_R16G16B16A16_UINT };
            case Format::r32_uint: return { .resource = DXGI_FORMAT_R32_UINT, .view = DXGI_FORMAT_R32_UINT };
            case Format::rg32_uint: return { .resource = DXGI_FORMAT_R32G32_UINT, .view = DXGI_FORMAT_R32G32_UINT };
            case Format::rgb32_uint: return { .resource = DXGI_FORMAT_R32G32B32_UINT, .view = DXGI_FORMAT_R32G32B32_UINT };
            case Format::rgba32_uint: return { .resource = DXGI_FORMAT_R32G32B32A32_UINT, .view = DXGI_FORMAT_R32G32B32A32_UINT };
            case Format::r16_float: return { .resource = DXGI_FORMAT_R16_FLOAT, .view = DXGI_FORMAT_R16_FLOAT };
            case Format::rg16_float: return { .resource = DXGI_FORMAT_R16G16_FLOAT, .view = DXGI_FORMAT_R16G16_FLOAT };
            case Format::rgba16_float: return { .resource = DXGI_FORMAT_R16G16B16A16_FLOAT, .view = DXGI_FORMAT_R16G16B16A16_FLOAT };
            case Format::r32_float: return { .resource = DXGI_FORMAT_R32_FLOAT, .view = DXGI_FORMAT_R32_FLOAT };
            case Format::rg32_float: return { .resource = DXGI_FORMAT_R32G32_FLOAT, .view = DXGI_FORMAT_R32G32_FLOAT };
            case Format::rgb32_float: return { .resource = DXGI_FORMAT_R32G32B32_FLOAT, .view = DXGI_FORMAT_R32G32B32_FLOAT };
            case Format::rgba32_float: return { .resource = DXGI_FORMAT_R32G32B32A32_FLOAT, .view = DXGI_FORMAT_R32G32B32A32_FLOAT };
            case Format::rgb10a2_unorm: return { .resource = DXGI_FORMAT_R10G10B10A2_UNORM, .view = DXGI_FORMAT_R10G10B10A2_UNORM };
            case Format::rg11b10_float: return { .resource = DXGI_FORMAT_R11G11B10_FLOAT, .view = DXGI_FORMAT_R11G11B10_FLOAT };
            case Format::d16_unorm: return { .resource = DXGI_FORMAT_R16_TYPELESS, .view = DXGI_FORMAT_R16_UNORM, .depth = DXGI_FORMAT_D16_UNORM };
            case Format::d24_unorm_s8_uint:
                return { .resource = DXGI_FORMAT_R24G8_TYPELESS, .view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS, .depth = DXGI_FORMAT_D24_UNORM_S8_UINT };
            case Format::d32_float: return { .resource = DXGI_FORMAT_R32_TYPELESS, .view = DXGI_FORMAT_R32_FLOAT, .depth = DXGI_FORMAT_D32_FLOAT };
            case Format::s8_uint: return {};
            case Format::d32_float_s8_uint:
                return { .resource = DXGI_FORMAT_R32G8X24_TYPELESS, .view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, .depth = DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
            case Format::eac_rg: return {}; // ETC/EAC has no D3D12 equivalent.
            case Format::astc_4x4_srgb: return {};
            case Format::astc_4x4_unorm: return {};
            case Format::bc3_srgb: return { .resource = DXGI_FORMAT_BC3_TYPELESS, .view = DXGI_FORMAT_BC3_UNORM_SRGB };
            case Format::bc3_unorm: return { .resource = DXGI_FORMAT_BC3_TYPELESS, .view = DXGI_FORMAT_BC3_UNORM };
            case Format::bc5_rg: return { .resource = DXGI_FORMAT_BC5_UNORM, .view = DXGI_FORMAT_BC5_UNORM };
            case Format::bc7_srgb: return { .resource = DXGI_FORMAT_BC7_TYPELESS, .view = DXGI_FORMAT_BC7_UNORM_SRGB };
            case Format::bc7_unorm: return { .resource = DXGI_FORMAT_BC7_TYPELESS, .view = DXGI_FORMAT_BC7_UNORM };
            case Format::undefined: return {};
            }
            return {};
        }

        // Flip-model swapchains reject _SRGB formats, so the buffers are created with the plain
        // format and the sRGB encoding is applied by the render target view instead.
        [[nodiscard]] DXGI_FORMAT map_swapchain_format(Format format) noexcept
        {
            switch (format)
            {
            case Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::bgra8_srgb: return DXGI_FORMAT_B8G8R8A8_UNORM;
            default: return map_format(format).view;
            }
        }

        [[nodiscard]] uint32 mip_extent(uint32 extent, uint32 mip_level) noexcept
        {
            return (extent >> mip_level) != 0 ? extent >> mip_level : 1;
        }

        [[nodiscard]] D3D12_COMPARISON_FUNC map_compare(CompareOp compare) noexcept
        {
            switch (compare)
            {
            case CompareOp::never: return D3D12_COMPARISON_FUNC_NEVER;
            case CompareOp::less: return D3D12_COMPARISON_FUNC_LESS;
            case CompareOp::equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case CompareOp::less_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case CompareOp::greater: return D3D12_COMPARISON_FUNC_GREATER;
            case CompareOp::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case CompareOp::greater_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case CompareOp::always: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
            return D3D12_COMPARISON_FUNC_ALWAYS;
        }

        [[nodiscard]] D3D12_TEXTURE_ADDRESS_MODE map_address_mode(AddressMode mode) noexcept
        {
            switch (mode)
            {
            case AddressMode::repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case AddressMode::mirrored_repeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case AddressMode::clamp_to_edge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }

        [[nodiscard]] D3D12_FILTER map_filter(const SamplerDesc& desc) noexcept
        {
            if (desc.anisotropic)
                return desc.compare_enabled ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
            const uint32 bits = (desc.min_filter == Filter::linear ? 0x4u : 0u) | (desc.mag_filter == Filter::linear ? 0x2u : 0u) |
                                (desc.mip_filter == Filter::linear ? 0x1u : 0u);
            const D3D12_FILTER table[8]{
                D3D12_FILTER_MIN_MAG_MIP_POINT,        D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
                D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR, D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
                D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            };
            const D3D12_FILTER filter = table[bits];
            if (!desc.compare_enabled)
                return filter;
            return static_cast<D3D12_FILTER>(filter | 0x80u); // Comparison filters share the base encoding.
        }

        [[nodiscard]] D3D12_BARRIER_SYNC map_sync(Stage stage) noexcept
        {
            const uint64 bits = static_cast<uint64>(stage);
            if (bits == 0)
                return D3D12_BARRIER_SYNC_NONE;
            if ((bits & static_cast<uint64>(Stage::all_commands)) != 0)
                return D3D12_BARRIER_SYNC_ALL;

            D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
            if ((bits & static_cast<uint64>(Stage::indirect)) != 0)
                sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
            if ((bits & static_cast<uint64>(Stage::index_input)) != 0)
                sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
            if ((bits & static_cast<uint64>(Stage::vertex)) != 0)
                sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
            if ((bits & static_cast<uint64>(Stage::mesh)) != 0)
                sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
            if ((bits & static_cast<uint64>(Stage::depth_stencil_tests)) != 0)
                sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
            if ((bits & static_cast<uint64>(Stage::fragment)) != 0)
                sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
            if ((bits & static_cast<uint64>(Stage::color_output)) != 0)
                sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
            if ((bits & static_cast<uint64>(Stage::compute)) != 0)
                sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            if ((bits & static_cast<uint64>(Stage::transfer)) != 0)
                sync |= D3D12_BARRIER_SYNC_COPY;
            // Stage::host has no D3D12 sync scope; queue submission already orders host writes.
            return sync;
        }

        // D3D12 ties an access to a layout, so a global barrier may not name an attachment access:
        // it identifies no resource and therefore cannot transition one. Vulkan keeps layout on image
        // barriers alone, which is why the same mask is legal there. Returns false for those accesses,
        // leaving the caller to fall back to common access.
        [[nodiscard]] bool map_global_access(Access access, D3D12_BARRIER_ACCESS& output) noexcept
        {
            constexpr uint64 attachment_access = static_cast<uint64>(Access::color_read) | static_cast<uint64>(Access::color_write) |
                                                 static_cast<uint64>(Access::depth_stencil_read) | static_cast<uint64>(Access::depth_stencil_write);
            const uint64 bits = static_cast<uint64>(access);
            if ((bits & attachment_access) != 0)
                return false;

            D3D12_BARRIER_ACCESS result = D3D12_BARRIER_ACCESS_COMMON;
            if ((bits & static_cast<uint64>(Access::transfer_read)) != 0)
                result |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
            if ((bits & static_cast<uint64>(Access::transfer_write)) != 0)
                result |= D3D12_BARRIER_ACCESS_COPY_DEST;
            if ((bits & static_cast<uint64>(Access::shader_read)) != 0)
                result |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            if ((bits & static_cast<uint64>(Access::shader_write)) != 0)
                result |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            if ((bits & static_cast<uint64>(Access::indirect_read)) != 0)
                result |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
            if ((bits & static_cast<uint64>(Access::index_read)) != 0)
                result |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
            output = result;
            return true;
        }

    } // namespace

    struct GpuHeapOwner
    {
        Device* state = nullptr;
        void* object = nullptr;
    };

    struct TextureHeapOwner
    {
        Device* state = nullptr;
        ID3D12Heap* heap = nullptr;
    };

    struct TimelineSemaphore
    {
        Device* state = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE event = nullptr;
    };

    namespace detail {

        // One linear or descriptor allocation. Linear heaps own a buffer resource and a pair of
        // internal descriptors; descriptor heaps own a CPU shadow and a slot range in the device heap.
        struct GpuHeapRecord
        {
            GpuHeapOwner owner{};
            Device* state = nullptr;
            uint32 heap_id = 0;
            MemoryType memory = MemoryType::cpu_visible;

            ID3D12Resource* resource = nullptr;
            void* mapped = nullptr;
            uint64 size = 0;
            uint32 shader_descriptor_index = 0; // SRV slot; the UAV is the next slot.

            byte* shadow = nullptr;     // Descriptor heaps only.
            uint32 descriptor_base = 0; // First device-heap slot backing this descriptor heap.
            uint32 descriptor_count = 0;
        };

        struct TextureCopyScratch
        {
            TextureCopyScratch* next = nullptr;
            ID3D12Resource* resource = nullptr;
            uint64 size = 0;
        };

        struct IndirectSignature
        {
            D3D12_INDIRECT_ARGUMENT_TYPE type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
            uint32 stride = 0;
            ID3D12CommandSignature* signature = nullptr;
        };

        struct CommandContext
        {
            CommandContext* next = nullptr;
            CommandContext* previous = nullptr;
            ID3D12CommandAllocator* allocator = nullptr;
            ID3D12GraphicsCommandList8* list = nullptr;
            CommandBuffer* commands = nullptr;
            TextureCopyScratch* texture_copy_scratch = nullptr;
            uint64 retire_value = 0;
            bool active = false;
        };

    } // namespace detail

    struct Texture
    {
        Device* state = nullptr;
        ID3D12Resource* resource = nullptr;
        TextureDesc desc{};
        FormatMapping formats{};
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
    };

    struct RenderView
    {
        Device* state = nullptr;
        Texture* texture = nullptr;
        RenderViewDesc desc{};
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        bool depth_stencil = false;
        uint32 slot = 0;
    };

    // D3D12 bakes depth-stencil state into the pipeline while the API treats it as dynamic, so a
    // graphics PSO keeps its shaders and target formats and compiles one variant per state it sees.
    struct PSOVariant
    {
        DepthStencilState state{};
        ID3D12PipelineState* pipeline = nullptr;
    };

    struct PSO
    {
        Device* state = nullptr;
        ID3D12PipelineState* pipeline = nullptr; // Compute pipelines have exactly one.
        bool compute = false;
        bool mesh = false;

        byte* first_stage = nullptr; // Vertex or mesh shader.
        size_t first_stage_size = 0;
        byte* fragment_stage = nullptr;
        size_t fragment_stage_size = 0;

        ColorTargetDesc color_targets[max_color_attachments]{};
        uint32 color_target_count = 0;
        Format depth_format = Format::undefined;
        Format stencil_format = Format::undefined;
        RasterizationState rasterization{};

        PSOVariant* variants = nullptr;
        uint32 variant_count = 0;
        uint32 variant_capacity = 0;
    };

    struct Swapchain
    {
        Device* state = nullptr;
        IDXGISwapChain4* swapchain = nullptr;
        Format format = Format::undefined;
        uint32x2 extent{};
        uint32 image_count = 0;
        Texture* textures[max_swapchain_images]{};
        RenderView* views[max_swapchain_images]{};
        uint32 image_index = 0;
    };

    struct CommandBuffer
    {
        Device* state = nullptr;
        detail::CommandContext* context = nullptr;
        ID3D12GraphicsCommandList8* list = nullptr;
        bool recording = false;
        bool rendering = false;
        const PSO* bound_pso = nullptr;
        ID3D12PipelineState* bound_pipeline = nullptr;
        Swapchain* swapchain = nullptr;
        DepthStencilState depth_stencil{};
        RenderView* color_views[max_color_attachments]{};
        uint32 color_view_count = 0;
        RenderView* depth_view = nullptr;
    };

    struct Device
    {
        IDXGIFactory6* factory = nullptr;
        IDXGIAdapter4* adapter = nullptr;
        ID3D12Device10* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12RootSignature* root_signature = nullptr;
        ID3D12InfoQueue* info_queue = nullptr;

        ID3D12DescriptorHeap* resource_heap = nullptr; // Shader-visible CBV/SRV/UAV.
        ID3D12DescriptorHeap* sampler_heap = nullptr;  // Shader-visible samplers.
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        ID3D12DescriptorHeap* dsv_heap = nullptr;
        uint32 resource_descriptor_size = 0;
        uint32 sampler_descriptor_size = 0;
        uint32 rtv_descriptor_size = 0;
        uint32 dsv_descriptor_size = 0;
        bool resource_slots[internal_descriptor_base]{};
        bool sampler_slots[sampler_descriptor_capacity]{};
        bool rtv_slots[256]{};
        bool dsv_slots[256]{};
        bool unrestricted_texture_copy_pitch = false;
        detail::IndirectSignature* indirect_signatures = nullptr;
        uint32 indirect_signature_count = 0;
        uint32 indirect_signature_capacity = 0;

        detail::GpuHeapRecord** heaps = nullptr; // Indexed by heap id; slot zero stays null.
        uint32 heap_capacity = 0;
        uint32 heap_count = 1;
        uint32 available_heap_id = 1;

        ID3D12Fence* command_retirement = nullptr;
        HANDLE retirement_event = nullptr;
        uint64 command_retirement_value = 0;
        uint64 completed_command_retirement = 0;
        detail::CommandContext* next_command_context = nullptr;
        size_t command_context_count = 0;
        uint32 active_command_buffers = 0;
        ID3D12CommandList** submit_lists = nullptr;
        size_t submit_capacity = 0;

        HWND window = nullptr;
        Swapchain* swapchain = nullptr;
        Swapchain* acquired_swapchain = nullptr;

        DeviceCaps caps{};
        char device_name[128]{};
        bool format_supported[format_count]{};

        Device() = default;
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        [[nodiscard]] uint32 register_heap(detail::GpuHeapRecord* record) noexcept
        {
            for (; available_heap_id < heap_count; ++available_heap_id)
            {
                if (!heaps[available_heap_id])
                {
                    heaps[available_heap_id] = record;
                    return available_heap_id++;
                }
            }
            if (heap_count >= heap_capacity)
            {
                const uint32 capacity = heap_capacity == 0 ? 16u : heap_capacity * 2u;
                detail::GpuHeapRecord** grown = static_cast<detail::GpuHeapRecord**>(calloc(capacity, sizeof(detail::GpuHeapRecord*)));
                if (heaps)
                {
                    memcpy(grown, heaps, heap_capacity * sizeof(detail::GpuHeapRecord*));
                    free(heaps);
                }
                heaps = grown;
                heap_capacity = capacity;
            }
            assert(heap_count <= max_heap_id && "exhausted the 24-bit GPU pointer heap id space");
            const uint32 id = heap_count++;
            heaps[id] = record;
            available_heap_id = heap_count;
            return id;
        }

        [[nodiscard]] detail::GpuHeapRecord* find_heap(uint64 address) const noexcept
        {
            const uint32 id = static_cast<uint32>(address >> gpu_address_offset_bits);
            assert(id != 0 && id < heap_count && "GPU pointer does not belong to this device");
            detail::GpuHeapRecord* record = heaps[id];
            assert(record && "GPU pointer refers to a destroyed heap");
            return record;
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE resource_handle(uint32 slot) const noexcept
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = resource_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(slot) * resource_descriptor_size;
            return handle;
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE sampler_handle(uint32 slot) const noexcept
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = sampler_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += size_t(slot) * sampler_descriptor_size;
            return handle;
        }

        void poll_command_retirement() noexcept
        {
            if (!command_retirement || completed_command_retirement == command_retirement_value)
                return;
            const uint64 completed = command_retirement->GetCompletedValue();
            assert(completed >= completed_command_retirement && completed <= command_retirement_value);
            if (completed == completed_command_retirement)
                return;
            completed_command_retirement = completed;
            reset_retired_command_contexts();
        }

        void wait_command_retirement(uint64 value) noexcept
        {
            assert(value <= command_retirement_value);
            if (value > command_retirement->GetCompletedValue())
            {
                require_hr(command_retirement->SetEventOnCompletion(value, retirement_event));
                WaitForSingleObject(retirement_event, INFINITE);
            }
            if (value > completed_command_retirement)
                completed_command_retirement = value;
            reset_retired_command_contexts();
        }

        void reset_retired_command_contexts() noexcept;
        [[nodiscard]] Error create_command_context(detail::CommandContext& context) noexcept;
        [[nodiscard]] Error grow_command_context_pool() noexcept;
        [[nodiscard]] detail::CommandContext& acquire_command_context() noexcept;
        void destroy_command_contexts() noexcept;
    };

    namespace {

        // GPU addresses are opaque handles, not D3D12 virtual addresses. Keeping the byte offset in the
        // low bits lets application-side pointer arithmetic work without knowing about the encoding.
        [[nodiscard]] void* encode_gpu_ptr(uint32 heap_id, uint64 byte_offset) noexcept
        {
            assert(heap_id != 0 && heap_id <= max_heap_id);
            assert(byte_offset <= gpu_address_offset_mask && "heap byte offset exceeds the 40-bit GPU pointer range");
            return reinterpret_cast<void*>((uint64(heap_id) << gpu_address_offset_bits) | byte_offset);
        }

        struct DecodedRange
        {
            detail::GpuHeapRecord* heap = nullptr;
            uint64 offset = 0;
        };

        [[nodiscard]] DecodedRange decode_gpu_range(const Device& device, GpuRange range) noexcept
        {
            const uint64 address = reinterpret_cast<uint64>(range.gpu);
            detail::GpuHeapRecord* heap = device.find_heap(address);
            const uint64 offset = address & gpu_address_offset_mask;
            assert(offset <= heap->size && range.size <= heap->size - offset && "GPU range extends past its heap");
            return { .heap = heap, .offset = offset };
        }

        void write_linear_heap_descriptors(Device& device, detail::GpuHeapRecord& record) noexcept
        {
            // A raw buffer view addresses 4-byte elements over the whole allocation, matching
            // ByteAddressBuffer in the shader. The view covers the padded resource, not the
            // requested size, so a heap smaller than one element still describes a legal range.
            const uint32 element_count = static_cast<uint32>(align_up(record.size, gpu_allocation_alignment) / 4);

            const D3D12_SHADER_RESOURCE_VIEW_DESC srv{
                .Format = DXGI_FORMAT_R32_TYPELESS,
                .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                .Buffer = { .NumElements = element_count, .Flags = D3D12_BUFFER_SRV_FLAG_RAW },
            };
            device.device->CreateShaderResourceView(record.resource, &srv, device.resource_handle(record.shader_descriptor_index));

            // Only GPU-only heaps can carry a UAV, so storing through a GPU pointer into a
            // cpu_visible or readback heap is a programming error rather than a supported path.
            if (record.memory != MemoryType::gpu_only)
                return;
            const D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
                .Format = DXGI_FORMAT_R32_TYPELESS,
                .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                .Buffer = { .NumElements = element_count, .Flags = D3D12_BUFFER_UAV_FLAG_RAW },
            };
            device.device->CreateUnorderedAccessView(record.resource, nullptr, &uav, device.resource_handle(record.shader_descriptor_index + 1));
        }

    } // namespace

    void Device::reset_retired_command_contexts() noexcept
    {
        detail::CommandContext* context = next_command_context;
        if (!context)
            return;
        detail::CommandContext* current = context;
        do
        {
            if (!current->active && current->retire_value != 0 && current->retire_value <= completed_command_retirement)
            {
                assert_hr(current->allocator->Reset());
                current->retire_value = 0;
            }
            current = current->next;
        } while (current != context);
    }

    Error Device::create_command_context(detail::CommandContext& context) noexcept
    {
        HRESULT result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context.allocator));
        if (FAILED(result))
            return error_from_hresult(result);
        result = device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&context.list));
        if (FAILED(result))
        {
            release(context.allocator);
            return error_from_hresult(result);
        }
        context.commands = new CommandBuffer{
            .state = this,
            .context = &context,
            .list = context.list,
        };
        return Error::none;
    }

    Error Device::grow_command_context_pool() noexcept
    {
        detail::CommandContext* context = new detail::CommandContext;
        const Error error = create_command_context(*context);
        if (error != Error::none)
        {
            delete context;
            return error;
        }
        if (!next_command_context)
        {
            context->next = context;
            context->previous = context;
            next_command_context = context;
        }
        else
        {
            context->next = next_command_context;
            context->previous = next_command_context->previous;
            next_command_context->previous->next = context;
            next_command_context->previous = context;
        }
        ++command_context_count;
        return Error::none;
    }

    detail::CommandContext& Device::acquire_command_context() noexcept
    {
        detail::CommandContext* context = next_command_context;
        assert(context);
        if (active_command_buffers == 0 && context->retire_value > completed_command_retirement)
            poll_command_retirement();
        if (context->active || context->retire_value > completed_command_retirement)
        {
            const Error error = grow_command_context_pool();
            if (error != Error::none)
                abort();
            context = next_command_context->previous;
        }
        if (context->retire_value != 0)
        {
            assert_hr(context->allocator->Reset());
            context->retire_value = 0;
        }
        next_command_context = context->next;
        return *context;
    }

    void Device::destroy_command_contexts() noexcept
    {
        detail::CommandContext* context = next_command_context;
        if (!context)
            return;
        context->previous->next = nullptr;
        while (context)
        {
            detail::CommandContext* next = context->next;
            while (context->texture_copy_scratch)
            {
                detail::TextureCopyScratch* scratch = context->texture_copy_scratch;
                context->texture_copy_scratch = scratch->next;
                release(scratch->resource);
                delete scratch;
            }
            delete context->commands;
            release(context->list);
            release(context->allocator);
            delete context;
            context = next;
        }
        next_command_context = nullptr;
        command_context_count = 0;
    }

    namespace {

        DeviceInit fail_device_creation(Device* state, Error error) noexcept;
        [[nodiscard]] Error recreate_swapchain(Swapchain& swapchain) noexcept;
        void destroy_swapchain(Swapchain& swapchain) noexcept;
        void release_swapchain_views(Swapchain& swapchain) noexcept;
        [[nodiscard]] uint32x2 window_extent(HWND window) noexcept;

        [[nodiscard]] Error create_device_objects(Device& state, const DeviceDesc& desc) noexcept
        {
            // Feature floor for the modern binding model. Older devices are rejected rather than emulated.
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
                options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
            {
                return Error::unsupported;
            }
            D3D12_FEATURE_DATA_SHADER_MODEL shader_model{ .HighestShaderModel = D3D_SHADER_MODEL_6_6 };
            if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) ||
                shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_6)
            {
                return Error::unsupported;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
            if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) || !options12.EnhancedBarriersSupported)
            {
                return Error::unsupported;
            }

            D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{};
            if (FAILED(state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14))) ||
                !options14.IndependentFrontAndBackStencilRefMaskSupported)
                return Error::unsupported;
            D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13{};
            state.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options13, sizeof(options13));
            state.unrestricted_texture_copy_pitch = options13.UnrestrictedBufferTextureCopyPitchSupported != FALSE;

            const D3D12_COMMAND_QUEUE_DESC queue_desc{ .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
            Error error = error_from_hresult(state.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&state.queue)));
            if (error != Error::none)
                return error;

            // One root signature for every PSO: 64 root constants plus directly indexed heaps.
            const D3D12_ROOT_PARAMETER1 root_parameter{
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                .Constants = { .ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = root_constant_count },
                .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
            };
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = {
            .NumParameters = 1,
            .pParameters = &root_parameter,
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED,
        },
    };
            ID3DBlob* root_blob = nullptr;
            ID3DBlob* root_error = nullptr;
            HRESULT result = D3D12SerializeVersionedRootSignature(&root_desc, &root_blob, &root_error);
            release(root_error);
            if (FAILED(result))
                return error_from_hresult(result);
            result = state.device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&state.root_signature));
            release(root_blob);
            if (FAILED(result))
                return error_from_hresult(result);

            const D3D12_DESCRIPTOR_HEAP_DESC resource_desc{
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                .NumDescriptors = resource_descriptor_capacity,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            };
            error = error_from_hresult(state.device->CreateDescriptorHeap(&resource_desc, IID_PPV_ARGS(&state.resource_heap)));
            if (error != Error::none)
                return error;
            const D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{
                .Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                .NumDescriptors = sampler_descriptor_capacity,
                .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            };
            error = error_from_hresult(state.device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&state.sampler_heap)));
            if (error != Error::none)
                return error;
            const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 256 };
            error = error_from_hresult(state.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&state.rtv_heap)));
            if (error != Error::none)
                return error;
            const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{ .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV, .NumDescriptors = 256 };
            error = error_from_hresult(state.device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&state.dsv_heap)));
            if (error != Error::none)
                return error;

            state.resource_descriptor_size = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            state.sampler_descriptor_size = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
            state.rtv_descriptor_size = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            state.dsv_descriptor_size = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

            error = error_from_hresult(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.command_retirement)));
            if (error != Error::none)
                return error;
            state.retirement_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!state.retirement_event)
                return Error::driver_error;

            for (uint32 index = 0; index < initial_command_context_count; ++index)
            {
                error = state.grow_command_context_pool();
                if (error != Error::none)
                    return error;
            }

            for (uint32 index = 0; index < format_count; ++index)
            {
                const FormatMapping mapping = map_format(static_cast<Format>(index));
                state.format_supported[index] = mapping.resource != DXGI_FORMAT_UNKNOWN;
            }
            (void)desc;
            return Error::none;
        }

    } // namespace

    DeviceInit create_device(const DeviceDesc& desc) noexcept
    {
        Device* state = new Device;

#if !defined(NDEBUG)
        ID3D12Debug6* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            release(debug);
        }
#endif

        uint32 factory_flags = 0;
#if !defined(NDEBUG)
        factory_flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        HRESULT result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&state->factory));
        if (FAILED(result))
            return fail_device_creation(state, error_from_hresult(result));

        for (uint32 index = 0;; ++index)
        {
            IDXGIAdapter4* adapter = nullptr;
            if (FAILED(state->factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
                break;
            DXGI_ADAPTER_DESC3 adapter_desc{};
            if (SUCCEEDED(adapter->GetDesc3(&adapter_desc)) && (adapter_desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&state->device))))
            {
                state->adapter = adapter;
                WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, state->device_name, sizeof(state->device_name) - 1, nullptr, nullptr);
                break;
            }
            release(adapter);
        }
        if (!state->device)
            return fail_device_creation(state, Error::unsupported);

#if !defined(NDEBUG)
        state->device->QueryInterface(IID_PPV_ARGS(&state->info_queue));
        active_info_queue = state->info_queue;
        if (state->info_queue)
        {
            // Textures carry no optimized clear value because the API takes clear values per pass,
            // not per resource. The resulting clears are correct, only slower, so drop the advice.
            D3D12_MESSAGE_ID denied[]{
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            };
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<uint32>(sizeof(denied) / sizeof(denied[0]));
            filter.DenyList.pIDList = denied;
            state->info_queue->AddStorageFilterEntries(&filter);
        }
#endif

        const Error error = create_device_objects(*state, desc);
        if (error != Error::none)
            return fail_device_creation(state, error);

        D3D12_FEATURE_DATA_D3D12_OPTIONS4 native16{};
        state->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &native16, sizeof(native16));

        state->caps = {
            .device_name = state->device_name,
            .max_push_data_size = max_push_data_size,
            .texture_heap_alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            .texture_descriptor_size = state->resource_descriptor_size,
            .sampler_descriptor_size = state->sampler_descriptor_size,
            .texture_compression_bc = true,
            .texture_compression_astc = false,
            .storage_input_output16 = native16.Native16BitShaderOpsSupported != FALSE,
        };

        state->window = static_cast<HWND>(desc.window);
        if (state->window)
        {
            assert(desc.swapchain_format != Format::undefined && "a windowed device requires a swapchain format");
            state->swapchain = new Swapchain{ .state = state, .format = desc.swapchain_format };
            const Error swapchain_error = recreate_swapchain(*state->swapchain);
            if (swapchain_error != Error::none)
                return fail_device_creation(state, swapchain_error);
        }
        return { .device = state };
    }

    namespace {

        DeviceInit fail_device_creation(Device* state, Error error) noexcept
        {
            destroy_device(state);
            return { .error = error };
        }

    } // namespace

    void destroy_device(Device* device) noexcept
    {
        if (!device)
            return;
        if (device->queue && device->command_retirement)
            wait_idle(device);
        if (device->swapchain)
        {
            destroy_swapchain(*device->swapchain);
            delete device->swapchain;
            device->swapchain = nullptr;
        }
        drain_debug_messages(device->info_queue);
        if (active_info_queue == device->info_queue)
            active_info_queue = nullptr;
        device->destroy_command_contexts();
        if (device->retirement_event)
            CloseHandle(device->retirement_event);
        for (uint32 index = 0; index < device->indirect_signature_count; ++index)
            release(device->indirect_signatures[index].signature);
        free(device->indirect_signatures);
        free(device->submit_lists);
        free(device->heaps);
        release(device->command_retirement);
        release(device->dsv_heap);
        release(device->rtv_heap);
        release(device->sampler_heap);
        release(device->resource_heap);
        release(device->root_signature);
        release(device->queue);
        release(device->info_queue);
        release(device->device);
        release(device->adapter);
        release(device->factory);
        delete device;
    }

    const DeviceCaps& get_device_caps(const Device* device) noexcept
    {
        assert(device && "get_device_caps called with a null device");
        return device->caps;
    }

    bool supports_texture_format(const Device* device, Format format, TextureUsage usage) noexcept
    {
        assert(device && "supports_texture_format called with a null device");
        if (format == Format::undefined)
            return false;
        const FormatMapping mapping = map_format(format);
        if (mapping.resource == DXGI_FORMAT_UNKNOWN)
            return false;

        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ .Format = mapping.view };
        if (FAILED(device->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
            return false;

        const uint32 usage_bits = static_cast<uint32>(usage);
        if ((usage_bits & static_cast<uint32>(TextureUsage::sampled)) != 0 && (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) == 0)
            return false;
        if ((usage_bits & static_cast<uint32>(TextureUsage::storage)) != 0 && (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0)
            return false;
        if ((usage_bits & static_cast<uint32>(TextureUsage::color_attachment)) != 0 && (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0)
            return false;
        if ((usage_bits & static_cast<uint32>(TextureUsage::depth_stencil_attachment)) != 0)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT depth_support{ .Format = mapping.depth };
            if (mapping.depth == DXGI_FORMAT_UNKNOWN ||
                FAILED(device->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &depth_support, sizeof(depth_support))) ||
                (depth_support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) == 0)
            {
                return false;
            }
        }
        return true;
    }

    TimelineSemaphore* create_timeline_semaphore(Device* device, uint64 initial_value) noexcept
    {
        assert(device && "create_timeline_semaphore called with a null device");
        TimelineSemaphore* result = new TimelineSemaphore{ .state = device };
        require_hr(device->device->CreateFence(initial_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->fence)));
        result->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!result->event)
            abort();
        return result;
    }

    void destroy_timeline_semaphore(TimelineSemaphore* semaphore) noexcept
    {
        if (!semaphore)
            return;
        assert(semaphore->state && semaphore->fence && "destroy_timeline_semaphore received an invalid semaphore");
        if (semaphore->event)
            CloseHandle(semaphore->event);
        release(semaphore->fence);
        delete semaphore;
    }

    uint64 timeline_completed_value(const TimelineSemaphore* semaphore) noexcept
    {
        assert(semaphore && semaphore->fence && "timeline_completed_value received an invalid semaphore");
        return semaphore->fence->GetCompletedValue();
    }

    void wait_timeline(TimelinePoint point) noexcept
    {
        assert(point.semaphore && point.semaphore->fence && "wait_timeline received an invalid timeline point");
        if (point.semaphore->fence->GetCompletedValue() >= point.value)
            return;
        require_hr(point.semaphore->fence->SetEventOnCompletion(point.value, point.semaphore->event));
        WaitForSingleObject(point.semaphore->event, INFINITE);
    }

    void wait_idle(Device* device) noexcept
    {
        assert(device && "wait_idle called with a null device");
        const uint64 value = ++device->command_retirement_value;
        require_hr(device->queue->Signal(device->command_retirement, value));
        device->wait_command_retirement(value);
    }

    // ---------------------------------------------------------------------------
    // Linear and descriptor heaps
    // ---------------------------------------------------------------------------

    namespace {

        [[nodiscard]] uint32 allocate_descriptor_slots(bool* slots, uint32 capacity, uint32 count) noexcept
        {
            uint32 available_count = 0;
            for (uint32 slot = 0; slot < capacity; ++slot)
            {
                available_count = slots[slot] ? 0 : available_count + 1;
                if (available_count != count)
                    continue;
                const uint32 first = slot + 1 - count;
                memset(slots + first, true, count * sizeof(bool));
                return first;
            }
            assert(false && "descriptor heap capacity exceeded");
            return 0;
        }

        [[nodiscard]] GpuHeap allocate_linear_heap(Device& device, uint64 size, MemoryType memory) noexcept
        {
            D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_BARRIER_LAYOUT initial = D3D12_BARRIER_LAYOUT_UNDEFINED;
            switch (memory)
            {
            case MemoryType::cpu_visible: heap_type = D3D12_HEAP_TYPE_UPLOAD; break;
            case MemoryType::gpu_only: heap_type = D3D12_HEAP_TYPE_DEFAULT; break;
            case MemoryType::readback: heap_type = D3D12_HEAP_TYPE_READBACK; break;
            default: assert(false && "create_gpu_heap received an invalid memory type"); return {};
            }
            (void)initial;

            const D3D12_HEAP_PROPERTIES properties{ .Type = heap_type };
            const D3D12_RESOURCE_DESC1 resource_desc{
                .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                .Alignment = 0,
                .Width = align_up(size, gpu_allocation_alignment),
                .Height = 1,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .Format = DXGI_FORMAT_UNKNOWN,
                .SampleDesc = { .Count = 1 },
                .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                .Flags = heap_type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
            };

            detail::GpuHeapRecord* record = new detail::GpuHeapRecord{ .state = &device, .memory = memory, .size = size };
            const HRESULT create_result = device.device->CreateCommittedResource3(
                &properties,
                D3D12_HEAP_FLAG_NONE,
                &resource_desc,
                D3D12_BARRIER_LAYOUT_UNDEFINED,
                nullptr,
                nullptr,
                0,
                nullptr,
                IID_PPV_ARGS(&record->resource)
            );
#if !defined(NDEBUG)
            if (FAILED(create_result))
            {
                drain_debug_messages(device.info_queue);
                fprintf(stderr, "NoGraphicsAPI: device removed reason 0x%08lx\n", static_cast<unsigned long>(device.device->GetDeviceRemovedReason()));
            }
#endif
            require_hr(create_result);
            if (heap_type != D3D12_HEAP_TYPE_DEFAULT)
            {
                const D3D12_RANGE read_range{};
                require_hr(record->resource->Map(0, memory == MemoryType::readback ? nullptr : &read_range, &record->mapped));
            }

            // Every linear heap is reachable from shaders through an adjacent SRV/UAV descriptor pair
            // whose index the shader derives from the heap id.
            record->heap_id = device.register_heap(record);
            record->shader_descriptor_index = internal_descriptor_base + (record->heap_id - 1) * 2;
            assert(record->shader_descriptor_index + 1 < resource_descriptor_capacity && "exhausted the internal descriptor region");
            write_linear_heap_descriptors(device, *record);

            record->owner = { .state = &device, .object = record };
            return {
        .range = {
            .cpu = static_cast<byte*>(record->mapped),
            .gpu = static_cast<byte*>(encode_gpu_ptr(record->heap_id, 0)),
            .size = size,
        },
        .owner = &record->owner,
    };
        }

        [[nodiscard]] GpuHeap allocate_descriptor_heap(Device& device, uint64 size, MemoryType memory) noexcept
        {
            const bool texture_heap = memory == MemoryType::texture_descriptor_heap;
            const uint64 slot_size = texture_heap ? device.caps.texture_descriptor_size : device.caps.sampler_descriptor_size;
            assert(slot_size != 0 && size % slot_size == 0 && "descriptor heap size must be a multiple of the descriptor slot size");
            const uint32 count = static_cast<uint32>(size / slot_size);

            detail::GpuHeapRecord* record = new detail::GpuHeapRecord{ .state = &device, .memory = memory, .size = size };
            // D3D12 descriptors are not mapped memory, so the public cpu pointer addresses a shadow
            // allocation and write_*_descriptor translates a shadow offset into a device heap slot.
            record->shadow = static_cast<byte*>(malloc(size != 0 ? size : 1));
            record->descriptor_count = count;
            record->descriptor_base = allocate_descriptor_slots(
                texture_heap ? device.resource_slots : device.sampler_slots,
                texture_heap ? internal_descriptor_base : sampler_descriptor_capacity,
                count
            );

            record->heap_id = device.register_heap(record);
            record->owner = { .state = &device, .object = record };
            return {
        .range = {
            .cpu = record->shadow,
            .gpu = static_cast<byte*>(encode_gpu_ptr(record->heap_id, 0)),
            .size = size,
        },
        .owner = &record->owner,
    };
        }

        // Descriptor writes address the shadow allocation, so the owning heap is found by pointer range.
        [[nodiscard]] detail::GpuHeapRecord* find_descriptor_heap(const Device& device, const void* cpu_destination, uint64& slot) noexcept
        {
            const byte* target = static_cast<const byte*>(cpu_destination);
            for (uint32 id = 1; id < device.heap_count; ++id)
            {
                detail::GpuHeapRecord* record = device.heaps[id];
                if (!record || !record->shadow)
                    continue;
                if (target < record->shadow || target >= record->shadow + record->size)
                    continue;
                const uint64 offset = static_cast<uint64>(target - record->shadow);
                const uint64 slot_size =
                    record->memory == MemoryType::texture_descriptor_heap ? device.caps.texture_descriptor_size : device.caps.sampler_descriptor_size;
                assert(offset % slot_size == 0 && "descriptor destination is not aligned to a descriptor slot");
                slot = offset / slot_size;
                return record;
            }
            assert(false && "descriptor destination does not belong to a live descriptor heap");
            return nullptr;
        }

    } // namespace

    GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory) noexcept
    {
        assert(device && "create_gpu_heap called with a null device");
        assert(byte_count != 0 && "create_gpu_heap requires a non-zero size");
        if (memory == MemoryType::texture_descriptor_heap || memory == MemoryType::sampler_descriptor_heap)
            return allocate_descriptor_heap(*device, byte_count, memory);
        return allocate_linear_heap(*device, byte_count, memory);
    }

    void destroy_gpu_heap(const GpuHeap& heap) noexcept
    {
        if (!heap.range.gpu)
        {
            assert(!heap.range.cpu && heap.range.size == 0 && !heap.owner && "destroy_gpu_heap received an invalid empty heap");
            return;
        }
        const GpuHeapOwner* owner = heap.owner;
        assert(owner && owner->state && owner->object && "destroy_gpu_heap requires a live heap returned by create_gpu_heap");
        detail::GpuHeapRecord* record = static_cast<detail::GpuHeapRecord*>(owner->object);
        Device* device = record->state;
        assert(&record->owner == owner && "destroy_gpu_heap owner does not match its heap");

        if (record->mapped)
            record->resource->Unmap(0, nullptr);
        release(record->resource);
        free(record->shadow);
        assert(record->heap_id < device->heap_count);
        device->heaps[record->heap_id] = nullptr;
        if (record->heap_id < device->available_heap_id)
            device->available_heap_id = record->heap_id;
        if (record->memory == MemoryType::texture_descriptor_heap)
            memset(device->resource_slots + record->descriptor_base, false, record->descriptor_count * sizeof(bool));
        else if (record->memory == MemoryType::sampler_descriptor_heap)
            memset(device->sampler_slots + record->descriptor_base, false, record->descriptor_count * sizeof(bool));
        record->owner = {};
        delete record;
    }

    // ---------------------------------------------------------------------------
    // Texture heaps, textures and render views
    // ---------------------------------------------------------------------------

    namespace {

        [[nodiscard]] D3D12_RESOURCE_DESC1 make_texture_desc(const TextureDesc& desc, const FormatMapping& formats) noexcept
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
            const uint32 usage = static_cast<uint32>(desc.usage);
            if ((usage & static_cast<uint32>(TextureUsage::storage)) != 0)
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if ((usage & static_cast<uint32>(TextureUsage::color_attachment)) != 0)
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if ((usage & static_cast<uint32>(TextureUsage::depth_stencil_attachment)) != 0)
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                if ((usage & static_cast<uint32>(TextureUsage::sampled)) == 0)
                    flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            }

            D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            uint16 array_size = static_cast<uint16>(desc.layer_count);
            if (desc.type == TextureType::one_d)
                dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            else if (desc.type == TextureType::three_d)
            {
                dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                array_size = static_cast<uint16>(desc.extent.z);
            }
            else if (desc.type == TextureType::cube || desc.type == TextureType::cube_array)
                array_size = static_cast<uint16>(desc.layer_count);

            return {
                .Dimension = dimension,
                .Alignment = 0,
                .Width = desc.extent.x,
                .Height = desc.extent.y,
                .DepthOrArraySize = array_size,
                .MipLevels = static_cast<uint16>(desc.mip_levels),
                .Format = formats.resource,
                .SampleDesc = { .Count = 1 },
                .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
                .Flags = flags,
            };
        }

    } // namespace

    TextureHeap create_texture_heap(Device* device, uint64 byte_count) noexcept
    {
        assert(device && byte_count != 0 && "create_texture_heap requires a device and a non-zero size");
        const D3D12_HEAP_DESC desc{
            .SizeInBytes = align_up(byte_count, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT),
            .Properties = { .Type = D3D12_HEAP_TYPE_DEFAULT },
            .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        };
        TextureHeapOwner* owner = new TextureHeapOwner{ .state = device };
        require_hr(device->device->CreateHeap(&desc, IID_PPV_ARGS(&owner->heap)));
        return { .size = byte_count, .owner = owner };
    }

    void destroy_texture_heap(const TextureHeap& heap) noexcept
    {
        if (!heap.owner)
        {
            assert(heap.size == 0 && "destroy_texture_heap received an invalid empty heap");
            return;
        }
        TextureHeapOwner* owner = const_cast<TextureHeapOwner*>(heap.owner);
        assert(owner->state && owner->heap && "destroy_texture_heap requires a live heap");
        release(owner->heap);
        delete owner;
    }

    SizeAlign get_texture_size_align(Device* device, const TextureDesc& desc) noexcept
    {
        assert(device && "get_texture_size_align called with a null device");
        const FormatMapping formats = map_format(desc.format);
        assert(formats.resource != DXGI_FORMAT_UNKNOWN && "texture format is not supported by the D3D12 backend");
        const D3D12_RESOURCE_DESC1 resource_desc = make_texture_desc(desc, formats);
        const D3D12_RESOURCE_ALLOCATION_INFO info = device->device->GetResourceAllocationInfo2(0, 1, &resource_desc, nullptr);
        return { .size = info.SizeInBytes, .align = info.Alignment };
    }

    Texture* create_texture(Device* device, const TextureDesc& desc, const TextureHeap& heap, uint64 offset) noexcept
    {
        assert(device && heap.owner && heap.owner->heap && "create_texture requires a device and a live texture heap");
        const FormatMapping formats = map_format(desc.format);
        assert(formats.resource != DXGI_FORMAT_UNKNOWN && "texture format is not supported by the D3D12 backend");
        const D3D12_RESOURCE_DESC1 resource_desc = make_texture_desc(desc, formats);

        Texture* texture = new Texture{ .state = device, .desc = desc, .formats = formats, .layout = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON };
        require_hr(device->device->CreatePlacedResource2(
            heap.owner->heap,
            offset,
            &resource_desc,
            D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON,
            nullptr,
            0,
            nullptr,
            IID_PPV_ARGS(&texture->resource)
        ));
        return texture;
    }

    void destroy_texture(Texture* texture) noexcept
    {
        if (!texture)
            return;
        assert(texture->state && texture->resource && "destroy_texture received an invalid texture");
        release(texture->resource);
        delete texture;
    }

    RenderView* create_render_view(Texture* texture, const RenderViewDesc& desc) noexcept
    {
        assert(texture && texture->state && "create_render_view received an invalid texture");
        Device* device = texture->state;
        const bool depth_stencil = (static_cast<uint32>(texture->desc.usage) & static_cast<uint32>(TextureUsage::depth_stencil_attachment)) != 0;

        RenderView* view = new RenderView{ .state = device, .texture = texture, .desc = desc, .depth_stencil = depth_stencil };
        if (depth_stencil)
        {
            view->slot = allocate_descriptor_slots(device->dsv_slots, 256, 1);
            view->handle = device->dsv_heap->GetCPUDescriptorHandleForHeapStart();
            view->handle.ptr += size_t(view->slot) * device->dsv_descriptor_size;
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{ .Format = texture->formats.depth };
            if (texture->desc.type == TextureType::one_d)
            {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsv.Texture1DArray = { .MipSlice = desc.mip_level, .FirstArraySlice = desc.slice, .ArraySize = 1 };
            }
            else
            {
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Texture2DArray = { .MipSlice = desc.mip_level, .FirstArraySlice = desc.slice, .ArraySize = 1 };
            }
            device->device->CreateDepthStencilView(texture->resource, &dsv, view->handle);
        }
        else
        {
            view->slot = allocate_descriptor_slots(device->rtv_slots, 256, 1);
            view->handle = device->rtv_heap->GetCPUDescriptorHandleForHeapStart();
            view->handle.ptr += size_t(view->slot) * device->rtv_descriptor_size;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{ .Format = texture->formats.view };
            if (texture->desc.type == TextureType::one_d)
            {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtv.Texture1DArray = { .MipSlice = desc.mip_level, .FirstArraySlice = desc.slice, .ArraySize = 1 };
            }
            else if (texture->desc.type == TextureType::three_d)
            {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtv.Texture3D = { .MipSlice = desc.mip_level, .FirstWSlice = desc.slice, .WSize = 1 };
            }
            else
            {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray = { .MipSlice = desc.mip_level, .FirstArraySlice = desc.slice, .ArraySize = 1 };
            }
            device->device->CreateRenderTargetView(texture->resource, &rtv, view->handle);
        }
        return view;
    }

    void destroy_render_view(RenderView* render_view) noexcept
    {
        if (!render_view)
            return;
        assert(render_view->state && "destroy_render_view received an invalid view");
        if (render_view->depth_stencil)
            render_view->state->dsv_slots[render_view->slot] = false;
        else
            render_view->state->rtv_slots[render_view->slot] = false;
        delete render_view;
    }

    void write_texture_descriptor(
        Device* device,
        void* cpu_destination,
        const Texture* texture,
        TextureDescriptorType type,
        const TextureDescriptorDesc& desc
    ) noexcept
    {
        assert(device && cpu_destination && texture && "write_texture_descriptor received an invalid argument");
        uint64 slot = 0;
        detail::GpuHeapRecord* record = find_descriptor_heap(*device, cpu_destination, slot);
        assert(record && record->memory == MemoryType::texture_descriptor_heap && "write_texture_descriptor requires a texture descriptor heap");
        assert(slot < record->descriptor_count && "descriptor destination is past the end of its heap");

        DXGI_FORMAT format = desc.format == Format::undefined ? texture->formats.view : map_format(desc.format).view;
        uint32 plane = 0;
        if (desc.aspect == TextureAspect::stencil)
        {
            format = texture->desc.format == Format::d24_unorm_s8_uint ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            plane = 1;
        }
        const uint32 mip_count = desc.mip_count != 0 ? desc.mip_count : texture->desc.mip_levels - desc.base_mip;
        const uint32 layer_count = desc.layer_count != 0 ? desc.layer_count : texture->desc.layer_count - desc.base_layer;
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = device->resource_handle(record->descriptor_base + static_cast<uint32>(slot));

        if (type == TextureDescriptorType::sampled)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{ .Format = format, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
            if (plane == 1)
                srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                    1,
                    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                    D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
                );
            switch (texture->desc.type)
            {
            case TextureType::one_d:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srv.Texture1D = { .MostDetailedMip = desc.base_mip, .MipLevels = mip_count };
                break;
            case TextureType::two_d:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D = { .MostDetailedMip = desc.base_mip, .MipLevels = mip_count, .PlaneSlice = plane };
                break;
            case TextureType::three_d:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srv.Texture3D = { .MostDetailedMip = desc.base_mip, .MipLevels = mip_count };
                break;
            case TextureType::cube:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv.TextureCube = { .MostDetailedMip = desc.base_mip, .MipLevels = mip_count };
                break;
            case TextureType::cube_array:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srv.TextureCubeArray = { .MostDetailedMip = desc.base_mip,
                                         .MipLevels = mip_count,
                                         .First2DArrayFace = desc.base_layer,
                                         .NumCubes = layer_count / 6 };
                break;
            case TextureType::two_d_array:
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv.Texture2DArray = { .MostDetailedMip = desc.base_mip,
                                       .MipLevels = mip_count,
                                       .FirstArraySlice = desc.base_layer,
                                       .ArraySize = layer_count,
                                       .PlaneSlice = plane };
                break;
            }
            device->device->CreateShaderResourceView(texture->resource, &srv, handle);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{ .Format = format };
            switch (texture->desc.type)
            {
            case TextureType::one_d:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uav.Texture1D = { .MipSlice = desc.base_mip };
                break;
            case TextureType::two_d:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D = { .MipSlice = desc.base_mip, .PlaneSlice = plane };
                break;
            case TextureType::three_d:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uav.Texture3D = { .MipSlice = desc.base_mip, .WSize = mip_extent(texture->desc.extent.z, desc.base_mip) };
                break;
            case TextureType::cube:
            case TextureType::cube_array:
            case TextureType::two_d_array:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav.Texture2DArray = { .MipSlice = desc.base_mip, .FirstArraySlice = desc.base_layer, .ArraySize = layer_count, .PlaneSlice = plane };
                break;
            }
            device->device->CreateUnorderedAccessView(texture->resource, nullptr, &uav, handle);
        }
    }

    void write_sampler_descriptor(Device* device, void* cpu_destination, const SamplerDesc& desc) noexcept
    {
        assert(device && cpu_destination && "write_sampler_descriptor received an invalid argument");
        uint64 slot = 0;
        detail::GpuHeapRecord* record = find_descriptor_heap(*device, cpu_destination, slot);
        assert(record && record->memory == MemoryType::sampler_descriptor_heap && "write_sampler_descriptor requires a sampler descriptor heap");
        assert(slot < record->descriptor_count && "descriptor destination is past the end of its heap");

        const D3D12_SAMPLER_DESC sampler{
            .Filter = map_filter(desc),
            .AddressU = map_address_mode(desc.address_u),
            .AddressV = map_address_mode(desc.address_v),
            .AddressW = map_address_mode(desc.address_w),
            .MipLODBias = 0.0f,
            .MaxAnisotropy = desc.anisotropic ? 4u : 1u,
            .ComparisonFunc = desc.compare_enabled ? map_compare(desc.compare) : D3D12_COMPARISON_FUNC_NEVER,
            .MinLOD = 0.0f,
            .MaxLOD = D3D12_FLOAT32_MAX,
        };
        device->device->CreateSampler(&sampler, device->sampler_handle(record->descriptor_base + static_cast<uint32>(slot)));
    }

    // ---------------------------------------------------------------------------
    // Swapchain
    // ---------------------------------------------------------------------------

    namespace {

        uint32x2 window_extent(HWND window) noexcept
        {
            RECT rect{};
            if (!window || !GetClientRect(window, &rect))
                return {};
            return { .x = static_cast<uint32>(rect.right - rect.left), .y = static_cast<uint32>(rect.bottom - rect.top) };
        }

        void release_swapchain_views(Swapchain& swapchain) noexcept
        {
            for (uint32 index = 0; index < swapchain.image_count; ++index)
            {
                if (swapchain.views[index])
                {
                    destroy_render_view(swapchain.views[index]);
                    swapchain.views[index] = nullptr;
                }
                if (swapchain.textures[index])
                {
                    release(swapchain.textures[index]->resource);
                    delete swapchain.textures[index];
                    swapchain.textures[index] = nullptr;
                }
            }
        }

        [[nodiscard]] Error create_swapchain_views(Swapchain& swapchain) noexcept
        {
            Device& device = *swapchain.state;
            const FormatMapping formats = map_format(swapchain.format);
            for (uint32 index = 0; index < swapchain.image_count; ++index)
            {
                ID3D12Resource* buffer = nullptr;
                const HRESULT result = swapchain.swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer));
                if (FAILED(result))
                    return error_from_hresult(result);

                Texture* texture = new Texture{
                    .state = &device,
                    .resource = buffer,
                    .desc = { .extent = { .x = swapchain.extent.x, .y = swapchain.extent.y, .z = 1 },
                              .format = swapchain.format,
                              .usage = TextureUsage::color_attachment },
                    .formats = formats,
                    .layout = D3D12_BARRIER_LAYOUT_PRESENT,
                };
                swapchain.textures[index] = texture;

                RenderView* view = new RenderView{ .state = &device, .texture = texture };
                view->slot = allocate_descriptor_slots(device.rtv_slots, 256, 1);
                view->handle = device.rtv_heap->GetCPUDescriptorHandleForHeapStart();
                view->handle.ptr += size_t(view->slot) * device.rtv_descriptor_size;
                const D3D12_RENDER_TARGET_VIEW_DESC rtv{ .Format = formats.view, .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D };
                device.device->CreateRenderTargetView(buffer, &rtv, view->handle);
                swapchain.views[index] = view;
            }
            return Error::none;
        }

        Error recreate_swapchain(Swapchain& swapchain) noexcept
        {
            Device& device = *swapchain.state;
            const uint32x2 extent = window_extent(device.window);
            if (extent.x == 0 || extent.y == 0)
            {
                swapchain.extent = {};
                return Error::none;
            }

            const DXGI_FORMAT buffer_format = map_swapchain_format(swapchain.format);
            if (buffer_format == DXGI_FORMAT_UNKNOWN)
                return Error::unsupported;

            if (!swapchain.swapchain)
            {
                const DXGI_SWAP_CHAIN_DESC1 desc{
                    .Width = extent.x,
                    .Height = extent.y,
                    .Format = buffer_format,
                    .SampleDesc = { .Count = 1 },
                    .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                    .BufferCount = 2,
                    .Scaling = DXGI_SCALING_NONE,
                    .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
                    .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
                };
                IDXGISwapChain1* created = nullptr;
                HRESULT result = device.factory->CreateSwapChainForHwnd(device.queue, device.window, &desc, nullptr, nullptr, &created);
                if (FAILED(result))
                    return error_from_hresult(result);
                result = created->QueryInterface(IID_PPV_ARGS(&swapchain.swapchain));
                release(created);
                if (FAILED(result))
                    return error_from_hresult(result);
                device.factory->MakeWindowAssociation(device.window, DXGI_MWA_NO_ALT_ENTER);
                swapchain.image_count = desc.BufferCount;
            }
            else
            {
                release_swapchain_views(swapchain);
                const HRESULT result = swapchain.swapchain->ResizeBuffers(swapchain.image_count, extent.x, extent.y, buffer_format, 0);
                if (FAILED(result))
                    return error_from_hresult(result);
            }

            swapchain.extent = extent;
            swapchain.image_index = swapchain.swapchain->GetCurrentBackBufferIndex();
            return create_swapchain_views(swapchain);
        }

        void destroy_swapchain(Swapchain& swapchain) noexcept
        {
            release_swapchain_views(swapchain);
            release(swapchain.swapchain);
        }

    } // namespace

    uint32x2 get_drawable_extent(Device* device) noexcept
    {
        assert(device && "get_drawable_extent called with a null device");
        return window_extent(device->window);
    }

    SwapchainFrame acquire(Device* device) noexcept
    {
        assert(device && device->swapchain && "acquire requires a windowed device");
        Swapchain& swapchain = *device->swapchain;
        const uint32x2 extent = window_extent(device->window);
        if (extent.x == 0 || extent.y == 0)
            return {};
        if (extent.x != swapchain.extent.x || extent.y != swapchain.extent.y)
        {
            wait_idle(device);
            if (recreate_swapchain(swapchain) != Error::none || swapchain.extent.x == 0)
                return {};
        }
        swapchain.image_index = swapchain.swapchain->GetCurrentBackBufferIndex();
        device->acquired_swapchain = &swapchain;
        return { .render_view = swapchain.views[swapchain.image_index], .extent = swapchain.extent };
    }

    // ---------------------------------------------------------------------------
    // Pipeline state
    // ---------------------------------------------------------------------------

    namespace {

        [[nodiscard]] D3D12_BLEND map_blend_factor(BlendFactor factor) noexcept
        {
            switch (factor)
            {
            case BlendFactor::zero: return D3D12_BLEND_ZERO;
            case BlendFactor::one: return D3D12_BLEND_ONE;
            case BlendFactor::source_color: return D3D12_BLEND_SRC_COLOR;
            case BlendFactor::one_minus_source_color: return D3D12_BLEND_INV_SRC_COLOR;
            case BlendFactor::destination_color: return D3D12_BLEND_DEST_COLOR;
            case BlendFactor::one_minus_destination_color: return D3D12_BLEND_INV_DEST_COLOR;
            case BlendFactor::source_alpha: return D3D12_BLEND_SRC_ALPHA;
            case BlendFactor::one_minus_source_alpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case BlendFactor::destination_alpha: return D3D12_BLEND_DEST_ALPHA;
            case BlendFactor::one_minus_destination_alpha: return D3D12_BLEND_INV_DEST_ALPHA;
            case BlendFactor::source_alpha_saturate: return D3D12_BLEND_SRC_ALPHA_SAT;
            }
            return D3D12_BLEND_ONE;
        }

        [[nodiscard]] D3D12_BLEND_OP map_blend_op(BlendOp operation) noexcept
        {
            switch (operation)
            {
            case BlendOp::add: return D3D12_BLEND_OP_ADD;
            case BlendOp::subtract: return D3D12_BLEND_OP_SUBTRACT;
            case BlendOp::reverse_subtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            case BlendOp::minimum: return D3D12_BLEND_OP_MIN;
            case BlendOp::maximum: return D3D12_BLEND_OP_MAX;
            }
            return D3D12_BLEND_OP_ADD;
        }

        [[nodiscard]] D3D12_STENCIL_OP map_stencil_op(StencilOp operation) noexcept
        {
            switch (operation)
            {
            case StencilOp::keep: return D3D12_STENCIL_OP_KEEP;
            case StencilOp::zero: return D3D12_STENCIL_OP_ZERO;
            case StencilOp::replace: return D3D12_STENCIL_OP_REPLACE;
            case StencilOp::increment_clamp: return D3D12_STENCIL_OP_INCR_SAT;
            case StencilOp::decrement_clamp: return D3D12_STENCIL_OP_DECR_SAT;
            case StencilOp::invert: return D3D12_STENCIL_OP_INVERT;
            case StencilOp::increment_wrap: return D3D12_STENCIL_OP_INCR;
            case StencilOp::decrement_wrap: return D3D12_STENCIL_OP_DECR;
            }
            return D3D12_STENCIL_OP_KEEP;
        }

        [[nodiscard]] D3D12_DEPTH_STENCILOP_DESC map_stencil_face(const StencilFaceState& face) noexcept
        {
            return {
                .StencilFailOp = map_stencil_op(face.fail),
                .StencilDepthFailOp = map_stencil_op(face.depth_fail),
                .StencilPassOp = map_stencil_op(face.pass),
                .StencilFunc = map_compare(face.compare),
            };
        }

        [[nodiscard]] D3D12_RASTERIZER_DESC map_rasterization(const RasterizationState& state) noexcept
        {
            return {
                .FillMode = D3D12_FILL_MODE_SOLID,
                .CullMode = state.cull == CullMode::none ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK,
                // Clockwise culling is expressed by which winding counts as front facing.
                .FrontCounterClockwise = state.cull == CullMode::clockwise ? TRUE : FALSE,
                .DepthBias = static_cast<int32>(state.depth_bias_constant),
                .DepthBiasClamp = state.depth_bias_clamp,
                .SlopeScaledDepthBias = state.depth_bias_slope,
                .DepthClipEnable = TRUE,
            };
        }

        [[nodiscard]] D3D12_BLEND_DESC map_blend(const ColorTargetDesc* targets, uint32 count) noexcept
        {
            D3D12_BLEND_DESC blend{ .IndependentBlendEnable = TRUE };
            for (uint32 index = 0; index < count; ++index)
            {
                blend.RenderTarget[index] = {
                    .BlendEnable = targets[index].blend.enabled ? TRUE : FALSE,
                    .LogicOpEnable = FALSE,
                    .SrcBlend = map_blend_factor(targets[index].blend.color.source),
                    .DestBlend = map_blend_factor(targets[index].blend.color.destination),
                    .BlendOp = map_blend_op(targets[index].blend.color.operation),
                    .SrcBlendAlpha = map_blend_factor(targets[index].blend.alpha.source),
                    .DestBlendAlpha = map_blend_factor(targets[index].blend.alpha.destination),
                    .BlendOpAlpha = map_blend_op(targets[index].blend.alpha.operation),
                    .LogicOp = D3D12_LOGIC_OP_NOOP,
                    .RenderTargetWriteMask = targets[index].write_mask,
                };
            }
            return blend;
        }

        [[nodiscard]] D3D12_DEPTH_STENCIL_DESC1 map_depth_stencil(const DepthStencilState& state) noexcept
        {
            return {
                .DepthEnable = state.depth_test ? TRUE : FALSE,
                .DepthWriteMask = state.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO,
                .DepthFunc = map_compare(state.depth_compare),
                .StencilEnable = state.stencil_test ? TRUE : FALSE,
                .StencilReadMask = state.stencil_read_mask,
                .StencilWriteMask = state.stencil_write_mask,
                .FrontFace = map_stencil_face(state.front),
                .BackFace = map_stencil_face(state.back),
                .DepthBoundsTestEnable = FALSE,
            };
        }

        [[nodiscard]] byte* copy_shader_code(Span<const uint32> code, size_t& size) noexcept
        {
            size = code.size * sizeof(uint32);
            if (size == 0)
                return nullptr;
            byte* copy = static_cast<byte*>(malloc(size));
            memcpy(copy, code.data, size);
            return copy;
        }

// Pipeline state stream subobjects must be individually aligned to a pointer boundary. The
// trailing padding that alignment adds is part of the format D3D12 expects to walk.
#pragma warning(push)
#pragma warning(disable : 4324)
        template<typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type> struct alignas(void*) StreamSubobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
            T value{};
        };

        struct GraphicsPipelineStream
        {
            StreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
            StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vertex;
            StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> mesh;
            StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> fragment;
            StreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
            StreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
            StreamSubobject<D3D12_DEPTH_STENCIL_DESC1, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1> depth_stencil;
            StreamSubobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> depth_format;
            StreamSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> render_targets;
            StreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
            StreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
        };
#pragma warning(pop)

        [[nodiscard]] ID3D12PipelineState* create_pipeline_variant(const PSO& pso, const DepthStencilState& state) noexcept
        {
            GraphicsPipelineStream stream{};
            stream.root_signature.value = pso.state->root_signature;
            if (pso.mesh)
                stream.mesh.value = { .pShaderBytecode = pso.first_stage, .BytecodeLength = pso.first_stage_size };
            else
                stream.vertex.value = { .pShaderBytecode = pso.first_stage, .BytecodeLength = pso.first_stage_size };
            stream.fragment.value = { .pShaderBytecode = pso.fragment_stage, .BytecodeLength = pso.fragment_stage_size };
            stream.blend.value = map_blend(pso.color_targets, pso.color_target_count);
            stream.rasterizer.value = map_rasterization(pso.rasterization);
            stream.depth_stencil.value = map_depth_stencil(state);

            const Format depth_stencil_format = pso.depth_format != Format::undefined ? pso.depth_format : pso.stencil_format;
            stream.depth_format.value = depth_stencil_format != Format::undefined ? map_format(depth_stencil_format).depth : DXGI_FORMAT_UNKNOWN;
            if (stream.depth_format.value == DXGI_FORMAT_UNKNOWN)
            {
                stream.depth_stencil.value.DepthEnable = FALSE;
                stream.depth_stencil.value.StencilEnable = FALSE;
            }

            stream.render_targets.value.NumRenderTargets = pso.color_target_count;
            for (uint32 index = 0; index < pso.color_target_count; ++index)
                stream.render_targets.value.RTFormats[index] = map_format(pso.color_targets[index].format).view;
            stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            stream.sample.value = { .Count = 1 };

            // A mesh pipeline must not declare a vertex stage, and vice versa.
            const size_t stream_size = sizeof(stream);
            D3D12_PIPELINE_STATE_STREAM_DESC desc{ .SizeInBytes = stream_size, .pPipelineStateSubobjectStream = &stream };
            ID3D12PipelineState* pipeline = nullptr;
            require_hr(pso.state->device->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
            return pipeline;
        }

        [[nodiscard]] PSO* create_graphics_like_pso(
            Device* device,
            Span<const uint32> first_stage,
            Span<const uint32> fragment_stage,
            Span<const ColorTargetDesc> color_targets,
            Format depth_format,
            Format stencil_format,
            const RasterizationState& rasterization,
            bool mesh
        ) noexcept
        {
            assert(color_targets.size <= max_color_attachments && "too many color targets");
            PSO* pso = new PSO{ .state = device, .mesh = mesh };
            pso->first_stage = copy_shader_code(first_stage, pso->first_stage_size);
            pso->fragment_stage = copy_shader_code(fragment_stage, pso->fragment_stage_size);
            pso->color_target_count = static_cast<uint32>(color_targets.size);
            for (size_t index = 0; index < color_targets.size; ++index)
                pso->color_targets[index] = color_targets.data[index];
            pso->depth_format = depth_format;
            pso->stencil_format = stencil_format;
            pso->rasterization = rasterization;

            // begin_render_pass starts every pass with depth and stencil disabled.
            pso->pipeline = create_pipeline_variant(*pso, DepthStencilState{});
            return pso;
        }

    } // namespace

    PSO* create_graphics_pso(Device* device, const GraphicsPSODesc& desc) noexcept
    {
        assert(device && "create_graphics_pso called with a null device");
        return create_graphics_like_pso(
            device,
            desc.vertex_spirv,
            desc.fragment_spirv,
            desc.color_targets,
            desc.depth_format,
            desc.stencil_format,
            desc.rasterization,
            false
        );
    }

    PSO* create_mesh_pso(Device* device, const MeshPSODesc& desc) noexcept
    {
        assert(device && "create_mesh_pso called with a null device");
        return create_graphics_like_pso(
            device,
            desc.mesh_spirv,
            desc.fragment_spirv,
            desc.color_targets,
            desc.depth_format,
            desc.stencil_format,
            desc.rasterization,
            true
        );
    }

    PSO* create_compute_pso(Device* device, Span<const uint32> compute_spirv) noexcept
    {
        assert(device && "create_compute_pso called with a null device");
        PSO* pso = new PSO{ .state = device, .compute = true };
        pso->first_stage = copy_shader_code(compute_spirv, pso->first_stage_size);
        const D3D12_COMPUTE_PIPELINE_STATE_DESC desc{
            .pRootSignature = device->root_signature,
            .CS = { .pShaderBytecode = pso->first_stage, .BytecodeLength = pso->first_stage_size },
        };
        require_hr(device->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso->pipeline)));
        return pso;
    }

    void destroy_pso(PSO* pso) noexcept
    {
        if (!pso)
            return;
        for (uint32 index = 0; index < pso->variant_count; ++index)
            release(pso->variants[index].pipeline);
        free(pso->variants);
        free(pso->first_stage);
        free(pso->fragment_stage);
        release(pso->pipeline);
        delete pso;
    }

    // ---------------------------------------------------------------------------
    // Command recording
    // ---------------------------------------------------------------------------

    namespace {

        void transition_texture(
            CommandBuffer& commands,
            Texture& texture,
            D3D12_BARRIER_LAYOUT layout,
            D3D12_BARRIER_SYNC sync,
            D3D12_BARRIER_ACCESS access
        ) noexcept
        {
            if (texture.layout == layout)
                return;
            assert(commands.list && "texture transitions require a recording command buffer");
            const D3D12_TEXTURE_BARRIER barrier{
                .SyncBefore = D3D12_BARRIER_SYNC_ALL,
                .SyncAfter = sync,
                .AccessBefore = D3D12_BARRIER_ACCESS_COMMON,
                .AccessAfter = access,
                .LayoutBefore = texture.layout,
                .LayoutAfter = layout,
                .pResource = texture.resource,
                .Subresources = { .IndexOrFirstMipLevel = 0xffffffffu },
            };
            const D3D12_BARRIER_GROUP group{ .Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &barrier };
            commands.list->Barrier(1, &group);
            texture.layout = layout;
        }

        // The API sets depth-stencil state dynamically, so the matching pipeline variant is chosen at
        // draw time rather than when the PSO is bound.
        void resolve_graphics_pipeline(CommandBuffer& commands) noexcept
        {
            const PSO* pso = commands.bound_pso;
            assert(pso && !pso->compute && "a graphics PSO must be bound before drawing");

            // DepthStencilState is a packed byte aggregate, so variants compare bitwise.
            static_assert(sizeof(DepthStencilState) == 16, "DepthStencilState must stay padding free for bitwise variant lookup");
            static constexpr DepthStencilState default_state{};

            ID3D12PipelineState* pipeline = pso->pipeline;
            if (memcmp(&commands.depth_stencil, &default_state, sizeof(DepthStencilState)) != 0)
            {
                PSO* mutable_pso = const_cast<PSO*>(pso);
                pipeline = nullptr;
                for (uint32 index = 0; index < pso->variant_count; ++index)
                {
                    if (memcmp(&pso->variants[index].state, &commands.depth_stencil, sizeof(DepthStencilState)) == 0)
                    {
                        pipeline = pso->variants[index].pipeline;
                        break;
                    }
                }
                if (!pipeline)
                {
                    if (mutable_pso->variant_count == mutable_pso->variant_capacity)
                    {
                        mutable_pso->variant_capacity = mutable_pso->variant_capacity == 0 ? 4u : mutable_pso->variant_capacity * 2u;
                        mutable_pso->variants = static_cast<PSOVariant*>(realloc(mutable_pso->variants, mutable_pso->variant_capacity * sizeof(PSOVariant)));
                    }
                    pipeline = create_pipeline_variant(*pso, commands.depth_stencil);
                    mutable_pso->variants[mutable_pso->variant_count++] = { .state = commands.depth_stencil, .pipeline = pipeline };
                }
            }

            if (commands.bound_pipeline != pipeline)
            {
                commands.list->SetPipelineState(pipeline);
                commands.bound_pipeline = pipeline;
            }
        }

        void emit_root_data(CommandBuffer& commands, ByteSpan root) noexcept
        {
            if (root.size == 0)
                return;
            assert(root.data && "root data span has a size but no data");
            assert(root.size % 4 == 0 && "root data size must be a multiple of four bytes");
            assert(root.size <= max_push_data_size && "root data exceeds DeviceCaps::max_push_data_size");
            const uint32 word_count = static_cast<uint32>(root.size / 4);
            if (commands.bound_pso && commands.bound_pso->compute)
                commands.list->SetComputeRoot32BitConstants(0, word_count, root.data, 0);
            else
                commands.list->SetGraphicsRoot32BitConstants(0, word_count, root.data, 0);
        }

    } // namespace

    CommandBuffer* begin_commands(Device* device) noexcept
    {
        assert(device && "begin_commands called with a null device");
        detail::CommandContext& context = device->acquire_command_context();
        CommandBuffer& commands = *context.commands;

        assert_hr(context.list->Reset(context.allocator, nullptr));
        context.active = true;
        commands.recording = true;
        commands.rendering = false;
        commands.bound_pso = nullptr;
        commands.bound_pipeline = nullptr;
        commands.swapchain = nullptr;
        commands.color_view_count = 0;
        commands.depth_view = nullptr;

        // Both shader-visible heaps stay bound for the whole list; SM 6.6 indexes them directly.
        ID3D12DescriptorHeap* heaps[2]{ device->resource_heap, device->sampler_heap };
        context.list->SetDescriptorHeaps(2, heaps);
        context.list->SetGraphicsRootSignature(device->root_signature);
        context.list->SetComputeRootSignature(device->root_signature);

        ++device->active_command_buffers;
        return &commands;
    }

    void set_texture_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept
    {
        assert(commands && commands->recording && "set_texture_descriptor_heap requires a recording command buffer");
        const detail::GpuHeapRecord* record = commands->state->find_heap(reinterpret_cast<uint64>(heap.gpu));
        assert(record->memory == MemoryType::texture_descriptor_heap && "set_texture_descriptor_heap requires a texture descriptor heap");
        assert(record->size == heap.size && "set_texture_descriptor_heap requires the full GpuHeap range");
        // The device owns one shader-visible resource heap; application heaps are slot ranges in it.
        assert(record->descriptor_base == 0 && "the D3D12 backend supports one texture descriptor heap per device");
    }

    void set_sampler_descriptor_heap(CommandBuffer* commands, GpuRange heap) noexcept
    {
        assert(commands && commands->recording && "set_sampler_descriptor_heap requires a recording command buffer");
        const detail::GpuHeapRecord* record = commands->state->find_heap(reinterpret_cast<uint64>(heap.gpu));
        assert(record->memory == MemoryType::sampler_descriptor_heap && "set_sampler_descriptor_heap requires a sampler descriptor heap");
        assert(record->size == heap.size && "set_sampler_descriptor_heap requires the full GpuHeap range");
        assert(record->descriptor_base == 0 && "the D3D12 backend supports one sampler descriptor heap per device");
    }

    void copy_memory(CommandBuffer* commands, GpuRange source, GpuRange destination) noexcept
    {
        assert(commands && commands->recording && "copy_memory requires a recording command buffer");
        assert(source.size <= destination.size && "copy_memory destination is smaller than the source");
        const DecodedRange from = decode_gpu_range(*commands->state, source);
        const DecodedRange to = decode_gpu_range(*commands->state, destination);
        commands->list->CopyBufferRegion(to.heap->resource, to.offset, from.heap->resource, from.offset, source.size);
    }

    namespace {

        [[nodiscard]] ID3D12Resource* acquire_texture_copy_scratch(CommandBuffer& commands, uint64 size) noexcept
        {
            detail::TextureCopyScratch* scratch = commands.context->texture_copy_scratch;
            if (scratch && scratch->size >= size)
                return scratch->resource;
            uint64 capacity = scratch ? scratch->size * 2 : 65536;
            while (capacity < size)
                capacity *= 2;
            scratch = new detail::TextureCopyScratch{ .next = commands.context->texture_copy_scratch, .size = capacity };
            const D3D12_HEAP_PROPERTIES properties{ .Type = D3D12_HEAP_TYPE_DEFAULT };
            const D3D12_RESOURCE_DESC1 desc{
                .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                .Width = capacity,
                .Height = 1,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .SampleDesc = { .Count = 1 },
                .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            };
            require_hr(commands.state->device->CreateCommittedResource3(
                &properties,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_BARRIER_LAYOUT_UNDEFINED,
                nullptr,
                nullptr,
                0,
                nullptr,
                IID_PPV_ARGS(&scratch->resource)
            ));
            // Previously recorded copies retain their scratch resources until the command context is destroyed.
            commands.context->texture_copy_scratch = scratch;
            return scratch->resource;
        }

        void texture_copy_scratch_barrier(CommandBuffer& commands, ID3D12Resource* resource, bool prepare_write) noexcept
        {
            const D3D12_BUFFER_BARRIER barrier{
                .SyncBefore = D3D12_BARRIER_SYNC_COPY,
                .SyncAfter = D3D12_BARRIER_SYNC_COPY,
                .AccessBefore = prepare_write ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_COPY_DEST,
                .AccessAfter = prepare_write ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COPY_SOURCE,
                .pResource = resource,
                .Size = ~uint64{ 0 },
            };
            const D3D12_BARRIER_GROUP group{ .Type = D3D12_BARRIER_TYPE_BUFFER, .NumBarriers = 1, .pBufferBarriers = &barrier };
            commands.list->Barrier(1, &group);
        }

        void copy_texture_memory(CommandBuffer& commands, Texture& texture, GpuRange memory, const TextureCopyDesc& copy, bool upload) noexcept
        {
            const DecodedRange range = decode_gpu_range(*commands.state, memory);
            const TextureFormatInfo format = get_texture_format_info(texture.desc.format);
            const uint32 width = copy.extent.x != 0 ? copy.extent.x : mip_extent(texture.desc.extent.x, copy.mip_level) - copy.offset.x;
            const uint32 height = copy.extent.y != 0 ? copy.extent.y : mip_extent(texture.desc.extent.y, copy.mip_level) - copy.offset.y;
            const uint32 depth = copy.extent.z != 0 ? copy.extent.z : mip_extent(texture.desc.extent.z, copy.mip_level) - copy.offset.z;
            const uint32 layers = copy.slice_count != 0 ? copy.slice_count : texture.desc.layer_count - copy.base_slice;
            const uint32 rows = (height + format.block_extent.y - 1) / format.block_extent.y;
            const uint64 row_bytes = uint64((width + format.block_extent.x - 1) / format.block_extent.x) * format.bytes_per_block;
            const uint64 row_pitch = copy.row_pitch_bytes != 0 ? copy.row_pitch_bytes : row_bytes;
            const uint64 slice_pitch = copy.slice_pitch_bytes != 0 ? copy.slice_pitch_bytes : row_pitch * rows;
            assert(width && height && depth && layers && "texture copy must have a nonempty region");
            assert(row_pitch >= row_bytes && slice_pitch >= row_pitch * (rows - 1) + row_bytes);
            assert(
                memory.size >= (uint64(layers) * depth - 1) * slice_pitch + uint64(rows - 1) * row_pitch + row_bytes && "texture copy exceeds its memory range"
            );

            const bool direct = commands.state->unrestricted_texture_copy_pitch ||
                                (range.offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 && row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0 &&
                                 (uint64(layers) * depth == 1 || slice_pitch % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0));
            const uint64 footprint_row_pitch = direct ? row_pitch : align_up(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            ID3D12Resource* buffer = direct ? range.heap->resource : acquire_texture_copy_scratch(commands, footprint_row_pitch * rows);
            const D3D12_RESOURCE_DESC resource_desc = texture.resource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            commands.state->device->GetCopyableFootprints(&resource_desc, copy.mip_level, 1, 0, &footprint, nullptr, nullptr, nullptr);
            footprint.Footprint.Width = static_cast<uint32>(align_up(width, format.block_extent.x));
            footprint.Footprint.Height = static_cast<uint32>(align_up(height, format.block_extent.y));
            footprint.Footprint.Depth = 1;
            footprint.Footprint.RowPitch = static_cast<uint32>(footprint_row_pitch);

            for (uint32 layer = 0; layer < layers; ++layer)
            {
                const D3D12_TEXTURE_COPY_LOCATION texture_location{
                    .pResource = texture.resource,
                    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    .SubresourceIndex = copy.mip_level + (copy.base_slice + layer) * texture.desc.mip_levels,
                };
                for (uint32 slice = 0; slice < depth; ++slice)
                {
                    const uint64 memory_offset = range.offset + (uint64(layer) * depth + slice) * slice_pitch;
                    footprint.Offset = direct ? memory_offset : 0;
                    const D3D12_TEXTURE_COPY_LOCATION buffer_location{
                        .pResource = buffer,
                        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                        .PlacedFootprint = footprint,
                    };
                    if (!direct)
                    {
                        texture_copy_scratch_barrier(commands, buffer, true);
                        if (upload)
                        {
                            for (uint32 row = 0; row < rows; ++row)
                                commands.list
                                    ->CopyBufferRegion(buffer, row * footprint_row_pitch, range.heap->resource, memory_offset + row * row_pitch, row_bytes);
                            texture_copy_scratch_barrier(commands, buffer, false);
                        }
                    }
                    if (upload)
                    {
                        const D3D12_BOX region{ .right = width, .bottom = height, .back = 1 };
                        commands.list->CopyTextureRegion(
                            &texture_location,
                            copy.offset.x,
                            copy.offset.y,
                            copy.offset.z + slice,
                            &buffer_location,
                            format.depth ? nullptr : &region
                        );
                    }
                    else
                    {
                        const D3D12_BOX region{ .left = copy.offset.x,
                                                .top = copy.offset.y,
                                                .front = copy.offset.z + slice,
                                                .right = copy.offset.x + width,
                                                .bottom = copy.offset.y + height,
                                                .back = copy.offset.z + slice + 1 };
                        commands.list->CopyTextureRegion(&buffer_location, 0, 0, 0, &texture_location, format.depth ? nullptr : &region);
                        if (!direct)
                        {
                            texture_copy_scratch_barrier(commands, buffer, false);
                            for (uint32 row = 0; row < rows; ++row)
                                commands.list
                                    ->CopyBufferRegion(range.heap->resource, memory_offset + row * row_pitch, buffer, row * footprint_row_pitch, row_bytes);
                        }
                    }
                }
            }
        }

    } // namespace

    void copy_memory_to_texture(CommandBuffer* commands, GpuRange source, Texture* destination, const TextureCopyDesc& copy) noexcept
    {
        assert(commands && commands->recording && destination && "copy_memory_to_texture received an invalid argument");
        copy_texture_memory(*commands, *destination, source, copy, true);
    }

    void copy_texture_to_memory(CommandBuffer* commands, Texture* source, GpuRange destination, const TextureCopyDesc& copy) noexcept
    {
        assert(commands && commands->recording && source && "copy_texture_to_memory received an invalid argument");
        copy_texture_memory(*commands, *source, destination, copy, false);
    }

    void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept
    {
        assert(commands && commands->recording && "barrier requires a recording command buffer");
        // NoGraphicsAPI exposes no resource lists, so every explicit barrier is a global barrier.
        const D3D12_BARRIER_SYNC sync_before = map_sync(before);
        const D3D12_BARRIER_SYNC sync_after = map_sync(after);

        // A stage scope of none carries no access, which D3D12 spells as no access rather than common.
        // Attachment accesses cannot appear in a global barrier at all, and common access has to apply
        // to both sides that carry one. Such barriers fall back to common, where the stage scopes still
        // express the dependency and the render pass carries the attachment layouts. Everything else
        // maps across directly.
        const bool scoped_before = sync_before != D3D12_BARRIER_SYNC_NONE;
        const bool scoped_after = sync_after != D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS access_before = D3D12_BARRIER_ACCESS_NO_ACCESS;
        D3D12_BARRIER_ACCESS access_after = D3D12_BARRIER_ACCESS_NO_ACCESS;

        bool needs_common = false;
        if (scoped_before)
            needs_common = !map_global_access(before_access, access_before) || access_before == D3D12_BARRIER_ACCESS_COMMON;
        if (scoped_after)
            needs_common = !map_global_access(after_access, access_after) || access_after == D3D12_BARRIER_ACCESS_COMMON || needs_common;
        if (needs_common)
        {
            if (scoped_before)
                access_before = D3D12_BARRIER_ACCESS_COMMON;
            if (scoped_after)
                access_after = D3D12_BARRIER_ACCESS_COMMON;
        }

        const D3D12_GLOBAL_BARRIER global{
            .SyncBefore = sync_before,
            .SyncAfter = sync_after,
            .AccessBefore = access_before,
            .AccessAfter = access_after,
        };
        const D3D12_BARRIER_GROUP group{ .Type = D3D12_BARRIER_TYPE_GLOBAL, .NumBarriers = 1, .pGlobalBarriers = &global };
        commands->list->Barrier(1, &group);
    }

    void begin_render_pass(CommandBuffer* commands, const RenderingDesc& desc) noexcept
    {
        assert(commands && commands->recording && !commands->rendering && "begin_render_pass requires a recording command buffer");
        assert(desc.colors.size <= max_color_attachments && "too many color attachments");

        D3D12_CPU_DESCRIPTOR_HANDLE color_handles[max_color_attachments]{};
        uint32x2 extent{};
        commands->color_view_count = static_cast<uint32>(desc.colors.size);
        for (size_t index = 0; index < desc.colors.size; ++index)
        {
            RenderView* view = desc.colors.data[index].render_view;
            assert(view && "color attachment requires a render view");
            transition_texture(
                *commands,
                *view->texture,
                D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                D3D12_BARRIER_SYNC_RENDER_TARGET,
                D3D12_BARRIER_ACCESS_RENDER_TARGET
            );
            color_handles[index] = view->handle;
            commands->color_views[index] = view;
            extent = { .x = mip_extent(view->texture->desc.extent.x, view->desc.mip_level),
                       .y = mip_extent(view->texture->desc.extent.y, view->desc.mip_level) };
        }

        RenderView* depth_view = desc.depth.render_view ? desc.depth.render_view : desc.stencil.render_view;
        commands->depth_view = depth_view;
        if (depth_view)
        {
            transition_texture(
                *commands,
                *depth_view->texture,
                D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
            );
            if (extent.x == 0)
                extent = { .x = mip_extent(depth_view->texture->desc.extent.x, depth_view->desc.mip_level),
                           .y = mip_extent(depth_view->texture->desc.extent.y, depth_view->desc.mip_level) };
        }

        commands->list->OMSetRenderTargets(
            static_cast<uint32>(desc.colors.size),
            desc.colors.size != 0 ? color_handles : nullptr,
            FALSE,
            depth_view ? &depth_view->handle : nullptr
        );

        for (size_t index = 0; index < desc.colors.size; ++index)
        {
            const ColorAttachment& attachment = desc.colors.data[index];
            if (attachment.load == LoadOp::clear)
            {
                const float clear[4]{ attachment.clear.x, attachment.clear.y, attachment.clear.z, attachment.clear.w };
                commands->list->ClearRenderTargetView(color_handles[index], clear, 0, nullptr);
            }
            else if (attachment.load == LoadOp::discard)
            {
                const RenderView& view = *attachment.render_view;
                // A volume slice shares its subresource with other slices, so preserve it instead of discarding the whole mip.
                if (view.texture->desc.type != TextureType::three_d)
                {
                    const D3D12_DISCARD_REGION region{
                        .FirstSubresource = view.desc.mip_level + view.desc.slice * view.texture->desc.mip_levels,
                        .NumSubresources = 1,
                    };
                    commands->list->DiscardResource(view.texture->resource, &region);
                }
            }
        }
        if (depth_view)
        {
            D3D12_CLEAR_FLAGS flags{};
            if (desc.depth.render_view && desc.depth.load == LoadOp::clear)
                flags |= D3D12_CLEAR_FLAG_DEPTH;
            if (desc.stencil.render_view && desc.stencil.load == LoadOp::clear)
                flags |= D3D12_CLEAR_FLAG_STENCIL;
            if (flags != 0)
                commands->list->ClearDepthStencilView(depth_view->handle, flags, desc.depth.clear, desc.stencil.clear, 0, nullptr);
        }

        // begin_render_pass resets a full render-area viewport and scissor and disables depth/stencil.
        // The viewport height is negative so clip space matches Vulkan, where Y points down.
        const D3D12_VIEWPORT viewport{
            .TopLeftX = 0.0f,
            .TopLeftY = static_cast<float>(extent.y),
            .Width = static_cast<float>(extent.x),
            .Height = -static_cast<float>(extent.y),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f,
        };
        const D3D12_RECT scissor{ .left = 0, .top = 0, .right = static_cast<LONG>(extent.x), .bottom = static_cast<LONG>(extent.y) };
        commands->list->RSSetViewports(1, &viewport);
        commands->list->RSSetScissorRects(1, &scissor);
        set_depth_stencil(commands, {});
        commands->rendering = true;
    }

    void end_render_pass(CommandBuffer* commands) noexcept
    {
        assert(commands && commands->rendering && "end_render_pass requires an open render pass");
        for (uint32 index = 0; index < commands->color_view_count; ++index)
        {
            transition_texture(
                *commands,
                *commands->color_views[index]->texture,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON,
                D3D12_BARRIER_SYNC_ALL,
                D3D12_BARRIER_ACCESS_COMMON
            );
        }
        if (commands->depth_view)
        {
            transition_texture(
                *commands,
                *commands->depth_view->texture,
                D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON,
                D3D12_BARRIER_SYNC_ALL,
                D3D12_BARRIER_ACCESS_COMMON
            );
        }
        commands->rendering = false;
        commands->color_view_count = 0;
        commands->depth_view = nullptr;
    }

    void set_viewport(CommandBuffer* commands, const Viewport& viewport) noexcept
    {
        assert(commands && commands->recording && "set_viewport requires a recording command buffer");
        const D3D12_VIEWPORT value{
            .TopLeftX = viewport.x,
            .TopLeftY = viewport.y + viewport.height,
            .Width = viewport.width,
            .Height = -viewport.height,
            .MinDepth = viewport.min_depth,
            .MaxDepth = viewport.max_depth,
        };
        commands->list->RSSetViewports(1, &value);
    }

    void set_scissor(CommandBuffer* commands, const Scissor& scissor) noexcept
    {
        assert(commands && commands->recording && "set_scissor requires a recording command buffer");
        const D3D12_RECT value{
            .left = scissor.x,
            .top = scissor.y,
            .right = scissor.x + static_cast<LONG>(scissor.width),
            .bottom = scissor.y + static_cast<LONG>(scissor.height),
        };
        commands->list->RSSetScissorRects(1, &value);
    }

    void set_depth_stencil(CommandBuffer* commands, const DepthStencilState& state) noexcept
    {
        assert(commands && commands->recording && "set_depth_stencil requires a recording command buffer");
        // D3D12 bakes depth-stencil state into the PSO, so it is resolved when a PSO is bound.
        commands->depth_stencil = state;
        commands->list->OMSetFrontAndBackStencilRef(state.front.reference, state.back.reference);
        commands->depth_stencil.front.reference = 0;
        commands->depth_stencil.back.reference = 0;
    }

    void bind_pso(CommandBuffer* commands, const PSO* pso) noexcept
    {
        assert(commands && commands->recording && pso && pso->pipeline && "bind_pso received an invalid argument");
        commands->bound_pso = pso;
        if (pso->compute)
        {
            commands->list->SetPipelineState(pso->pipeline);
            commands->bound_pipeline = pso->pipeline;
            return;
        }
        if (!pso->mesh)
            commands->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void draw(CommandBuffer* commands, ByteSpan root, uint32 vertex_count, uint32 instance_count, uint32 first_vertex, uint32 first_instance) noexcept
    {
        assert(commands && commands->rendering && "draw requires an open render pass");
        resolve_graphics_pipeline(*commands);
        emit_root_data(*commands, root);
        commands->list->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
    }

    void draw_indexed(
        CommandBuffer* commands,
        ByteSpan root,
        GpuRange indices,
        IndexType type,
        uint32 index_count,
        uint32 instance_count,
        uint32 first_index,
        int32 vertex_offset,
        uint32 first_instance
    ) noexcept
    {
        assert(commands && commands->rendering && "draw_indexed requires an open render pass");
        const DecodedRange range = decode_gpu_range(*commands->state, indices);
        const D3D12_INDEX_BUFFER_VIEW view{
            .BufferLocation = range.heap->resource->GetGPUVirtualAddress() + range.offset,
            .SizeInBytes = static_cast<uint32>(indices.size),
            .Format = type == IndexType::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
        };
        commands->list->IASetIndexBuffer(&view);
        resolve_graphics_pipeline(*commands);
        emit_root_data(*commands, root);
        commands->list->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void dispatch(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
    {
        assert(commands && commands->recording && !commands->rendering && "dispatch requires a recording command buffer outside a render pass");
        emit_root_data(*commands, root);
        commands->list->Dispatch(group_count.x, group_count.y, group_count.z);
    }

    namespace {

        void execute_indirect(
            CommandBuffer& commands,
            ByteSpan root,
            GpuRange arguments,
            D3D12_INDIRECT_ARGUMENT_TYPE type,
            uint32 command_count,
            uint32 stride
        ) noexcept
        {
            if (command_count == 0)
                return;
            uint32 argument_size = sizeof(D3D12_DISPATCH_ARGUMENTS);
            if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW)
                argument_size = sizeof(D3D12_DRAW_ARGUMENTS);
            else if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)
                argument_size = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
            if (stride == 0)
                stride = argument_size;
            const DecodedRange range = decode_gpu_range(*commands.state, arguments);
            assert(arguments.size >= uint64(command_count - 1) * stride + argument_size && "indirect arguments exceed their memory range");
            Device& device = *commands.state;
            ID3D12CommandSignature* signature = nullptr;
            for (uint32 index = 0; index < device.indirect_signature_count; ++index)
            {
                const detail::IndirectSignature& cached = device.indirect_signatures[index];
                if (cached.type == type && cached.stride == stride)
                {
                    signature = cached.signature;
                    break;
                }
            }
            if (!signature)
            {
                const D3D12_INDIRECT_ARGUMENT_DESC argument{ .Type = type };
                const D3D12_COMMAND_SIGNATURE_DESC desc{ .ByteStride = stride, .NumArgumentDescs = 1, .pArgumentDescs = &argument };
                require_hr(device.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)));
                if (device.indirect_signature_count == device.indirect_signature_capacity)
                {
                    device.indirect_signature_capacity = device.indirect_signature_capacity == 0 ? 4 : device.indirect_signature_capacity * 2;
                    device.indirect_signatures = static_cast<detail::IndirectSignature*>(
                        realloc(device.indirect_signatures, device.indirect_signature_capacity * sizeof(detail::IndirectSignature))
                    );
                }
                device.indirect_signatures[device.indirect_signature_count++] = { .type = type, .stride = stride, .signature = signature };
            }
            if (type != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)
                resolve_graphics_pipeline(commands);
            emit_root_data(commands, root);
            commands.list->ExecuteIndirect(signature, command_count, range.heap->resource, range.offset, nullptr, 0);
        }

    } // namespace

    void draw_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
    {
        assert(commands && commands->rendering && "draw_indirect requires an open render pass");
        execute_indirect(*commands, root, arguments, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, draw_count, stride);
    }

    void draw_indexed_indirect(
        CommandBuffer* commands,
        ByteSpan root,
        GpuRange indices,
        IndexType type,
        GpuRange arguments,
        uint32 draw_count,
        uint32 stride
    ) noexcept
    {
        assert(commands && commands->rendering && "draw_indexed_indirect requires an open render pass");
        const DecodedRange range = decode_gpu_range(*commands->state, indices);
        const D3D12_INDEX_BUFFER_VIEW view{
            .BufferLocation = range.heap->resource->GetGPUVirtualAddress() + range.offset,
            .SizeInBytes = static_cast<uint32>(indices.size),
            .Format = type == IndexType::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT,
        };
        commands->list->IASetIndexBuffer(&view);
        execute_indirect(*commands, root, arguments, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, draw_count, stride);
    }

    void dispatch_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments) noexcept
    {
        assert(commands && commands->recording && !commands->rendering && "dispatch_indirect requires commands outside a render pass");
        execute_indirect(*commands, root, arguments, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, 1, 0);
    }

    void draw_meshlets(CommandBuffer* commands, ByteSpan root, uint32x3 group_count) noexcept
    {
        assert(commands && commands->rendering && "draw_meshlets requires an open render pass");
        resolve_graphics_pipeline(*commands);
        emit_root_data(*commands, root);
        commands->list->DispatchMesh(group_count.x, group_count.y, group_count.z);
    }

    void draw_meshlets_indirect(CommandBuffer* commands, ByteSpan root, GpuRange arguments, uint32 draw_count, uint32 stride) noexcept
    {
        assert(commands && commands->rendering && "draw_meshlets_indirect requires an open render pass");
        execute_indirect(*commands, root, arguments, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, draw_count, stride);
    }

    // ---------------------------------------------------------------------------
    // Submission
    // ---------------------------------------------------------------------------

    namespace {

        void submit_commands(Device& device, Span<CommandBuffer* const> commands, TimelinePoint completion, Swapchain* present) noexcept
        {
            if (device.submit_capacity < commands.size)
            {
                free(device.submit_lists);
                device.submit_capacity = commands.size < 8 ? 8 : commands.size;
                device.submit_lists = static_cast<ID3D12CommandList**>(calloc(device.submit_capacity, sizeof(ID3D12CommandList*)));
            }

            for (size_t index = 0; index < commands.size; ++index)
            {
                CommandBuffer* buffer = commands.data[index];
                assert(buffer && buffer->recording && buffer->state == &device && "submit received a command buffer that was not begun on this device");
                assert(!buffer->rendering && "submit received a command buffer with an open render pass");
                if (present && index + 1 == commands.size)
                {
                    Texture* back_buffer = present->textures[present->image_index];
                    transition_texture(*buffer, *back_buffer, D3D12_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON);
                }
                assert_hr(buffer->list->Close());
                buffer->recording = false;
                device.submit_lists[index] = buffer->list;
            }

            if (commands.size != 0)
                device.queue->ExecuteCommandLists(static_cast<uint32>(commands.size), device.submit_lists);

            const uint64 retirement = ++device.command_retirement_value;
            require_hr(device.queue->Signal(device.command_retirement, retirement));
            for (size_t index = 0; index < commands.size; ++index)
            {
                detail::CommandContext* context = commands.data[index]->context;
                context->active = false;
                context->retire_value = retirement;
                assert(device.active_command_buffers != 0);
                --device.active_command_buffers;
            }
            if (completion.semaphore)
                require_hr(device.queue->Signal(completion.semaphore->fence, completion.value));
            drain_debug_messages(device.info_queue);
        }

    } // namespace

    void submit(Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
    {
        assert(commands.size != 0 && commands.data && commands.data[0] && "submit requires at least one command buffer");
        submit_commands(*commands.data[0]->state, commands, completion, nullptr);
    }

    void submit_and_present(Device* device, Span<CommandBuffer* const> commands, TimelinePoint completion) noexcept
    {
        assert(device && device->swapchain && "submit_and_present requires a windowed device");
        Swapchain* present = device->acquired_swapchain;
        submit_commands(*device, commands, completion, present);
        if (present && present->swapchain)
        {
            const HRESULT result = present->swapchain->Present(1, 0);
            if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
                return;
            assert_hr(result);
        }
        device->acquired_swapchain = nullptr;
    }

} // namespace gpu
