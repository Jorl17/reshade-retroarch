#version 450

// Test shader for tests/e2e.py: enlarges the input to the output size with
// nearest-neighbour sampling in exact integer maths, and inverts the colours, so its
// result is the same bytes on every graphics API. Used to check that the add-on's own work
// (frame copy, capture of the native picture, write-back) is exact, apart from any
// differences between librashader's runtimes for different APIs. The inversion makes sure
// the result never equals the untouched frame, even where an exact enlargement would.
// Output pixel (x, y) is 1 - input pixel (floor(x * in_w / out_w), floor(y * in_h / out_h)),
// that is 255 - v for each 8-bit channel value v.

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
    const uvec2 out_pixel = uvec2(vTexCoord * params.OutputSize.xy);
    const uvec2 p = (out_pixel * uvec2(params.SourceSize.xy)) / uvec2(params.OutputSize.xy);
    FragColor = vec4(1.0 - texelFetch(Source, ivec2(p), 0).rgb, 1.0);
}
