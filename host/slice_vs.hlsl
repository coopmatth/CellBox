// slice_vs.hlsl - 1.0.42 Slice 2 vertex shader: pixel-space -> NDC
// transform with dual-space guard.
// Input: guest POS attribute converted to float3 on upload (BE->LE swapped).
// BOOT-VP-DIAG (1.0.41) PROVED BO3's early draws carry 1280x720 pixel-space
// positions (x 864..1180, y 621..679), which fed raw into SV_Position clipped
// 100% outside NDC [-1,1]. Pixel-space coordinates therefore transform to
// NDC; pre-normalized coordinates (|x|<=2 and |y|<=2) pass through unchanged.
// EARLY-2D-PASS APPROXIMATION ONLY: UV-space [0,1] draws also pass through
// unchanged (known limitation, not fixed by this guard); 3D object-space
// geometry needs the real vertex-program transform (Slice 3+ work, not hidden).
// D3D12 NDC is y-up, pixel space y-down, so py=0 (top) -> +1 and
// py=720 (bottom) -> -1. 1280x720 matches the scratch/bridge dims.
// The blit's uv.y = 1-p.y flip is texture sampling, NOT geometry - it is
// untouched, so there is no double flip. z=0, w=1 (unchanged).
// 1.1.4: second vertex stream carries the guest diffuse colour (RSX attribute 3,
// converted to float4 on upload; constant grey when the attribute is not a
// readable vertex array). It is passed through to the pixel shader.
struct Vin
{
    float3 pos : POSITION;
    float4 col : COLOR;
};

struct Vout
{
    float4 pos : SV_Position;
    float4 col : COLOR;
};

Vout main(Vin v)
{
    float2 xy = v.pos.xy;
    if (abs(xy.x) > 2.0f || abs(xy.y) > 2.0f)
    {
        xy.x = (xy.x / 1280.0f) * 2.0f - 1.0f;
        xy.y = 1.0f - (xy.y / 720.0f) * 2.0f;
    }
    Vout o;
    o.pos = float4(xy, 0.0f, 1.0f);
    o.col = v.col;
    return o;
}
