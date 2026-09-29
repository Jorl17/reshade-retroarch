// Placeholder for the RetroArch Shaders add-on.
//
// ReShade only runs add-ons' effect hooks when at least one effect is loaded. If
// this is your only effect file, keep it: without it the RetroArch shaders never
// run. It is hidden and never enabled, so it costs nothing.

// A triangle covering the screen. Written without bitwise operators (like ReShade's own
// PostProcessVS) so it also compiles for Direct3D 9, whose shaders have none.
float4 PlaceholderVS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PlaceholderPS(float4 pos : SV_Position) : SV_Target
{
    return 0.0; // never runs: the technique is hidden and never enabled
}

technique RetroArchShaders_Placeholder < hidden = true; ui_label = "RetroArch Shaders (placeholder)"; >
{
    pass
    {
        VertexShader = PlaceholderVS;
        PixelShader = PlaceholderPS;
    }
}
