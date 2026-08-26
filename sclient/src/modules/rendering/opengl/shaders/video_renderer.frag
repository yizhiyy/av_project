#version 330 core

// 顶点着色器传来的归一化纹理坐标。
in vec2 v_tex_coord;

// plane0/1/2 分别绑定到纹理单元 0/1/2。
// YUV420P 使用三个单通道纹理，NV12 使用 Y 单通道 + UV 双通道纹理。
uniform sampler2D u_plane0;
uniform sampler2D u_plane1;
uniform sampler2D u_plane2;
uniform int u_color_mode;

out vec4 frag_color;

vec3 yuv_to_rgb(float y, float u, float v) {
    // U/V 的纹理采样值以 0.5 为中性点，先平移到 [-0.5, 0.5] 再按
    // BT.601 风格系数转换为 RGB。
    float u_shifted = u - 0.5;
    float v_shifted = v - 0.5;
    return vec3(
        y + 1.402 * v_shifted,
        y - 0.344136 * u_shifted - 0.714136 * v_shifted,
        y + 1.772 * u_shifted
    );
}

void main() {
    if (u_color_mode == 1) {
        // YUV420P：Y、U、V 分别从三个纹理的红色通道读取。
        float y = texture(u_plane0, v_tex_coord).r;
        float u = texture(u_plane1, v_tex_coord).r;
        float v = texture(u_plane2, v_tex_coord).r;
        frag_color = vec4(yuv_to_rgb(y, u, v), 1.0);
        return;
    }

    if (u_color_mode == 2) {
        // NV12：Y 位于 plane0，交错 UV 位于 plane1 的 R/G 通道。
        float y = texture(u_plane0, v_tex_coord).r;
        vec2 uv = texture(u_plane1, v_tex_coord).rg;
        frag_color = vec4(yuv_to_rgb(y, uv.x, uv.y), 1.0);
        return;
    }

    // 兜底路径：将 plane0 当作已经是 RGB/BGR 风格的彩色纹理直接显示。
    frag_color = vec4(texture(u_plane0, v_tex_coord).rgb, 1.0);
}
