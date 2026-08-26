#include <glad/glad.h>

// 告诉 ImGui 使用项目已经加载好的 GLAD 函数，而不是再次选择其他 OpenGL loader。
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
// 直接编译 ImGui 官方 OpenGL3 实现，使其与 GLFW 后端配合绘制 HUD。
#include "imgui/backends/imgui_impl_opengl3.cpp"
