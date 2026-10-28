// blit_vs.hlsl — 1.0.36 Slice 1 fullscreen-triangle vertex shader.
// UV mapping: uv.y = 1 - p.y so guest row 0 (image top) lands at screen
// top. A naive uv = p mapping would flip the image vertically; if the
// console image ever appears upside-down, this one term is the suspect.
struct VOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VOut main(uint vid : SV_VertexID)
{
    VOut o;
    float2 p = float2((float)((vid << 1) & 2), (float)(vid & 2));
    o.pos = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}
