#version 450

// Direct3D 9 (Shader Model 3) version of nearest.slang: the same enlargement and colour
// inversion computed in floating point and sampled with point filtering at the centre of
// the chosen pixel, as Shader Model 3 has no integer arithmetic or exact texel fetch.
// Output pixel (x, y) is 1 - input pixel (floor(x * in_w / out_w), floor(y * in_h / out_h)).

layout(push_constant) uniform Push
{
    vec4 SourceSize;
    vec4 OutputSize;
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
    const vec2 out_pixel = floor(vTexCoord * params.OutputSize.xy);
    const vec2 p = floor(out_pixel * params.SourceSize.xy / params.OutputSize.xy);
    FragColor = vec4(1.0 - texture(Source, (p + 0.5) * params.SourceSize.zw).rgb, 1.0);
}
