// VideoEncoder.h
// 视频编码器门面（Facade）头文件。
// 对上层提供统一的视频编码接口（YUYV422 原始帧 -> H.264 编码帧），
// 具体编码实现由后端（当前为 x264）提供。

#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace encoding {

// 前置声明：具体编码后端在 .cpp 中定义。
class VideoEncoderBackend;

// 可用的编码后端类型。
enum class EncodeBackend {
    kAuto,  // 尚未解析出具体后端
    kX264,  // x264 软件编码器
};

// 编码后端解析结果：包含枚举类型与名称（用于日志展示）。
struct VideoEncoderBackendSelection {
    EncodeBackend backend = EncodeBackend::kAuto;
    std::string backend_name = "auto";
};

// 根据编码器配置中的 backend 字段解析实际使用的编码后端：
// 当前仅支持 "x264"；"libx264" 为不再支持的旧配置名。
inline bool ResolveVideoEncoderBackendSelection(
        const config::CodecConfig &config,
        VideoEncoderBackendSelection *selection,
        std::string *error_message) {
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "video encoder backend selection output is null";
        }
        return false;
    }

    // backend 字段不能为空。
    if (config.backend.empty()) {
        if (error_message != nullptr) {
            *error_message = "codec.backend must not be empty";
        }
        return false;
    }

    // 解析为 x264 后端。
    if (config.backend == "x264") {
        selection->backend = EncodeBackend::kX264;
        selection->backend_name = "x264";
        return true;
    }

    // 其余值一律拒绝；对旧配置名给出迁移提示。
    if (error_message != nullptr) {
        if (config.backend == "libx264") {
            *error_message = "codec.backend=libx264 is no longer supported; use codec.backend=x264 for software encoding";
        } else {
            *error_message = "codec.backend must be 'x264'";
        }
    }
    return false;
}

// VideoEncoderBackendFactory：按解析结果创建编码后端实例。
class VideoEncoderBackendFactory {
public:
    static std::unique_ptr<VideoEncoderBackend> Create(
            const VideoEncoderBackendSelection &selection,
            std::string *error_message);
};

// VideoEncoder：视频编码器的统一入口。
// 生命周期：Initialize -> EncodeYuyv422Frame(...) 多次 -> Shutdown。
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // 初始化编码器（无错误信息版本，失败时信息被丢弃）。
    bool Initialize(int width, int height, int fps, const config::CodecConfig &config);

    // 初始化编码器：width/height 为画面尺寸，fps 为帧率，config 为编码参数。
    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message);

    // 编码一帧 YUYV422 原始图像（无错误信息版本）。
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe);

    // 编码一帧 YUYV422 原始图像。
    // input/input_length - 原始帧数据及其字节数；
    // output             - 输出 H.264 编码码流；
    // is_keyframe        - 输出帧是否为关键帧（I 帧）。
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message);

    // 关闭编码器并释放资源（幂等，可再次 Initialize）。
    void Shutdown();

    // 当前实际使用的编码后端。
    EncodeBackend backend() const;

    // 当前实际使用的编码后端名称。
    const std::string &backend_name() const;

private:
    // Pimpl：具体实现放在 .cpp 中。
    struct VideoEncoderImpl;
    std::unique_ptr<VideoEncoderImpl> impl_;
};

// 兼容别名：早期代码中使用 IVideoEncoder 名称。
using IVideoEncoder = VideoEncoder;

// CodecFactory：根据配置创建编码器实例（不校验初始化参数）。
class CodecFactory {
public:
    static std::unique_ptr<IVideoEncoder> Create(const config::CodecConfig &config);
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODER_H
