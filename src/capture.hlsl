// Shader that rebuilds a game's native (original, low-resolution) picture from the
// frame the game presents. Compiled at build time by fxc into capture_vs.h and
// capture_ps.h (see CMakeLists.txt); renderer.cpp draws with it into a texture of the
// native size, and then runs the RetroArch preset on that texture.
//
// The game drew a native.x x native.y picture and stretched it over the rectangle
// `rect` (x, y, w, h) of the frame, so each native pixel became a block ("cell") of
// frame pixels. This shader outputs one pixel per native pixel, read from the centre
// of its cell: for native pixel i (on each axis), frame pixel
//   rect.xy + floor((i + 0.5) * rect.zw / native)
// It uses integer maths, identical to cell_centre() in grid_detect.h (the grid
// detector's code), so it reads exactly the pixels the detector checked when it found
// the grid.

// Values set by renderer.cpp before each draw (struct CaptureParams there has the same
// layout).
cbuffer CaptureParams : register(b0)
{
    uint4 rect;   // x, y, w, h in frame pixels: where the stretched picture is
    uint2 native; // native resolution: width, height of the original picture
    // Unused; rounds the buffer up to 32 bytes (a multiple of 16 is required).
    uint2 padding;
};

// The frame (a copy of it, see Renderer::snapshot in renderer.h), viewed as plain
// UNORM so the shader reads the values the game stored, not sRGB-decoded ones.
Texture2D<float4> frame : register(t0);

// Vertex shader: makes one triangle that covers the whole render target, from the
// vertex number alone (no vertex buffer). Vertices 0, 1 and 2 land at (-1, 1), (3, 1)
// and (-1, -3) in clip space, where the visible area is -1..1 on both axes; the
// visible square lies inside the triangle, so every target pixel is drawn once.
void VSMain(uint id : SV_VertexID, out float4 pos : SV_Position)
{
    // uv is (0, 0), (2, 0), (0, 2) for vertices 0, 1, 2; then map to clip space (y up).
    float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Pixel shader: runs once for each pixel of the native-size target and returns the
// frame pixel at the centre of that native pixel's cell, with alpha set to 1 (opaque)
// because the frame's alpha channel is not part of the picture.
float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    const uint2 i = uint2(pos.xy); // native pixel index (pos is i + 0.5)
    // floor((i + 0.5) * w / native), written as (2i + 1) * w / (2 * native) to stay in
    // integers (unsigned division rounds down).
    const uint2 p = rect.xy + ((2 * i + 1) * rect.zw) / (2 * native);
    return float4(frame.Load(int3(p, 0)).rgb, 1.0);
}
