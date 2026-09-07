set(shader ${CMAKE_CURRENT_SOURCE_DIR}/triangle.slang)
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(vertex_shader ${shader_dir}/triangle.vert.${NOGRAPHICSAPI_SHADER_SUFFIX})
set(fragment_shader ${shader_dir}/triangle.frag.${NOGRAPHICSAPI_SHADER_SUFFIX})

NoGraphicsAPI_compile_slang(${vertex_shader} ${shader} vertexMain vertex)
NoGraphicsAPI_compile_slang(${fragment_shader} ${shader} fragmentMain fragment)

NoGraphicsAPI_add_example(example_triangle
    triangle.cpp
    ${shader}
    ${vertex_shader}
    ${fragment_shader}
)
target_compile_definitions(example_triangle PRIVATE
    NOGRAPHICSAPI_VERTEX_SHADER_PATH="${vertex_shader}"
    NOGRAPHICSAPI_FRAGMENT_SHADER_PATH="${fragment_shader}"
)
