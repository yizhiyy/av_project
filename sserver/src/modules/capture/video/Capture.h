// Capture.h
// 采集模块门面（Facade）头文件。
// 负责根据配置选择具体采集后端（空设备 / V4L2），
// 并向上层提供统一的初始化、启动、采帧与关闭接口。

#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// 可用的采集后端类型。
enum class CaptureBackend {
    kAuto,  // 自动选择（尚未解析出具体后端）
    kNull,  // 空设备：生成模拟帧，用于测试/基准
    kV4L2,  // Linux V4L2 摄像头设备
};

// 采集后端解析结果：包含枚举类型及对应的名称（用于日志展示）。
struct CaptureBackendSelection {
    CaptureBackend backend = CaptureBackend::kAuto;
    std::string backend_name = "auto";
};

// 根据采集配置中的 source 字段解析实际使用的后端：
// "null"  -> NullCaptureDevice；"v4l2" -> V4L2CaptureDevice。
// 成功返回 true 并填充 selection；失败返回 false 并给出错误信息。
inline bool ResolveCaptureBackendSelection(
        const config::CaptureConfig &config,
        CaptureBackendSelection *selection,
        std::string *error_message) {
    // 输出参数为空属于调用错误，直接返回失败。
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "capture backend selection output is null";
        }
        return false;
    }

    // 空设备后端：无需真实硬件，用于测试。
    if (config.source == "null") {
        selection->backend = CaptureBackend::kNull;
        selection->backend_name = "null";
        return true;
    }

    // V4L2 后端：从 Linux 摄像头设备采集真实画面。
    if (config.source == "v4l2") {
        selection->backend = CaptureBackend::kV4L2;
        selection->backend_name = "v4l2";
        return true;
    }

    // 配置了不支持/未知的采集源。
    if (error_message != nullptr) {
        *error_message = "capture.source must be either 'v4l2' or 'null'";
    }
    return false;
}

// CaptureBackendFactory：按后端选择结果创建对应的采集设备实例。
class CaptureBackendFactory {
public:
    // 创建采集设备；失败时返回 nullptr 并填充 error_message。
    static std::unique_ptr<ICaptureDevice> Create(
            const CaptureBackendSelection &selection,
            const config::CaptureConfig &capture_config,
            const config::CodecConfig &codec_config,
            std::string *error_message);
};

// Capture：采集模块对外的统一入口。
// 内部持有具体采集设备（ICaptureDevice），负责设备生命周期管理
// 以及“采集 + 编码”或“仅采集原始帧”两种工作路径的切换。
class Capture {
public:
    Capture();
    ~Capture();

    // 初始化：解析后端并创建设备实例。
    bool initialize(const core::ApplicationContext &context, std::string *error_message);

    // 启动采集：打开并启动底层设备。
    bool start(std::string *error_message);

    // 停止采集（设备仍可重新启动）。
    void stop();

    // 关闭并释放全部资源（不可再启动）。
    void shutdown();

    // 当前模块状态。
    core::ModuleState state() const;

    // 采集一帧已编码数据（采集+编码一体化路径）。
    common::model::EncodedFramePtr CaptureFrame();

    // 当前实际使用的后端类型。
    CaptureBackend backend() const;

    // 当前实际使用的后端名称。
    const std::string &backend_name() const;

    // 设备描述字符串。
    std::string Describe() const;

    // 底层设备是否支持原始帧采集。
    bool SupportsRawCapture() const;

    // 采集一帧原始数据（双线程路径）。
    RawCaptureFramePtr CaptureRawFrame();

    // 编码一帧原始数据（双线程路径）。
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw);

private:
    // Pimpl 手法：具体实现放在 .cpp 中，隐藏内部细节、减少头文件依赖。
    struct CaptureImpl;
    std::unique_ptr<CaptureImpl> impl_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
