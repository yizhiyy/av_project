// V4L2CaptureDevice.h
// Linux V4L2 摄像头采集设备头文件。
// 通过 Video4Linux2 内核接口从真实摄像头采集 YUYV 原始画面，
// 并支持两种工作路径：采集+编码一体化，或采集原始帧交给独立编码线程。

#ifndef SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <linux/videodev2.h>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

// MappedBuffer：一个通过 mmap 映射的 V4L2 采集缓冲区。
// start 指向驱动与用户空间共享的内存，length 为缓冲区字节数。
struct MappedBuffer {
    void *start = nullptr;      // mmap 映射的起始地址
    std::size_t length = 0;     // 缓冲区长度（字节）
};

// V4L2CaptureDevice：基于 V4L2 内存映射（MMAP）方式的采集设备实现。
// 典型流程：Open -> Start(查询能力/配置格式/映射缓冲区/开始推流)
//           -> CaptureFrame / CaptureRawFrame -> Stop -> Close。
class V4L2CaptureDevice : public ICaptureDevice {
public:
    V4L2CaptureDevice(const config::CaptureConfig &capture_config, const config::CodecConfig &codec_config);
    ~V4L2CaptureDevice() override;

    bool Open() override;
    bool Start() override;
    common::model::EncodedFramePtr CaptureFrame() override;
    void Stop() override;
    void Close() override;
    std::string Describe() const override;

    // V4L2 设备支持原始帧采集（双线程模式）。
    bool SupportsRawCapture() const override;
    RawCaptureFramePtr CaptureRawFrame() override;
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw) override;

private:
    // 执行 ioctl；若被信号中断（EINTR）则自动重试。
    int IoctlWithRetry(unsigned long request, void *arg);

    // 查询设备能力（是否支持视频采集与流式 I/O）。
    bool QueryCapabilities();

    // 配置采集格式（分辨率、YUYV 像素格式等）。
    bool ConfigureFormat();

    // 尝试配置帧率；驱动不支持时仅告警并继续。
    bool ConfigureFrameRate();

    // 申请并 mmap 映射采集缓冲区。
    bool InitializeMemoryMapping();

    // 将全部缓冲区放入驱动队列，等待摄像头填充。
    bool QueueCaptureBuffers();

    // 通知驱动开始推流（VIDIOC_STREAMON）。
    bool StartStreaming();

    // 通知驱动停止推流（VIDIOC_STREAMOFF）。
    void StopStreaming();

    // 解除全部 mmap 映射并清空缓冲区列表。
    void ReleaseMappedBuffers();

private:
    config::CaptureConfig capture_config_;                      // 采集配置（设备路径、分辨率、帧率、缓冲数等）
    config::CodecConfig codec_config_;                          // 编码器配置
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;  // 视频编码器（H.264）
    int device_fd_;                                             // 设备文件描述符，未打开时为 -1
    bool opened_;                                               // 设备是否已打开
    bool streaming_;                                            // 是否处于推流状态
    bool logged_timestamp_source_;                              // 是否已输出时间戳来源提示（仅打印一次）
    std::vector<MappedBuffer> buffers_;                         // mmap 映射的采集缓冲区列表
    std::uint64_t sequence_;                                    // 帧序号，每帧自增
};

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
