#version 450

// Capture pass for Direct3D 9: the same job as capture.slang (see there), written for
// Shader Model 3, which has no integer arithmetic and no exact texel fetch. The cell
// centre is computed in floating point and sampled with point filtering at the centre of
// that frame pixel. Float division can land one frame pixel away from the exact integer
// result near a cell edge; that pixel is still inside the same cell (cells are at least 2
// pixels wide), so for a nearest-neighbour or sharp stretch the colour is the same.

#pragma parameter rra_rect_x "Game picture: left edge in the frame (pixels)" 0.0 0.0 16384.0 1.0
#pragma parameter rra_rect_y "Game picture: top edge in the frame (pixels)" 0.0 0.0 16384.0 1.0
#pragma parameter rra_rect_w "Game picture: width in the frame (pixels)" 1.0 1.0 16384.0 1.0
#pragma parameter rra_rect_h "Game picture: height in the frame (pixels)" 1.0 1.0 16384.0 1.0
#pragma parameter rra_native_w "Game picture: native width" 1.0 1.0 16384.0 1.0
#pragma parameter rra_native_h "Game picture: native height" 1.0 1.0 16384.0 1.0

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    vec4 OutputSize;
    float rra_rect_x;
    float rra_rect_y;
    float rra_rect_w;
    float rra_rect_h;
    float rra_native_w;
    float rra_native_h;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
    // Which native pixel this output pixel is.
    const vec2 cell = floor(vTexCoord * params.OutputSize.xy);
    const vec2 origin = vec2(params.rra_rect_x, params.rra_rect_y);
    const vec2 size = vec2(params.rra_rect_w, params.rra_rect_h);
    const vec2 native = vec2(params.rra_native_w, params.rra_native_h);
    // Frame pixel at the centre of that cell, then the centre of that pixel.
    const vec2 p = origin + floor((2.0 * cell + 1.0) * size / (2.0 * native));
    FragColor = vec4(texture(Source, (p + 0.5) * params.SourceSize.zw).rgb, 1.0);
}
