set(simulation_shader ${CMAKE_CURRENT_SOURCE_DIR}/simulation.slang)
set(gbuffer_shader ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer.slang)
set(deferred_lighting_shader ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(simulation_compute_shader ${shader_dir}/simulation.comp.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(gbuffer_mesh_shader ${shader_dir}/gbuffer.mesh.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(gbuffer_fragment_shader ${shader_dir}/gbuffer.frag.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(deferred_vertex_shader ${shader_dir}/deferred.vert.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(deferred_fragment_shader ${shader_dir}/deferred.frag.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(gbuffer_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/gbuffer_shared.h
    ${CMAKE_CURRENT_SOURCE_DIR}/object_data.h)
set(simulation_shader_dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation_shared.h
    ${CMAKE_CURRENT_SOURCE_DIR}/object_data.h)

NoGraphicsAPI_compile_slang(${simulation_compute_shader} ${simulation_shader} computeMain compute
    DEPENDS ${simulation_shader_dependencies})
NoGraphicsAPI_compile_slang(${gbuffer_mesh_shader} ${gbuffer_shader} meshMain mesh
    DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${gbuffer_fragment_shader} ${gbuffer_shader} fragmentMain fragment
    DEPENDS ${gbuffer_shader_dependencies})
NoGraphicsAPI_compile_slang(${deferred_vertex_shader} ${deferred_lighting_shader} vertexMain vertex
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting_shared.h)
NoGraphicsAPI_compile_slang(${deferred_fragment_shader} ${deferred_lighting_shader} fragmentMain fragment
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting_shared.h)

NoGraphicsAPI_add_example(example_deferred_renderer
    deferred_renderer.cpp
    object_data.h
    simulation_shared.h
    gbuffer_shared.h
    deferred_lighting_shared.h
    ${simulation_shader}
    ${gbuffer_shader}
    ${deferred_lighting_shader}
    ${simulation_compute_shader}
    ${gbuffer_mesh_shader}
    ${gbuffer_fragment_shader}
    ${deferred_vertex_shader}
    ${deferred_fragment_shader}
)
target_link_libraries(example_deferred_renderer PRIVATE NoGraphicsAPIUtility::math NoGraphicsAPIUtility::textures)
target_compile_definitions(example_deferred_renderer PRIVATE
    NOGRAPHICSAPI_SIMULATION_COMPUTE_SHADER_PATH="${simulation_compute_shader}"
    NOGRAPHICSAPI_GBUFFER_MESH_SHADER_PATH="${gbuffer_mesh_shader}"
    NOGRAPHICSAPI_GBUFFER_FRAGMENT_SHADER_PATH="${gbuffer_fragment_shader}"
    NOGRAPHICSAPI_DEFERRED_VERTEX_SHADER_PATH="${deferred_vertex_shader}"
    NOGRAPHICSAPI_DEFERRED_FRAGMENT_SHADER_PATH="${deferred_fragment_shader}"
)
