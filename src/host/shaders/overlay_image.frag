#version 450
// A plugin's RGBA image (host_overlay_image_set), drawn over the finished frame.
// Same vertex stage as the overlay (overlay.vert); the vertex colour tints it
// and its alpha fades it.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 0) out vec4 out_colour;
void main() {
    out_colour = texture(img, v_uv) * v_colour;
}
