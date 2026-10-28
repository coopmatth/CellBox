struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; float4 col : COLOR; };
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; o.col = i.col; return o; }
