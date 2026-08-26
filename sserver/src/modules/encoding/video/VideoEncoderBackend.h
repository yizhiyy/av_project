// VideoEncoderBackend.h
// 视频编码后端抽象接口（Interface）。
// VideoEncoder 门面通过该接口调用具体编码实现（当前为 x264），
// 未来可以新增其他编码后端（如硬件编码器）而不影响上层代码。

#ifndef SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config/AppConfig.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace encoding {

// VideoEncoderBackend：编码后端的统一接口。
class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;

    // 初始化编码器：width/height 为画面尺寸，fps 为帧率，config 为编码参数。
    virtual bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) = 0;

    // 编码一帧 YUYV422 图像，输出 H.264 码流并标记是否为关键帧。
    virtual bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) = 0;

    // 释放编码器资源（幂等）。
    virtual void Shutdown() = 0;

    // 返回后端类型。
    virtual EncodeBackend backend() const = 0;

    // 返回后端名称（用于日志）。
    virtual const std::string &backend_name() const = 0;
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_VIDEOENCODERBACKEND_H
