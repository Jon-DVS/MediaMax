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
    int useWarpPreview;
    int warpRows;
    int warpCols;
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

    // Warp preview (the control GUI's warp editor is open): a faint
    // screen-aligned reference grid at the ideal node positions plus a
    // marker at each node's CURRENT position, so the operator can see the
    // control lattice on the physical screen while adjusting the warp.
    // Screen-aligned on purpose (like the status overlay): it is display
    // UI, not scene content, so it is not warped itself. A node's current
    // position is the dense warp texture sampled at the node's ideal UV -
    // the surface passes exactly through its nodes, so that sample is the
    // node's target position (with no warp bound the node sits at its
    // ideal position).
    if (pc.useWarpPreview != 0)
    {
        const vec2 gridPx = vec2(float(pc.warpCols - 1), float(pc.warpRows - 1));
        // 1 px grid lines at the ideal node positions (anti-aliased with
        // the derivative, like ImGui's).
        vec2 cell = v_uv * gridPx;
        vec2 g = abs(fract(cell - 0.5) - 0.5) / fwidth(cell);
        float gridLine = 1.0 - min(min(g.x, g.y), 1.0);
        vec3 pvColor = vec3(1.0);
        float pvCover = gridLine * 0.4;
        // Node markers (soft dots; distances in display pixels so the
        // markers stay round at any display aspect).
        const vec2 sizePx = pc.displayRectPx.zw;
        for (int row = 0; row < 8; ++row)
        {
            if (row >= pc.warpRows)
            {
                break;
            }
            for (int col = 0; col < 8; ++col)
            {
                if (col >= pc.warpCols)
                {
                    break;
                }
                const vec2 ideal = vec2(float(col), float(row)) / gridPx;
                const vec2 pos = pc.useWarp != 0
                                     ? texture(sampler2D(warpTex, linearSampler), ideal).rg
                                     : ideal;
                const float dpx = length((v_uv - pos) * sizePx);
                const float dot = smoothstep(6.0, 4.5, dpx);
                pvColor = mix(pvColor, vec3(1.0, 0.85, 0.25), dot);
                pvCover = max(pvCover, dot);
            }
        }
        rgb = pvColor * pvCover + rgb * (1.0 - pvCover);
    }

    // The overlay is screen-aligned (display-local UV = v_uv), straight
    // alpha: standard src-over on the unpremultiplied scene color.
    if (pc.useOverlay != 0)
    {
        vec4 ov = texture(sampler2D(overlayTex, linearSampler), v_uv);
        rgb = ov.rgb * ov.a + rgb * (1.0 - ov.a);
    }

    fragColor = vec4(rgb, 1.0);
}
