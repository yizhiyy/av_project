// ICaptureDevice.h
// 采集设备抽象接口（Interface）。
// 所有具体采集设备（空设备、V4L2 摄像头等）都必须实现该接口，
// 上层模块只需面向接口编程，便于替换采集后端。

#ifndef SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace capture {

// RawCaptureFrame：未编码的原始采集帧。
// 用于“采集线程与编码线程分离”的双线程模式，避免编码耗时阻塞采集。
struct RawCaptureFrame {
    std::vector<std::uint8_t> data;   // 原始图像数据（例如 YUYV 格式）
    std::uint64_t capture_timestamp_ns = 0;  // 采集时刻（单调时钟，纳秒）
    std::size_t bytes_used = 0;       // 数据实际使用的字节数（可能小于缓冲区长度）
};

// 原始帧的智能指针别名，便于在队列/回调中传递。
using RawCaptureFramePtr = std::shared_ptr<RawCaptureFrame>;

// ICaptureDevice：采集设备生命周期与数据采集的统一接口。
// 典型调用顺序：Open() -> Start() -> CaptureFrame()/CaptureRawFrame() -> Stop() -> Close()。
class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;

    // 打开设备：检查/初始化底层硬件资源（文件描述符等）。
    virtual bool Open() = 0;

    // 启动采集：完成格式配置、缓冲区准备等，进入可采帧状态。
    virtual bool Start() = 0;

    // 采集一帧并返回已编码帧（采集+编码一体化路径）。
    // 返回空指针表示暂无可采帧或采集/编码失败。
    virtual common::model::EncodedFramePtr CaptureFrame() = 0;

    // 停止采集：停止底层数据流，释放编码器等运行时资源。
    virtual void Stop() = 0;

    // 关闭设备：释放打开阶段占用的资源（如关闭文件描述符）。
    virtual void Close() = 0;

    // 返回设备描述字符串，用于日志与状态展示。
    virtual std::string Describe() const = 0;

    // 以下为可选接口（默认实现表示“不支持原始帧采集”）：
    // 支持原始帧采集的设备可重写这三个方法，配合双线程采集+编码模式使用。

    // 是否支持原始帧采集（即 CaptureRawFrame / EncodeRawFrame 是否可用）。
    virtual bool SupportsRawCapture() const { return false; }

    // 采集一帧原始数据（不做编码），返回原始帧。
    virtual RawCaptureFramePtr CaptureRawFrame() { return nullptr; }

    // 将一帧原始数据编码为编码帧（通常由独立编码线程调用）。
    virtual common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr /* raw */) { return nullptr; }
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
