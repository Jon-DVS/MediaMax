#version 450

// Layer compositing fragment shader.
//
// Implements the exact equations of the CPU reference (core/src/blend/
// Blend.cpp): all compositing happens in premultiplied alpha space. The
// scene render target stores premultiplied color; the final display pass
// unpremultiplies. The two implementations must stay in sync.
//
// YUV 4:2:0 media is converted YUV -> RGB here, on the GPU, instead of on
// the decode thread: the shader implements the exact float32 equations of
// the CPU reference (core/src/media/VideoConversion.cpp) - same constants,
// same order of operations, same clamps. Like the compositing math above,
// the two implementations must stay in sync.

layout(push_constant) uniform LayerPush
{
    vec2 sceneSize;
    float opacity;
    int blendMode;
    vec4 layerColor;
    int useColor;
    float mediaSizeX; // YUV: the media (luma) texture size in pixels
    float mediaSizeY;
    int mediaFormat; // mediamax::VideoPixelFormat ordinal (0 = RGBA)
    int colorSpace; // mediamax::YuvColorSpace ordinal (0 = BT601, 1 = BT709, 2 = BT2020)
    int fullRange; // 0 = limited (video), 1 = full (JPEG)
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
layout(set = 0, binding = 0) uniform highp texture2D mediaTex; // RGBA, or the YUV luma plane
layout(set = 0, binding = 1) uniform highp texture2D sceneTex;
layout(set = 0, binding = 2) uniform sampler linearSampler;
layout(set = 0, binding = 3) uniform highp texture2D uPlane; // YUV U/Cb (null for RGBA)
layout(set = 0, binding = 4) uniform highp texture2D vPlane; // YUV V/Cr (null for RGBA)

// The YUV -> RGB conversion (the exact float32 form of
// VideoConversion::yuvToRgb): range normalization (limited: luma 16-235,
// chroma 64-240, i.e. centered at 128 and scaled by 255/224; full: 0-255,
// chroma centered at 0.5), then the matrix R = Y' + a*Cr', G = Y' - b*Cb' -
// c*Cr', B = Y' + d*Cb' with per-space coefficients, clamped to [0, 1].
vec3 yuvToRgb(float y, float cb, float cr, int colorSpace, int fullRange)
{
    float yn, cbn, crn;
    if (fullRange != 0)
    {
        yn = y;
        cbn = cb - 0.5;
        crn = cr - 0.5;
    }
    else
    {
        yn = (y - 16.0 / 255.0) * (255.0 / 219.0);
        cbn = (cb - 128.0 / 255.0) * (255.0 / 224.0);
        crn = (cr - 128.0 / 255.0) * (255.0 / 224.0);
    }
    float a, b, c, d;
    if (colorSpace == 0) // BT601
    {
        a = 1.402;
        b = 0.34414;
        c = 0.71414;
        d = 1.772;
    }
    else if (colorSpace == 2) // BT2020
    {
        a = 1.8814;
        b = 0.1197;
        c = 0.6812;
        d = 2.1145;
    }
    else // BT709 (1), and the Unknown (3) -> BT709 default
    {
        a = 1.5748;
        b = 0.1873;
        c = 0.4681;
        d = 1.8556;
    }
    return clamp(vec3(yn + a * crn, yn - b * cbn - c * crn, yn + d * cbn), 0.0, 1.0);
}

void main()
{
    // Source layer color (straight alpha). Color layers use their constant
    // color; YUV 4:2:0 media is converted in-shader from its three planes
    // (the luma is sampled at the media UV, the U/V planes at the co-sited
    // chroma coordinate: luma pixel (x, y) reads the chroma texel at
    // ((x + 1) / w, (y + 1) / h), which is v_uv + 0.5 / mediaSize for the
    // pixel-center UV - the linear sampler then hits the U/V texel center
    // for even luma positions and blends 50/50 for odd ones, exactly like
    // the CPU reference's co-sited bilinear).
    vec4 src;
    if (pc.useColor != 0)
    {
        src = pc.layerColor;
    }
    else if (pc.mediaFormat != 0)
    {
        const float y = texture(sampler2D(mediaTex, linearSampler), v_uv).x;
        const vec2 chromaUV = v_uv + vec2(0.5) / vec2(pc.mediaSizeX, pc.mediaSizeY);
        const float cb = texture(sampler2D(uPlane, linearSampler), chromaUV).x;
        const float cr = texture(sampler2D(vPlane, linearSampler), chromaUV).x;
        src = vec4(yuvToRgb(y, cb, cr, pc.colorSpace, pc.fullRange), 1.0);
    }
    else
    {
        src = texture(sampler2D(mediaTex, linearSampler), v_uv);
    }

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
