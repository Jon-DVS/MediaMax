#version 450

// Display pass vertex shader: maps a full-window quad (display-local pixels)
// into clip space.

layout(push_constant) uniform DisplayPush
{
    vec4 displayRectPx; // x, y, w, h of the display in output space
    vec2 sceneSize; // output space size in pixels
    float brightness; // output brightness multiplier
    int useWarp; // 1 => sample the warp texture
    int useOverlay; // 1 => blend the status overlay on top
    int useWarpPreview; // 1 => draw the warp grid + node markers
    int warpRows; // warp-preview lattice size (rows)
    int warpCols; // warp-preview lattice size (cols)
} pc;

layout(location = 0) in vec2 a_pos; // display-local pixels
layout(location = 1) in vec2 a_uv; // 0..1 over the display image

layout(location = 0) out vec2 v_uv;

void main()
{
    v_uv = a_uv;
    gl_Position = vec4(2.0 * a_pos.x / pc.displayRectPx.z - 1.0,
                       1.0 - 2.0 * a_pos.y / pc.displayRectPx.w,
                       0.0,
                       1.0);
}
