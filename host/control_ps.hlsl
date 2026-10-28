float4 main(float4 t0 : TEXCOORD0, float4 t1 : TEXCOORD1) : SV_Target {
    return float4(t0.x * t1.x, t0.y * t1.y, t0.z, t0.w);
}
