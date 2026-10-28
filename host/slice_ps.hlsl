// slice_ps.hlsl — 1.0.36 Slice 2 silhouette pixel shader.
// Solid magenta: proves a draw executed and its vertices reached the
// rasterizer, without any texture/shader-program dependency.
float4 main() : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
