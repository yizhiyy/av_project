// CaptureModule.h
// 采集模块（CaptureModule）头文件。
// 将采集能力包装为 core::IModule 模块，供应用框架统一管理生命周期，
// 并通过后台线程持续产帧、回调给下游（如编码/传输模块）。

#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/model/EncodedFrame.h"
#include "core/IModule.h"
#include "modules/capture/video/Capture.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

// 帧回调类型：下游模块通过 SetFrameHandler 注册，
// 采集模块每产出一帧编码数据就调用一次。
using FrameHandler = std::function<void(common::model::EncodedFramePtr)>;

// CaptureModule：采集线程的宿主模块。
// 工作模式由底层设备能力决定：
//   - 单线程模式：采集+编码在同一线程完成（CaptureLoop）；
//   - 双线程模式：采集线程出原始帧入队，编码线程出队编码（CapturePump + EncodePump）。
class CaptureModule : public core::IModule {
public:
    CaptureModule();
    ~CaptureModule() override;

    std::string name() const override;
    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;

    // 注册帧回调；回调会在线程上下文中被调用。
    void SetFrameHandler(FrameHandler handler);

private:
    // 单线程路径：采集并编码后直接回调。
    void CaptureLoop();
    // 双线程路径：只负责采集原始帧并入队。
    void CapturePump();
    // 双线程路径：从队列取原始帧、编码并回调。
    void EncodePump();

private:
    std::unique_ptr<Capture> capture_;                 // 底层采集门面
    std::thread worker_thread_;                        // 采集线程（单线程路径或采集泵）
    std::thread encode_thread_;                        // 编码线程（仅双线程路径使用）
    std::atomic_bool running_;                         // 线程运行标志
    std::atomic<core::ModuleState> state_;             // 模块状态（跨线程读取）
    std::mutex handler_mutex_;                         // 保护帧回调的读写
    FrameHandler frame_handler_;                       // 当前注册的帧回调
    common::concurrency::ThreadSafeQueue<RawCaptureFramePtr> raw_queue_;  // 原始帧队列（采集->编码）
    bool use_raw_capture_;                             // 是否启用双线程原始帧模式
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
