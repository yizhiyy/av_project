#version 330 core

// 顶点位置和纹理坐标分别来自 OpenGL 的 VAO/VBO。
layout (location = 0) in vec2 a_position;
layout (location = 1) in vec2 a_tex_coord;

// 为保持视频宽高比，CPU 会按窗口/图像比例计算缩放因子。
uniform vec2 u_scale;

// 传递给片段着色器，用于从各个 YUV 纹理采样。
out vec2 v_tex_coord;

void main() {
    // 顶点位于标准化设备坐标 [-1, 1]；缩放后的视频区域居中显示。
    gl_Position = vec4(a_position * u_scale, 0.0, 1.0);
    v_tex_coord = a_tex_coord;
}
