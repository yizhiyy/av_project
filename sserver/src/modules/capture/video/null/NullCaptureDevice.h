// NullCaptureDevice.h
// 空采集设备（Null Capture Device）头文件。
// 该设备不依赖真实摄像头硬件，用于在无硬件环境下进行测试、调试和基准测试：
//   1) 默认模式：直接生成携带 "null-frame-N" 文本的模拟编码帧；
//   2) h264_test_pattern 模式：生成随时间变化的合成 YUYV 测试画面，
//      再经过视频编码器编码为 H.264 帧。

#ifndef SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {

// NullCaptureDevice：ICaptureDevice 的“空实现”/虚拟采集设备。
// 通过模拟帧来验证“采集 → 编码 → 传输”的完整链路，
// 同时也可用于压力测试与延迟测量，无需真实视频源。
class NullCaptureDevice : public ICaptureDevice {
public:
    // 构造函数：保存采集配置与编码器配置，并初始化设备状态。
    // config      - 采集配置（分辨率、帧率、帧间隔、空载荷模式等）
    // codec_config - 编码器配置（编码格式、码率、关键帧间隔等）
    NullCaptureDevice(const config::CaptureConfig &config, const config::CodecConfig &codec_config);

    // 打开设备：空设备无真实硬件，仅标记为已打开。
    bool Open() override;

    // 启动采集：校验设备已打开；若配置为 h264_test_pattern，
    // 则初始化视频编码器并分配合成测试画面缓冲区，最后进入推流状态。
    bool Start() override;

    // 采集并生成一帧：按帧间隔休眠以模拟真实采集节奏，
    // 随后生成一帧编码数据（文本帧或 H.264 编码帧）并填充元信息。
    common::model::EncodedFramePtr CaptureFrame() override;

    // 停止采集：退出推流状态并释放编码器资源。
    void Stop() override;

    // 关闭设备：将设备标记为未打开。
    void Close() override;

    // 返回设备的描述字符串，用于日志或状态展示。
    std::string Describe() const override;

private:
    // 填充一帧合成的 YUYV 测试画面。
    // 画面内容随帧序号变化，方便肉眼/程序验证帧确实在持续更新。
    void FillSyntheticYuyvFrame();

    config::CaptureConfig config_;                    // 采集配置（分辨率、帧率、帧间隔、空载荷模式等）
    config::CodecConfig codec_config_;                // 编码器配置（编码格式、码率、关键帧间隔等）
    bool opened_;                                     // 设备是否已打开
    bool streaming_;                                  // 是否正在推流
    std::uint64_t sequence_;                          // 帧序号，每采集一帧自增一次
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;  // 可选视频编码器，仅 h264_test_pattern 模式使用
    std::vector<std::uint8_t> synthetic_yuyv_frame_;  // 合成 YUYV 测试帧的缓冲区（YUV422 格式）
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
