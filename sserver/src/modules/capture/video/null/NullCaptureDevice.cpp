#include "modules/capture/video/null/NullCaptureDevice.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {

// 构造函数：保存采集/编码配置，并将设备初始化为“未打开、未推流、序号从 0 开始”的状态。
NullCaptureDevice::NullCaptureDevice(
        const config::CaptureConfig &config,
        const config::CodecConfig &codec_config)
        : config_(config),
          codec_config_(codec_config),
          opened_(false),
          streaming_(false),
          sequence_(0) {
}

// 打开设备：空设备没有真实硬件需要初始化，直接标记为已打开并返回成功。
bool NullCaptureDevice::Open() {
    opened_ = true;
    return true;
}

// 启动采集：
// 1. 设备必须先打开；
// 2. 若配置为 h264_test_pattern，则创建并初始化 H.264 编码器，
//    同时按 宽×高×2 字节（YUYV 每个像素占 2 字节）分配合成画面缓冲区；
// 3. 全部就绪后进入推流状态。
bool NullCaptureDevice::Start() {
    if (!opened_) {
        return false;
    }

    if (config_.null_payload_mode == "h264_test_pattern") {
        // 创建视频编码器，并按采集配置（分辨率、帧率、编码参数）初始化。
        encoder_ = std::make_unique<modules::encoding::VideoEncoder>();
        std::string error_message;
        if (!encoder_->Initialize(
                    config_.width,
                    config_.height,
                    config_.fps,
                    codec_config_,
                    &error_message)) {
            // 编码器初始化失败时释放资源并返回失败。
            encoder_.reset();
            return false;
        }
        // 分配 YUYV 合成画面的缓冲区，初始全部填 0。
        synthetic_yuyv_frame_.assign(static_cast<std::size_t>(config_.width * config_.height * 2), 0);
    }

    // 进入推流状态，之后 CaptureFrame() 才允许产出帧。
    streaming_ = true;
    return true;
}

// 采集一帧并返回：
// 1. 未处于推流状态时返回空帧；
// 2. 按配置的帧间隔休眠，模拟真实摄像头的采集节奏；
// 3. 记录采集时刻（单调时钟，纳秒）；
// 4. 按空载荷模式生成帧内容：
//    - h264_test_pattern：填充合成 YUYV 画面并用编码器编码为 H.264；
//    - 其他模式：直接生成 "null-frame-N" 文本帧，必要时按配置补齐到指定字节数。
common::model::EncodedFramePtr NullCaptureDevice::CaptureFrame() {
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    // 模拟真实采集的帧间隔，避免空设备无限高速产帧。
    std::this_thread::sleep_for(std::chrono::milliseconds(config_.frame_interval_ms));
    // 记录“采集”时刻，用于端到端延迟统计。
    const std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();

    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;                 // 帧序号自增，用于接收端排序/丢帧检测
    frame->type = common::model::StreamPayloadType::kVideo;  // 声明这是视频流
    frame->capture_timestamp_ns = capture_timestamp_ns;      // 采集时间戳

    if (config_.null_payload_mode == "h264_test_pattern" && encoder_ != nullptr) {
        // 模式一：编码合成测试画面为 H.264 帧。
        FillSyntheticYuyvFrame();
        frame->encode_start_timestamp_ns = common::time::MonotonicNowNs();
        if (!encoder_->EncodeYuyv422Frame(
                    synthetic_yuyv_frame_.data(),
                    synthetic_yuyv_frame_.size(),
                    &frame->payload,
                    &frame->is_keyframe)) {
            // 编码失败时返回空帧，由上层决定是否重试或停止。
            return common::model::EncodedFramePtr();
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();
    } else {
        // 模式二：生成文本形式的模拟帧。
        frame->encode_start_timestamp_ns = capture_timestamp_ns;
        frame->is_keyframe = true;  // 文本帧始终视为关键帧，接收端可直接解码/显示

        // 帧内容形如 "null-frame-0"、"null-frame-1"……便于肉眼核对序号。
        std::ostringstream stream;
        stream << "null-frame-" << frame->sequence;
        const std::string text = stream.str();
        frame->payload.assign(text.begin(), text.end());

        // 若配置要求更长的载荷，则用周期变化的字母填充，以便测试带宽与负载。
        if (config_.null_payload_bytes > frame->payload.size()) {
            frame->payload.resize(config_.null_payload_bytes, static_cast<std::uint8_t>('A' + (frame->sequence % 26)));
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();
    }

    return frame;
}

// 停止采集：退出推流状态；若编码器存在则先优雅关闭再释放。
void NullCaptureDevice::Stop() {
    streaming_ = false;
    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }
}

// 关闭设备：仅更新状态标记；合成缓冲区由成员析构时自动释放。
void NullCaptureDevice::Close() {
    opened_ = false;
}

// 返回设备类型描述，用于日志、配置校验或状态面板展示。
std::string NullCaptureDevice::Describe() const {
    return "null-capture-device";
}

// 填充一帧合成的 YUYV 测试画面：
// - 画面按 YUYV 格式排列（每个像素 2 字节：Y0 U Y1 V，两个像素共享一组 U/V）；
// - 亮度 Y 与色度 U/V 都包含随行、列、帧序号变化的偏移，
//   因此每帧画面都在变化，便于确认采集/编码/显示链路确实在持续工作。
void NullCaptureDevice::FillSyntheticYuyvFrame() {
    if (synthetic_yuyv_frame_.empty()) {
        return;
    }

    const int width = config_.width;
    const int height = config_.height;
    // 帧序号乘以 7 后取 0~255，作为本帧的全局相位偏移，使画面逐帧变化。
    const std::uint8_t phase = static_cast<std::uint8_t>((sequence_ * 7) % 256);

    // YUYV 中每 2 个像素共享一对 U/V，因此列步长为 2。
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; column += 2) {
            // 当前像素对在缓冲区中的起始偏移（每像素 2 字节）。
            const std::size_t offset = static_cast<std::size_t>(row * width + column) * 2;
            // 两个亮度样本：让它们随位置与相位变化，形成移动的灰度图案。
            const std::uint8_t y0 = static_cast<std::uint8_t>((row + column + phase) % 256);
            const std::uint8_t y1 = static_cast<std::uint8_t>((row + column + 32 + phase) % 256);
            // 色度样本：U 随行变化，V 随列变化，再叠加相位，生成彩色测试图案。
            const std::uint8_t u = static_cast<std::uint8_t>((128 + row / 2 + phase) % 256);
            const std::uint8_t v = static_cast<std::uint8_t>((64 + column / 2 + phase) % 256);
            // 按 YUYV 布局写入：Y0 U Y1 V。
            synthetic_yuyv_frame_[offset + 0] = y0;
            synthetic_yuyv_frame_[offset + 1] = u;
            synthetic_yuyv_frame_[offset + 2] = y1;
            synthetic_yuyv_frame_[offset + 3] = v;
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
