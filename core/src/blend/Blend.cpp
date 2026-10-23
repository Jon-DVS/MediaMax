#include "mediamax/blend/Blend.h"

#include <algorithm>

namespace mediamax::blend
{

namespace
{

float clamp01(float v)
{
    return std::clamp(v, 0.f, 1.f);
}

} // namespace

// All modes compose in premultiplied alpha space (the scene render target
// stores premultiplied color), then unpremultiply for the result:
//
//   s_pm = src.rgb * src.a          d_pm = dst.rgb * dst.a
//   out.a = src.a + dst.a * (1 - src.a)          (standard "over")
//
//   Normal:   out_pm = s_pm + d_pm * (1 - src.a)
//   Add:      out_pm = s_pm + d_pm
//   Subtract: out_pm = max(0, d_pm - s_pm)
//   Multiply: out_pm = d_pm * src.rgb + d_pm * (1 - src.a)
//   Screen:   out_pm = s_pm * (1 - d_pm) + d_pm
//
// These are the exact equations the GPU layer shader implements
// (render/shaders/layer.frag), so CPU and GPU results match within rounding.

Color4 apply(const Color4& dst, const Color4& src, BlendMode mode)
{
    const float sa = clamp01(src.a);
    const float da = clamp01(dst.a);

    const float spmR = clamp01(src.r) * sa;
    const float spmG = clamp01(src.g) * sa;
    const float spmB = clamp01(src.b) * sa;
    const float dpmR = clamp01(dst.r) * da;
    const float dpmG = clamp01(dst.g) * da;
    const float dpmB = clamp01(dst.b) * da;

    float outR = 0.f, outG = 0.f, outB = 0.f;
    switch (mode)
    {
        case BlendMode::Normal:
            outR = spmR + dpmR * (1.f - sa);
            outG = spmG + dpmG * (1.f - sa);
            outB = spmB + dpmB * (1.f - sa);
            break;
        case BlendMode::Add:
            outR = spmR + dpmR;
            outG = spmG + dpmG;
            outB = spmB + dpmB;
            break;
        case BlendMode::Subtract:
            outR = dpmR - spmR;
            outG = dpmG - spmG;
            outB = dpmB - spmB;
            break;
        case BlendMode::Multiply:
            outR = dpmR * clamp01(src.r) + dpmR * (1.f - sa);
            outG = dpmG * clamp01(src.g) + dpmG * (1.f - sa);
            outB = dpmB * clamp01(src.b) + dpmB * (1.f - sa);
            break;
        case BlendMode::Screen:
            outR = spmR * (1.f - dpmR) + dpmR;
            outG = spmG * (1.f - dpmG) + dpmG;
            outB = spmB * (1.f - dpmB) + dpmB;
            break;
    }

    const float a = clamp01(sa + da * (1.f - sa));
    const Color4 out = {
        a > 0.f ? clamp01(outR / a) : 0.f,
        a > 0.f ? clamp01(outG / a) : 0.f,
        a > 0.f ? clamp01(outB / a) : 0.f,
        a,
    };
    return out;
}

std::string toString(BlendMode mode)
{
    switch (mode)
    {
        case BlendMode::Normal: return "normal";
        case BlendMode::Add: return "add";
        case BlendMode::Subtract: return "subtract";
        case BlendMode::Multiply: return "multiply";
        case BlendMode::Screen: return "screen";
    }
    return "normal";
}

BlendMode fromString(const std::string& name)
{
    if (name == "normal") return BlendMode::Normal;
    if (name == "add") return BlendMode::Add;
    if (name == "subtract") return BlendMode::Subtract;
    if (name == "multiply") return BlendMode::Multiply;
    if (name == "screen") return BlendMode::Screen;
    return BlendMode::Normal;
}

} // namespace mediamax::blend
