struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_index : SV_VertexID)
{
    static const float2 positions[3] = {float2(-1, -1), float2(1, -1), float2(-1, 1)};
    VertexOutput output;
    output.position = float4(positions[vertex_index % 3], 0, 1);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(1, 0, 0, 1);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain(out vertices VertexOutput output_vertices[3], out indices uint3 output_triangles[1])
{
    SetMeshOutputCounts(3, 1);
    output_vertices[0].position = float4(-1, -1, 0, 1);
    output_vertices[1].position = float4(1, -1, 0, 1);
    output_vertices[2].position = float4(-1, 1, 0, 1);
    output_triangles[0] = uint3(0, 1, 2);
}
