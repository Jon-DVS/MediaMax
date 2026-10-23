#version 450

// Layer compositing fragment shader.
//
// Implements the exact equations of the CPU reference (core/src/blend/
// Blend.cpp): all compositing happens in premultiplied alpha space. The
// scene render target stores premultiplied color; the final display pass
// unpremultiplies. The two implementations must stay in sync.
//
// Compositing is done per layer into ping-pong render targets (no fixed-
// function blending): this pass reads the accumulated scene (previous RT)
// and writes the updated scene (next RT).

layout(push_constant) uniform LayerPush
{
    vec2 sceneSize;
    float opacity;
    int blendMode;
    vec4 layerColor;
    int useColor;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_sceneUV;

layout(location = 0) out vec4 fragColor;

// NVRHI binds textures and samplers as separate descriptors (SAMPLED_IMAGE
// + SAMPLER). The matching GLSL form is a samplerless texture2D plus a pure
// sampler, combined at each use with the GLSL 4.00 sampler2D constructor:
// it lowers to OpSampledImage, the Vulkan separate image/sampler form.
// (The image2D + 3-arg texture() form is not accepted by glslang in Vulkan
// mode.)
layout(set = 0, binding = 0) uniform highp texture2D mediaTex;
layout(set = 0, binding = 1) uniform highp texture2D sceneTex;
layout(set = 0, binding = 2) uniform sampler linearSampler;

void main()
{
    // Source layer color (straight alpha). Color layers use their constant
    // color instead of a texture sample.
    vec4 src = (pc.useColor != 0) ? pc.layerColor : texture(sampler2D(mediaTex, linearSampler), v_uv);

    // Opacity scales the layer's alpha before compositing.
    float sa = clamp(src.a * pc.opacity, 0.0, 1.0);
    vec4 s_pm = vec4(clamp(src.rgb, 0.0, 1.0) * sa, sa);

    // Destination: the accumulated scene (already premultiplied).
    vec4 d_pm = texture(sampler2D(sceneTex, linearSampler), v_sceneUV);
    float da = clamp(d_pm.a, 0.0, 1.0);

    vec3 out_pm;
    if (pc.blendMode == 0) // Normal
    {
        out_pm = s_pm.rgb + d_pm.rgb * (1.0 - sa);
    }
    else if (pc.blendMode == 1) // Add
    {
        out_pm = s_pm.rgb + d_pm.rgb;
    }
    else if (pc.blendMode == 2) // Subtract
    {
        out_pm = max(vec3(0.0), d_pm.rgb - s_pm.rgb);
    }
    else if (pc.blendMode == 3) // Multiply
    {
        out_pm = d_pm.rgb * clamp(src.rgb, 0.0, 1.0) + d_pm.rgb * (1.0 - sa);
    }
    else // Screen
    {
        out_pm = s_pm.rgb * (1.0 - d_pm.rgb) + d_pm.rgb;
    }

    // Alpha: standard "over" (always sa + da * (1 - sa), all modes).
    float a = clamp(sa + da * (1.0 - sa), 0.0, 1.0);
    fragColor = vec4(out_pm, a);
}
