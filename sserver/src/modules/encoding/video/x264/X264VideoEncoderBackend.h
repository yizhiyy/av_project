// X264VideoEncoderBackend.h
// x264 软件编码后端头文件。
// 使用 libx264 将 YUYV422 原始画面编码为 H.264 码流，
// 编码前先把 YUYV422 转换为 x264 所需的 I420 平面格式。

#ifndef SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
#define SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H

#include <memory>
#include <string>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "x264.h"

namespace sserver {
namespace modules {
namespace encoding {

// x264 后端工厂函数：创建 X264VideoEncoderBackend 实例。
std::unique_ptr<VideoEncoderBackend> CreateX264VideoEncoderBackend();

// X264VideoEncoderBackend：VideoEncoderBackend 的 x264 实现。
class X264VideoEncoderBackend final : public VideoEncoderBackend {
public:
    X264VideoEncoderBackend();
    ~X264VideoEncoderBackend() override;

    bool Initialize(int width, int height, int fps, const config::CodecConfig &config, std::string *error_message) override;
    bool EncodeYuyv422Frame(
            const std::uint8_t *input,
            std::size_t input_length,
            std::vector<std::uint8_t> *output,
            bool *is_keyframe,
            std::string *error_message) override;
    void Shutdown() override;
    EncodeBackend backend() const override;
    const std::string &backend_name() const override;

private:
    std::string backend_name_;  // 后端名称（"x264"）
    x264_param_t *param_;       // x264 参数对象（malloc 分配）
    x264_t *handle_;            // x264 编码器句柄
    x264_picture_t *picture_;   // 输入画面对象（含 I420 平面）
    x264_nal_t *nal_;           // 编码输出的 NAL 单元数组（由 x264 内部管理）
    int pts_;                   // 显示时间戳序号，逐帧自增
};

}  // namespace encoding
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_ENCODING_VIDEO_X264_X264VIDEOENCODERBACKEND_H
