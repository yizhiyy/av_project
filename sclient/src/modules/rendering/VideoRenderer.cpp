#include "modules/rendering/VideoRenderer.h"

#include <utility>

#include "modules/rendering/VideoRendererBackend.h"
#include "modules/rendering/opengl/OpenGlVideoRendererBackend.h"

namespace sclient {

namespace {

// 后端工厂：上层只依赖 VideoRendererBackend 接口，具体的 OpenGL 实现
// 在这里创建。将来增加 Vulkan/软件渲染器时，只需扩展这个分发点。
std::unique_ptr<VideoRendererBackend> CreateVideoRendererBackend(
        RenderBackend requested_backend,
        std::string *error_message) {
    switch (requested_backend) {
        case RenderBackend::kAuto:
        case RenderBackend::kOpenGl:
            return CreateOpenGlVideoRendererBackend();
        default:
            if (error_message != nullptr) {
                *error_message = "requested renderer backend is not supported";
            }
            return nullptr;
    }
}

}  // namespace

struct VideoRendererImpl {
    // Pimpl 将 OpenGL/GLFW 类型隔离在 cpp 文件中，公共头文件不需要引入
    // 图形库头文件，也能保持 ABI 相对稳定。
    RenderBackend requested_backend = RenderBackend::kAuto;
    RenderBackend active_backend = RenderBackend::kOpenGl;
    std::string active_backend_name = "opengl";
    std::unique_ptr<VideoRendererBackend> backend;
};

VideoRenderer::VideoRenderer()
        : impl_(std::make_unique<VideoRendererImpl>()) {
}

VideoRenderer::~VideoRenderer() {
    // 析构时统一释放后端持有的窗口、OpenGL 对象和 ImGui 资源。
    Shutdown();
}

bool VideoRenderer::Initialize(
        const std::string &window_title,
        RenderBackend requested_backend,
        bool enable_vsync,
        std::string *error_message,
        std::string *info_message) {
    // 允许同一个 VideoRenderer 对象重复初始化：先清理旧后端，避免资源泄漏。
    Shutdown();

    impl_->requested_backend = requested_backend;
    impl_->backend = CreateVideoRendererBackend(requested_backend, error_message);
    if (!impl_->backend) {
        return false;
    }

    // 工厂只负责创建对象，真正的窗口/GL 上下文初始化由后端完成。
    if (!impl_->backend->Initialize(window_title, enable_vsync, error_message)) {
        impl_->backend.reset();
        return false;
    }

    // 记录后端最终选择结果（auto 也会在这里落为具体后端）。
    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();

    if (info_message != nullptr) {
        *info_message = std::string("renderer=") +
                (impl_->active_backend_name == "opengl" ? "opengl(glfw+glad+imgui)" : impl_->active_backend_name) +
                " vsync=" + (enable_vsync ? "on" : "off");
    }
    return true;
}

int VideoRenderer::PollKey(int delay_ms) const {
    if (!impl_->backend) {
        return 27;
    }
    return impl_->backend->PollKey(delay_ms);
}

RenderBackend VideoRenderer::backend() const {
    return impl_->active_backend;
}

const std::string &VideoRenderer::backend_name() const {
    return impl_->active_backend_name;
}

bool VideoRenderer::SupportsNativeFrame(const DecodedFrame &frame) const {
    // 渲染前可由调用方检查像素格式，避免把不支持的帧送入 OpenGL 路径。
    return impl_->backend != nullptr && impl_->backend->SupportsNativeFrame(frame);
}

bool VideoRenderer::Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }
    // VideoRenderer 本身不参与绘制，只做后端生命周期和接口转发。
    return impl_->backend->Render(frame, frame_info, error_message);
}

void VideoRenderer::UpdateWindowTitle(const std::string &title) {
    if (impl_->backend) {
        impl_->backend->UpdateWindowTitle(title);
    }
}

void VideoRenderer::ToggleFullscreen() {
    if (impl_->backend) {
        impl_->backend->ToggleFullscreen();
    }
}

bool VideoRenderer::SaveScreenshot(const std::string &path, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }
    return impl_->backend->SaveScreenshot(path, error_message);
}

void VideoRenderer::Shutdown() {
    if (impl_->backend) {
        // 先让后端按 OpenGL/ImGui 的依赖顺序释放资源，再销毁 C++ 对象。
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = RenderBackend::kAuto;
    impl_->active_backend = RenderBackend::kOpenGl;
    impl_->active_backend_name = "opengl";
}

}  // namespace sclient
