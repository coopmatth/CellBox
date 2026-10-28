Texture2D g_font : register(t0);
SamplerState g_samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD, float4 col : COLOR) : SV_Target {
    float a = g_font.Sample(g_samp, uv).r;
    return float4(col.rgb, col.a * a);
}
