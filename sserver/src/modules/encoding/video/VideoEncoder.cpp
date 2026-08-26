#include "modules/encoding/video/VideoEncoder.h"

#include <utility>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "modules/encoding/video/x264/X264VideoEncoderBackend.h"

namespace sserver {
namespace modules {
namespace encoding {

// VideoEncoder 的内部实现（Pimpl）。
struct VideoEncoder::VideoEncoderImpl {
    EncodeBackend requested_backend = EncodeBackend::kAuto;   // 配置请求的后端
    std::string requested_backend_name = "auto";               // 请求后端名称
    EncodeBackend active_backend = EncodeBackend::kX264;       // 实际启用的后端
    std::string active_backend_name = "x264";                  // 实际启用后端名称
    std::unique_ptr<VideoEncoderBackend> backend;              // 具体编码后端实例
};

// 编码后端工厂：按解析结果创建后端实例。
std::unique_ptr<VideoEncoderBackend> VideoEncoderBackendFactory::Create(
        const VideoEncoderBackendSelection &selection,
        std::string *error_message) {
    switch (selection.backend) {
        case EncodeBackend::kX264:
            // x264 软件编码后端。
            return CreateX264VideoEncoderBackend();
        case EncodeBackend::kAuto:
        default:
            // kAuto 表示尚未解析，正常流程不应到达这里。
            if (error_message != nullptr) {
                *error_message = "requested encode backend is not supported";
            }
            return nullptr;
    }
}

// 构造函数：创建内部实现对象。
VideoEncoder::VideoEncoder()
        : impl_(std::make_unique<VideoEncoderImpl>()) {
}

// 析构函数：确保编码器资源被释放。
VideoEncoder::~VideoEncoder() {
    Shutdown();
}

// 无错误信息版本的初始化，直接委托给带错误信息的重载。
bool VideoEncoder::Initialize(int width, int height, int fps, const config::CodecConfig &config) {
    return Initialize(width, height, fps, config, nullptr);
}

// 初始化编码器：先清理旧实例，再解析后端、创建并初始化后端。
bool VideoEncoder::Initialize(
        int width,
        int height,
        int fps,
        const config::CodecConfig &config,
        std::string *error_message) {
    // 支持重复初始化。
    Shutdown();

    // 从配置解析编码后端。
    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, error_message)) {
        return false;
    }

    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;

    // 创建具体后端实例。
    impl_->backend = VideoEncoderBackendFactory::Create(selection, error_message);
    if (!impl_->backend) {
        return false;
    }

    // 用画面参数与编码配置初始化后端；失败时释放实例。
    if (!impl_->backend->Initialize(width, height, fps, config, error_message)) {
        impl_->backend.reset();
        return false;
    }

    // 后端初始化成功后才更新实际后端信息。
    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();
    return true;
}

// 无错误信息版本的编码入口。
bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe) {
    return EncodeYuyv422Frame(input, input_length, output, is_keyframe, nullptr);
}

// 编码一帧 YUYV422 图像：先校验已初始化，再委托给后端。
bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe,
        std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "video encoder is not initialized";
        }
        return false;
    }

    return impl_->backend->EncodeYuyv422Frame(input, input_length, output, is_keyframe, error_message);
}

// 关闭编码器：释放后端并重置状态，保证可再次初始化。
void VideoEncoder::Shutdown() {
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = EncodeBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = EncodeBackend::kX264;
    impl_->active_backend_name = "x264";
}

// 返回实际编码后端。
EncodeBackend VideoEncoder::backend() const {
    return impl_->active_backend;
}

// 返回实际编码后端名称。
const std::string &VideoEncoder::backend_name() const {
    return impl_->active_backend_name;
}

// 工厂：仅校验配置中的后端是否受支持，然后创建 VideoEncoder 实例。
std::unique_ptr<IVideoEncoder> CodecFactory::Create(const config::CodecConfig &config) {
    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, nullptr)) {
        return std::unique_ptr<IVideoEncoder>();
    }
    return std::unique_ptr<IVideoEncoder>(new VideoEncoder());
}

}  // namespace encoding
}  // namespace modules
}  // namespace sserver
