#include "../src/NoGraphicsAPI_dx12.cpp"
#include "dx12_test_shared.h"

using namespace gpu;

namespace {

    bool expect(bool condition, const char* message)
    {
        if (!condition)
            fprintf(stderr, "FAILED: %s\n", message);
        return condition;
    }

    Span<uint32> read_test_shader(const char* stage)
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/dx12_test.%s.dxil", DX12_TEST_SHADER_DIRECTORY, stage);
        FILE* file = fopen(path, "rb");
        if (!file)
            return {};
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        rewind(file);
        uint32* data = static_cast<uint32*>(malloc(size));
        fread(data, 1, size, file);
        fclose(file);
        return { data, size_t(size) / sizeof(uint32) };
    }

    struct TestTexture
    {
        TextureHeap heap{};
        Texture* texture = nullptr;
    };

    TestTexture create_test_texture(Device* device, const TextureDesc& desc)
    {
        const SizeAlign allocation = get_texture_size_align(device, desc);
        const TextureHeap heap = create_texture_heap(device, allocation.size);
        return { .heap = heap, .texture = create_texture(device, desc, heap, 0) };
    }

    void destroy_test_texture(const TestTexture& texture)
    {
        destroy_texture(texture.texture);
        destroy_texture_heap(texture.heap);
    }

    void finish_commands(Device* device, CommandBuffer* commands)
    {
        submit({ commands }, {});
        wait_idle(device);
        drain_debug_messages(device->info_queue);
    }

    bool test_descriptor_reuse(Device* device)
    {
        bool valid = true;
        for (uint32 iteration = 0; iteration < 17000; ++iteration)
        {
            const GpuHeap descriptors = create_gpu_heap(device, device->caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
            const detail::GpuHeapRecord* record = device->find_heap(reinterpret_cast<uint64>(descriptors.range.gpu));
            valid &= record->descriptor_base == 0 && record->heap_id == 1;
            destroy_gpu_heap(descriptors);
        }
        for (uint32 iteration = 0; iteration < 20; ++iteration)
        {
            const GpuHeap heap = create_gpu_heap(device, 16);
            valid &= device->find_heap(reinterpret_cast<uint64>(heap.range.gpu))->heap_id == 1;
            destroy_gpu_heap(heap);
        }
        const TestTexture color = create_test_texture(device, { .usage = TextureUsage::color_attachment });
        const TestTexture depth = create_test_texture(device, { .format = Format::d32_float, .usage = TextureUsage::depth_stencil_attachment });
        for (uint32 iteration = 0; iteration < 600; ++iteration)
        {
            RenderView* color_view = create_render_view(color.texture);
            RenderView* depth_view = create_render_view(depth.texture);
            valid &= color_view->slot == 0 && depth_view->slot == 0;
            destroy_render_view(color_view);
            destroy_render_view(depth_view);
        }
        for (uint32 iteration = 0; iteration < 4; ++iteration)
        {
            const GpuHeap textures = create_gpu_heap(device, device->caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
            const GpuHeap samplers = create_gpu_heap(device, device->caps.sampler_descriptor_size, MemoryType::sampler_descriptor_heap);
            CommandBuffer* commands = begin_commands(device);
            set_texture_descriptor_heap(commands, gpu_range(textures));
            set_sampler_descriptor_heap(commands, gpu_range(samplers));
            finish_commands(device, commands);
            destroy_gpu_heap(samplers);
            destroy_gpu_heap(textures);
        }
        destroy_test_texture(depth);
        destroy_test_texture(color);
        return expect(valid, "descriptor slots and heap IDs must be reusable");
    }

    bool test_texture_copy(Device* device, TextureType type, uint32 width, uint32 height, bool padded, bool gpu_source)
    {
        const bool volume = type == TextureType::three_d;
        const uint32 depth = volume ? 2 : 1;
        const uint32 layers = type == TextureType::two_d_array ? 2 : 1;
        const TextureDesc desc{
            .type = type,
            .extent = { .x = width + 4, .y = height + 4, .z = volume ? 4u : 1u },
            .mip_levels = 2,
            .layer_count = layers > 1 ? 4u : 1u,
            .usage = TextureUsage::transfer_source | TextureUsage::transfer_destination,
        };
        const TestTexture texture = create_test_texture(device, desc);
        const uint64 row_bytes = uint64(width) * 4;
        const uint64 row_pitch = row_bytes + (padded ? 12 : 0);
        const uint64 slice_pitch = row_pitch * height + (padded ? 20 : 0);
        const uint64 memory_size = slice_pitch * layers * depth;
        const GpuHeap upload = create_gpu_heap(device, memory_size + 32);
        const GpuHeap readback = create_gpu_heap(device, memory_size + 32, MemoryType::readback);
        const GpuHeap staging = gpu_source ? create_gpu_heap(device, memory_size + 32, MemoryType::gpu_only) : GpuHeap{};
        memset(upload.range.cpu, 0xc3, memory_size + 32);
        memset(readback.range.cpu, 0x5a, memory_size + 32);
        for (uint32 slice = 0; slice < layers * depth; ++slice)
            for (uint32 row = 0; row < height; ++row)
                for (uint64 column = 0; column < row_bytes; ++column)
                    upload.range.cpu[16 + slice * slice_pitch + row * row_pitch + column] = byte(1 + (slice * 37 + row * 11 + column) % 200);
        const TextureCopyDesc region{
            .base_slice = layers > 1 ? 1u : 0u,
            .slice_count = layers,
            .offset = { .x = 2, .y = 1, .z = volume ? 1u : 0u },
            .extent = { .x = width, .y = height, .z = depth },
            .row_pitch_bytes = padded ? row_pitch : 0,
            .slice_pitch_bytes = padded ? slice_pitch : 0,
        };
        CommandBuffer* commands = begin_commands(device);
        if (gpu_source)
        {
            copy_memory(commands, gpu_range(upload), gpu_range(staging));
            barrier(commands, Stage::transfer, Access::transfer_write, Stage::transfer, Access::transfer_read);
        }
        copy_memory_to_texture(commands, { .gpu = (gpu_source ? staging.range.gpu : upload.range.gpu) + 16, .size = memory_size }, texture.texture, region);
        barrier(commands, Stage::transfer, Access::transfer_write, Stage::transfer, Access::transfer_read);
        copy_texture_to_memory(commands, texture.texture, { .gpu = readback.range.gpu + 16, .size = memory_size }, region);
        finish_commands(device, commands);
        bool valid = true;
        for (uint32 slice = 0; slice < layers * depth; ++slice)
            for (uint32 row = 0; row < height; ++row)
                valid &= memcmp(
                             upload.range.cpu + 16 + slice * slice_pitch + row * row_pitch,
                             readback.range.cpu + 16 + slice * slice_pitch + row * row_pitch,
                             row_bytes
                         ) == 0;
        for (uint32 index = 0; index < 16; ++index)
            valid &= readback.range.cpu[index] == 0x5a && readback.range.cpu[16 + memory_size + index] == 0x5a;
        destroy_gpu_heap(staging);
        destroy_gpu_heap(readback);
        destroy_gpu_heap(upload);
        destroy_test_texture(texture);
        return expect(valid, "texture copy must preserve regions, pitches, slices and range boundaries");
    }

    uint32 sample_descriptor(Device* device, PSO* compute, const GpuHeap& descriptors, uint32 descriptor_index, uint32 operation)
    {
        const GpuHeap output = create_gpu_heap(device, 16, MemoryType::gpu_only);
        const GpuHeap readback = create_gpu_heap(device, 16, MemoryType::readback);
        const Dx12TestRoot root{ .output = reinterpret_cast<uint32*>(output.range.gpu), .descriptor_index = descriptor_index, .operation = operation };
        CommandBuffer* commands = begin_commands(device);
        set_texture_descriptor_heap(commands, gpu_range(descriptors));
        bind_pso(commands, compute);
        dispatch(commands, ByteSpan(root), { .x = 1, .y = 1, .z = 1 });
        barrier(commands, Stage::compute, Access::shader_write, Stage::transfer, Access::transfer_read);
        copy_memory(commands, gpu_range(output), gpu_range(readback));
        finish_commands(device, commands);
        const uint32 result = *reinterpret_cast<uint32*>(readback.range.cpu);
        destroy_gpu_heap(readback);
        destroy_gpu_heap(output);
        return result;
    }

    bool test_texture_descriptors(Device* device, PSO* compute)
    {
        bool valid = true;
        const GpuHeap descriptors = create_gpu_heap(device, device->caps.texture_descriptor_size * 2, MemoryType::texture_descriptor_heap);
        const GpuHeap upload = create_gpu_heap(device, 4);
        *reinterpret_cast<uint32*>(upload.range.cpu) = 91;
        constexpr TextureType types[]{ TextureType::one_d, TextureType::two_d, TextureType::three_d };
        for (uint32 index = 0; index < 3; ++index)
        {
            const TestTexture texture = create_test_texture(
                device,
                { .type = types[index],
                  .format = Format::r32_uint,
                  .usage = TextureUsage::sampled | TextureUsage::storage | TextureUsage::transfer_destination }
            );
            write_texture_descriptor(device, descriptors.range.cpu, texture.texture, TextureDescriptorType::sampled);
            write_texture_descriptor(device, descriptors.range.cpu + device->caps.texture_descriptor_size, texture.texture, TextureDescriptorType::storage);
            CommandBuffer* commands = begin_commands(device);
            copy_memory_to_texture(commands, gpu_range(upload), texture.texture);
            barrier(commands, Stage::transfer, Access::transfer_write, Stage::compute, Access::shader_read);
            finish_commands(device, commands);
            valid &= sample_descriptor(device, compute, descriptors, 0, index + 1) == 91;
            valid &= sample_descriptor(device, compute, descriptors, 1, index + 4) == 91;
            valid &= sample_descriptor(device, compute, descriptors, 0, index + 1) == 92;
            destroy_test_texture(texture);
        }
        destroy_gpu_heap(upload);
        destroy_gpu_heap(descriptors);
        return expect(valid, "1D, 2D and 3D sampled/storage views must address the intended resource");
    }

    bool test_cube_array_descriptor(Device* device, PSO* compute)
    {
        const TestTexture texture = create_test_texture(
            device,
            { .type = TextureType::cube_array, .layer_count = 18, .usage = TextureUsage::sampled | TextureUsage::transfer_destination }
        );
        const GpuHeap descriptors = create_gpu_heap(device, device->caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
        const GpuHeap samplers = create_gpu_heap(device, device->caps.sampler_descriptor_size, MemoryType::sampler_descriptor_heap);
        const GpuHeap upload = create_gpu_heap(device, 18 * 4);
        for (uint32 face = 0; face < 18; ++face)
            memset(upload.range.cpu + face * 4, face + 1, 4);
        write_texture_descriptor(device, descriptors.range.cpu, texture.texture, TextureDescriptorType::sampled, { .base_layer = 6, .layer_count = 12 });
        write_sampler_descriptor(device, samplers.range.cpu, { .min_filter = Filter::nearest, .mag_filter = Filter::nearest, .mip_filter = Filter::nearest });
        CommandBuffer* commands = begin_commands(device);
        copy_memory_to_texture(commands, gpu_range(upload), texture.texture);
        finish_commands(device, commands);
        const bool valid = sample_descriptor(device, compute, descriptors, 0, 7) == 13;
        destroy_gpu_heap(upload);
        destroy_gpu_heap(samplers);
        destroy_gpu_heap(descriptors);
        destroy_test_texture(texture);
        return expect(valid, "cube array view must honor base layer and cube count");
    }

    bool test_indirect_dispatch(Device* device, PSO* compute)
    {
        const GpuHeap output = create_gpu_heap(device, 16, MemoryType::gpu_only);
        const GpuHeap readback = create_gpu_heap(device, 16, MemoryType::readback);
        const GpuHeap arguments = create_gpu_heap(device, 16);
        *reinterpret_cast<D3D12_DISPATCH_ARGUMENTS*>(arguments.range.cpu) = { .ThreadGroupCountX = 4, .ThreadGroupCountY = 1, .ThreadGroupCountZ = 1 };
        const Dx12TestRoot root{ .output = reinterpret_cast<uint32*>(output.range.gpu) };
        CommandBuffer* commands = begin_commands(device);
        bind_pso(commands, compute);
        dispatch_indirect(commands, ByteSpan(root), gpu_range(arguments));
        barrier(commands, Stage::compute, Access::shader_write, Stage::transfer, Access::transfer_read);
        copy_memory(commands, gpu_range(output), gpu_range(readback));
        finish_commands(device, commands);
        const uint32* values = reinterpret_cast<uint32*>(readback.range.cpu);
        const bool valid = values[0] == 42 && values[1] == 43 && values[2] == 44 && values[3] == 45;
        destroy_gpu_heap(arguments);
        destroy_gpu_heap(readback);
        destroy_gpu_heap(output);
        return expect(valid, "indirect dispatch must execute its argument buffer and root data");
    }

    bool test_raster_commands(Device* device, Span<const uint32> vertex, Span<const uint32> fragment, Span<const uint32> mesh)
    {
        const TestTexture color = create_test_texture(
            device,
            { .extent = { .x = 64, .y = 64, .z = 1 }, .mip_levels = 3, .usage = TextureUsage::color_attachment | TextureUsage::transfer_source }
        );
        RenderView* view = create_render_view(color.texture, { .mip_level = 2 });
        PSO* graphics =
            create_graphics_pso(device, { .vertex_spirv = vertex, .fragment_spirv = fragment, .color_targets = { { .format = Format::rgba8_unorm } } });
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 features{};
        device->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &features, sizeof(features));
        PSO* mesh_pipeline =
            features.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED
                ? create_mesh_pso(device, { .mesh_spirv = mesh, .fragment_spirv = fragment, .color_targets = { { .format = Format::rgba8_unorm } } })
                : nullptr;
        const GpuHeap readback = create_gpu_heap(device, 16 * 16 * 4 * 5, MemoryType::readback);
        const GpuHeap arguments = create_gpu_heap(device, 128);
        const GpuHeap indices = create_gpu_heap(device, 6);
        const uint16 index_data[]{ 0, 1, 2 };
        memcpy(indices.range.cpu, index_data, sizeof(index_data));
        memset(arguments.range.cpu, 0, 128);
        *reinterpret_cast<D3D12_DRAW_ARGUMENTS*>(arguments.range.cpu + 48) = { .VertexCountPerInstance = 3, .InstanceCount = 1 };
        *reinterpret_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(arguments.range.cpu + 80) = { .IndexCountPerInstance = 3, .InstanceCount = 1 };
        *reinterpret_cast<D3D12_DISPATCH_MESH_ARGUMENTS*>(arguments.range.cpu + 112) = { .ThreadGroupCountX = 1,
                                                                                         .ThreadGroupCountY = 1,
                                                                                         .ThreadGroupCountZ = 1 };
        CommandBuffer* commands = begin_commands(device);
        for (uint32 mode = 0; mode < (mesh_pipeline ? 5u : 4u); ++mode)
        {
            begin_render_pass(commands, { .colors = { { .render_view = view, .load = LoadOp::clear, .clear = { .w = 1 } } } });
            bind_pso(commands, mode == 4 ? mesh_pipeline : graphics);
            if (mode == 0)
                set_viewport(commands, { .width = 16, .height = 16 });
            if (mode < 2)
                draw(commands, {}, 3);
            else if (mode == 2)
                draw_indirect(commands, {}, { .gpu = arguments.range.gpu + 16, .size = 48 }, 2, 32);
            else if (mode == 3)
                draw_indexed_indirect(commands, {}, gpu_range(indices), IndexType::uint16, { .gpu = arguments.range.gpu + 80, .size = 20 });
            else
                draw_meshlets_indirect(commands, {}, { .gpu = arguments.range.gpu + 112, .size = 12 });
            end_render_pass(commands);
            copy_texture_to_memory(commands, color.texture, { .gpu = readback.range.gpu + mode * 1024, .size = 1024 }, { .mip_level = 2 });
        }
        finish_commands(device, commands);
        bool valid = true;
        for (uint32 mode = 1; mode < (mesh_pipeline ? 5u : 4u); ++mode)
            valid &= memcmp(readback.range.cpu, readback.range.cpu + mode * 1024, 1024) == 0;
        uint32 red_pixels = 0;
        for (uint32 pixel = 0; pixel < 256; ++pixel)
            red_pixels += readback.range.cpu[pixel * 4] == 255;
        valid &= red_pixels > 0;
        destroy_gpu_heap(indices);
        destroy_gpu_heap(arguments);
        destroy_gpu_heap(readback);
        destroy_pso(mesh_pipeline);
        destroy_pso(graphics);
        destroy_render_view(view);
        destroy_test_texture(color);
        return expect(valid, "mip viewport and indirect raster commands must match direct rendering");
    }

    bool test_stencil(Device* device, PSO* compute, Span<const uint32> vertex, Format format)
    {
        const TestTexture depth = create_test_texture(
            device,
            { .extent = { .x = 16, .y = 16, .z = 1 }, .format = format, .usage = TextureUsage::depth_stencil_attachment | TextureUsage::sampled }
        );
        RenderView* view = create_render_view(depth.texture);
        const GpuHeap descriptors = create_gpu_heap(device, device->caps.texture_descriptor_size, MemoryType::texture_descriptor_heap);
        write_texture_descriptor(device, descriptors.range.cpu, depth.texture, TextureDescriptorType::sampled, { .aspect = TextureAspect::stencil });
        PSO* pipeline = create_graphics_pso(device, { .vertex_spirv = vertex, .depth_format = format, .stencil_format = format });
        const GpuHeap indices = create_gpu_heap(device, 12);
        const uint16 index_data[]{ 0, 1, 2, 0, 2, 1 };
        memcpy(indices.range.cpu, index_data, sizeof(index_data));
        uint32 references[2]{};
        for (uint32 winding = 0; winding < 2; ++winding)
        {
            CommandBuffer* commands = begin_commands(device);
            begin_render_pass(
                commands,
                { .depth = { .render_view = view, .load = LoadOp::clear }, .stencil = { .render_view = view, .load = LoadOp::clear, .clear = 7 } }
            );
            set_depth_stencil(
                commands,
                { .stencil_test = true, .front = { .pass = StencilOp::replace, .reference = 17 }, .back = { .pass = StencilOp::replace, .reference = 31 } }
            );
            bind_pso(commands, pipeline);
            draw_indexed(commands, {}, gpu_range(indices), IndexType::uint16, 3, 1, winding * 3);
            end_render_pass(commands);
            finish_commands(device, commands);
            references[winding] = sample_descriptor(device, compute, descriptors, 0, 2);
        }
        const bool valid = (references[0] == 17 && references[1] == 31) || (references[0] == 31 && references[1] == 17);
        destroy_gpu_heap(indices);
        destroy_pso(pipeline);
        destroy_gpu_heap(descriptors);
        destroy_render_view(view);
        destroy_test_texture(depth);
        return expect(valid, "stencil sampling and independent face references must preserve both values");
    }

    bool test_discard_subresource(Device* device)
    {
        const TestTexture color = create_test_texture(
            device,
            { .extent = { .x = 8, .y = 8, .z = 1 }, .mip_levels = 2, .layer_count = 2, .usage = TextureUsage::color_attachment | TextureUsage::transfer_source }
        );
        RenderView* preserved = create_render_view(color.texture, { .slice = 1 });
        RenderView* discarded = create_render_view(color.texture, { .mip_level = 1 });
        const GpuHeap readback = create_gpu_heap(device, 8 * 8 * 4, MemoryType::readback);
        CommandBuffer* commands = begin_commands(device);
        begin_render_pass(commands, { .colors = { { .render_view = preserved, .load = LoadOp::clear, .clear = { .x = 1, .w = 1 } } } });
        end_render_pass(commands);
        begin_render_pass(commands, { .colors = { { .render_view = discarded, .load = LoadOp::discard } } });
        end_render_pass(commands);
        copy_texture_to_memory(commands, color.texture, gpu_range(readback), { .base_slice = 1, .slice_count = 1 });
        finish_commands(device, commands);
        bool valid = true;
        for (uint32 pixel = 0; pixel < 64; ++pixel)
            valid &= readback.range.cpu[pixel * 4] == 255;
        destroy_gpu_heap(readback);
        destroy_render_view(discarded);
        destroy_render_view(preserved);
        destroy_test_texture(color);
        return expect(valid, "discard must preserve other mips and array slices");
    }

} // namespace

int main()
{
    const DeviceInit initialized = create_device();
    if (initialized.error == Error::unsupported)
        return 77;
    if (!initialized.device)
        return 1;
    Device* device = initialized.device;
    const Span<uint32> compute_code = read_test_shader("compute");
    const Span<uint32> vertex_code = read_test_shader("vertex");
    const Span<uint32> fragment_code = read_test_shader("fragment");
    const Span<uint32> mesh_code = read_test_shader("mesh");
    if (!compute_code.data || !vertex_code.data || !fragment_code.data || !mesh_code.data)
        return 1;
    PSO* compute = create_compute_pso(device, compute_code);
    bool valid = expect(!supports_texture_format(device, Format::s8_uint, TextureUsage::depth_stencil_attachment), "stencil-only format must be unsupported");
    D3D12_BARRIER_ACCESS read_access{};
    valid &= expect(
        map_global_access(Access::shader_read, read_access) && (read_access & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0,
        "shader read barriers must include UAV reads"
    );
    valid &= test_descriptor_reuse(device);
    const bool unrestricted_pitch = device->unrestricted_texture_copy_pitch;
    for (uint32 fallback = 0; fallback < 2; ++fallback)
    {
        device->unrestricted_texture_copy_pitch = fallback == 0 && unrestricted_pitch;
        valid &= test_texture_copy(device, TextureType::two_d, 32, 2, false, false);
        valid &= test_texture_copy(device, TextureType::two_d_array, 11, 7, true, false);
        valid &= test_texture_copy(device, TextureType::three_d, 11, 7, true, true);
        valid &= test_texture_copy(device, TextureType::two_d, 257, 100, false, true);
    }
    device->unrestricted_texture_copy_pitch = unrestricted_pitch;
    valid &= test_texture_descriptors(device, compute);
    valid &= test_cube_array_descriptor(device, compute);
    valid &= test_indirect_dispatch(device, compute);
    valid &= test_raster_commands(device, vertex_code, fragment_code, mesh_code);
    valid &= test_stencil(device, compute, vertex_code, Format::d24_unorm_s8_uint);
    valid &= test_stencil(device, compute, vertex_code, Format::d32_float_s8_uint);
    valid &= test_discard_subresource(device);
    wait_idle(device);
    destroy_pso(compute);
    free(compute_code.data);
    free(vertex_code.data);
    free(fragment_code.data);
    free(mesh_code.data);
    destroy_device(device);
    return valid ? 0 : 1;
}
