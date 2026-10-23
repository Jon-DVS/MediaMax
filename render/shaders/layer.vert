#version 450

// Layer quad vertex shader.
//
// The CPU pre-transforms the media quad's corners into output-space pixel
// coordinates (see Transform2D::toOutputSpace); this shader only maps output
// space to clip space and forwards the texture coordinates.

layout(push_constant) uniform LayerPush
{
    vec2 sceneSize; // output space size in pixels
    float opacity;  // layer opacity (0..1)
    int blendMode;  // mediamax::BlendMode ordinal
    vec4 layerColor; // solid color of color layers
    int useColor;   // 1 => color layer, 0 => media texture
} pc;

layout(location = 0) in vec2 a_pos; // output-space pixels
layout(location = 1) in vec2 a_uv; // media UV (0..1)

layout(location = 0) out vec2 v_uv; // media UV
layout(location = 1) out vec2 v_sceneUV; // scene UV (0..1 over output space)

void main()
{
    v_uv = a_uv;
    v_sceneUV = a_pos / pc.sceneSize;
    gl_Position = vec4(2.0 * a_pos.x / pc.sceneSize.x - 1.0,
                       1.0 - 2.0 * a_pos.y / pc.sceneSize.y,
                       0.0,
                       1.0);
}
