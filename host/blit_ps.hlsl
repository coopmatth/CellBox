// blit_ps.hlsl — 1.0.36 Slice 1 blit pixel shader.
// Samples the bound SRV (display texture with BGRA-swizzle component
// mapping, or scratch RTV with identity mapping) with a point sampler.
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return tex.Sample(samp, uv);
}
