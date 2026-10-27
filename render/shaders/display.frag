#version 450

// Display pass fragment shader: samples the composited scene for one
// display, applies the projection warp (dense RG8 warp texture, see
// WarpMap::buildDenseTexture) and the display brightness, then
// unpremultiplies for output. When useOverlay is set, a status overlay
// (white text, straight alpha - see core overlay/StatusOverlay) is blended
// screen-aligned on top (no warp: it is display UI, not scene content).

layout(push_constant) uniform DisplayPush
{
    vec4 displayRectPx;
    vec2 sceneSize;
    float brightness;
    int useWarp;
    int useOverlay;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

// NVRHI binds textures and samplers as separate descriptors (SAMPLED_IMAGE
// + SAMPLER); see layer.frag for the matching GLSL form.
layout(set = 0, binding = 0) uniform highp texture2D sceneTex;
layout(set = 0, binding = 1) uniform highp texture2D warpTex;
layout(set = 0, binding = 2) uniform highp texture2D overlayTex;
layout(set = 0, binding = 3) uniform sampler linearSampler;

void main()
{
    // Scene UV of this fragment without warp.
    vec2 sceneUV = (pc.displayRectPx.xy + v_uv * pc.displayRectPx.zw) / pc.sceneSize;

    if (pc.useWarp != 0)
    {
        // The dense warp texture stores the warped display-local UV per
        // texel; remap it into scene space with the display's rectangle.
        vec2 warpedLocal = texture(sampler2D(warpTex, linearSampler), v_uv).rg;
        sceneUV = (pc.displayRectPx.xy + warpedLocal * pc.displayRectPx.zw) / pc.sceneSize;
    }

    vec4 pm = texture(sampler2D(sceneTex, linearSampler), sceneUV);

    // Unpremultiply, apply brightness, clamp. The scene is fully opaque
    // (it is cleared with alpha 1), so the output alpha is always 1.
    float a = max(pm.a, 1e-6);
    vec3 rgb = clamp(pm.rgb / a * pc.brightness, 0.0, 1.0);

    // The overlay is screen-aligned (display-local UV = v_uv), straight
    // alpha: standard src-over on the unpremultiplied scene color.
    if (pc.useOverlay != 0)
    {
        vec4 ov = texture(sampler2D(overlayTex, linearSampler), v_uv);
        rgb = ov.rgb * ov.a + rgb * (1.0 - ov.a);
    }

    fragColor = vec4(rgb, 1.0);
}
