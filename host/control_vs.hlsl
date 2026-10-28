struct VSOut { float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 pos : SV_Position; };
VSOut main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) {
    VSOut o; o.t0 = b; o.t1 = float4(a.xy * 0.5 + 0.5, 0.0, 1.0); o.pos = a; return o;
}
