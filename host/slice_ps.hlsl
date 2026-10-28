// slice_ps.hlsl - 1.1.4 Slice 2 pixel shader: interpolated vertex colour.
// Replaces the 1.0.36 solid-magenta silhouette shader. The colour comes from
// the guest's diffuse vertex attribute (see slice_vs.hlsl). Texturing is NOT
// implemented here: it needs the RSX texture/sampler registers and a
// fragment-program translation, which is later slice work.
struct Pin
{
    float4 pos : SV_Position;
    float4 col : COLOR;
};

float4 main(Pin p) : SV_Target
{
    return float4(saturate(p.col.rgb), 1.0f);
}
