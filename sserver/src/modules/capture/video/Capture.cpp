#include "modules/capture/video/Capture.h"

#include <utility>

#include "common/log/Logger.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/capture/video/null/NullCaptureDevice.h"
#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// Capture 的内部实现（Pimpl）。
// 集中保存采集/编码配置、后端选择结果、设备实例与模块状态。
struct Capture::CaptureImpl {
    config::CaptureConfig capture_config;       // 采集配置（来源、分辨率、帧率等）
    config::CodecConfig codec_config;           // 编码器配置
    CaptureBackend requested_backend = CaptureBackend::kAuto;   // 请求（配置解析）的后端
    std::string requested_backend_name = "auto";                 // 请求后端名称
    CaptureBackend active_backend = CaptureBackend::kNull;       // 实际启用的后端
    std::string active_backend_name = "null";                    // 实际启用后端名称
    std::unique_ptr<ICaptureDevice> device;                      // 具体采集设备实例
    core::ModuleState state = core::ModuleState::kCreated;       // 模块生命周期状态
};

// 后端工厂：按解析结果创建对应设备实例。
std::unique_ptr<ICaptureDevice> CaptureBackendFactory::Create(
        const CaptureBackendSelection &selection,
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config,
        std::string *error_message) {
    switch (selection.backend) {
        case CaptureBackend::kNull:
            // 空设备：无需硬件，直接构造。
            return std::unique_ptr<ICaptureDevice>(new NullCaptureDevice(capture_config, codec_config));
        case CaptureBackend::kV4L2:
            // V4L2 设备：基于 Linux 摄像头设备采集。
            return std::unique_ptr<ICaptureDevice>(new v4l2::V4L2CaptureDevice(capture_config, codec_config));
        case CaptureBackend::kAuto:
        default:
            // kAuto 表示尚未解析，这里不应出现；其余未知类型一律拒绝。
            if (error_message != nullptr) {
                *error_message = "requested capture backend is not supported";
            }
            return nullptr;
    }
}

// 构造函数：创建内部实现对象。
Capture::Capture()
        : impl_(std::make_unique<CaptureImpl>()) {
}

// 析构函数：确保资源被释放（幂等）。
Capture::~Capture() {
    shutdown();
}

// 初始化：先清理旧状态，再解析后端并创建设备实例。
bool Capture::initialize(const core::ApplicationContext &context, std::string *error_message) {
    // 支持重复初始化：先关闭已有设备。
    shutdown();

    // 根据配置解析后端类型。
    CaptureBackendSelection selection;
    if (!ResolveCaptureBackendSelection(context.config.capture, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 保存配置与请求的后端信息。
    impl_->capture_config = context.config.capture;
    impl_->codec_config = context.config.codec;
    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;

    // 通过工厂创建设备实例。
    impl_->device = CaptureBackendFactory::Create(selection, context.config.capture, context.config.codec, error_message);
    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 设备创建成功后才更新“实际后端”。
    impl_->active_backend = selection.backend;
    impl_->active_backend_name = selection.backend_name;
    impl_->state = core::ModuleState::kInitialized;
    return true;
}

// 启动采集：打开设备并开始推流。
bool Capture::start(std::string *error_message) {
    // 采集功能未启用时直接进入 Running，让上层流程照常继续。
    if (!impl_->capture_config.enabled) {
        impl_->state = core::ModuleState::kRunning;
        return true;
    }

    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "capture is not initialized";
        }
        return false;
    }

    // 打开底层设备（如 V4L2 的设备文件）。
    if (!impl_->device->Open()) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to open capture device";
        }
        return false;
    }

    // 启动数据流（分配缓冲区、开始出帧）。
    if (!impl_->device->Start()) {
        impl_->device->Close();
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to start capture device";
        }
        return false;
    }

    impl_->state = core::ModuleState::kRunning;
    return true;
}

// 停止采集：停止推流并关闭设备，保留设备实例以便再次启动。
void Capture::stop() {
    if (!impl_->device) {
        impl_->state = core::ModuleState::kStopped;
        return;
    }

    impl_->device->Stop();
    impl_->device->Close();
    impl_->state = core::ModuleState::kStopped;
}

// 完全关闭：停止设备并释放实例，重置全部状态。
void Capture::shutdown() {
    if (impl_->device) {
        impl_->device->Stop();
        impl_->device->Close();
        impl_->device.reset();
    }

    // 重置后端选择与状态，保证对象可再次 initialize。
    impl_->requested_backend = CaptureBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = CaptureBackend::kNull;
    impl_->active_backend_name = "null";
    impl_->state = core::ModuleState::kShutdown;
}

// 返回当前模块状态。
core::ModuleState Capture::state() const {
    return impl_->state;
}

// 采集一帧已编码数据；设备不存在或未运行时返回空帧。
common::model::EncodedFramePtr Capture::CaptureFrame() {
    if (!impl_->device) {
        return common::model::EncodedFramePtr();
    }
    return impl_->device->CaptureFrame();
}

// 返回实际启用的后端类型。
CaptureBackend Capture::backend() const {
    return impl_->active_backend;
}

// 返回实际启用后端名称。
const std::string &Capture::backend_name() const {
    return impl_->active_backend_name;
}

// 返回设备描述；未初始化时给出明确标识。
std::string Capture::Describe() const {
    if (!impl_->device) {
        return "capture(uninitialized)";
    }
    return impl_->device->Describe();
}

// 底层设备是否支持原始帧采集。
bool Capture::SupportsRawCapture() const {
    return impl_->device && impl_->device->SupportsRawCapture();
}

// 采集一帧原始数据；设备不存在时返回空指针。
RawCaptureFramePtr Capture::CaptureRawFrame() {
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->CaptureRawFrame();
}

// 编码一帧原始数据；设备不存在时返回空指针。
common::model::EncodedFramePtr Capture::EncodeRawFrame(RawCaptureFramePtr raw) {
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->EncodeRawFrame(raw);
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
