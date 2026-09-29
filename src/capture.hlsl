// Recovers a game's native image from the frame it presents.
//
// The game stretched a native_w x native_h image over `rect` (x, y, w, h) of the
// frame. Each output pixel (i, j) is one native pixel, read from the centre of its
// cell: rect.xy + floor((i + 0.5) * rect.zw / native). Integer maths, identical to
// cell_centre() in grid_detect.h, so the sample lands on the same pixel the
// detector validated.

cbuffer CaptureParams : register(b0)
{
    uint4 rect;   // x, y, w, h in frame pixels
    uint2 native; // native resolution
    uint2 padding;
};

Texture2D<float4> frame : register(t0);

void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    // Fullscreen triangle.
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    const uint2 i = uint2(pos.xy); // native pixel index (pos is i + 0.5)
    const uint2 p = rect.xy + ((2 * i + 1) * rect.zw) / (2 * native);
    return float4(frame.Load(int3(p, 0)).rgb, 1.0);
}
