#include "modules/capture/video/CaptureModule.h"

#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace capture {

// 构造函数：创建采集门面，初始化线程标志、状态与工作模式。
CaptureModule::CaptureModule()
        : capture_(std::make_unique<Capture>()),
          running_(false),
          state_(core::ModuleState::kCreated),
          use_raw_capture_(false) {
}

// 析构函数：确保后台线程与设备资源被释放。
CaptureModule::~CaptureModule() {
    shutdown();
}

// 模块名称，供日志与框架识别。
std::string CaptureModule::name() const {
    return "CaptureModule";
}

// 初始化：委托给 Capture 门面，失败时记录日志并置为 Failed。
bool CaptureModule::initialize(const core::ApplicationContext &context) {
    std::string error_message;
    if (!capture_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }
    state_ = core::ModuleState::kInitialized;
    return true;
}

// 启动：先启动底层采集，再根据设备能力拉起对应的工作线程。
bool CaptureModule::start() {
    std::string error_message;
    if (!capture_->start(&error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    // 设备支持原始帧时使用双线程（采集/编码分离），否则单线程采集+编码。
    use_raw_capture_ = capture_->SupportsRawCapture();
    if (use_raw_capture_) {
        worker_thread_ = std::thread(&CaptureModule::CapturePump, this);
        encode_thread_ = std::thread(&CaptureModule::EncodePump, this);
    } else {
        worker_thread_ = std::thread(&CaptureModule::CaptureLoop, this);
    }
    state_ = core::ModuleState::kRunning;
    common::log::Logger::Info("capture module started with " + capture_->Describe() +
                              (use_raw_capture_ ? " (dual-thread raw capture)" : " (single-thread)"));
    return true;
}

// 停止：置位停止标志并唤醒等待线程，回收线程后停止采集。
void CaptureModule::stop() {
    // 只有 Running 状态需要真正停止。
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    raw_queue_.NotifyAll();  // 唤醒可能阻塞在队列上的编码线程

    // 依次等待采集/编码线程退出，避免悬挂资源。
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (encode_thread_.joinable()) {
        encode_thread_.join();
    }
    capture_->stop();
    state_ = core::ModuleState::kStopped;
}

// 关闭：先停止，再释放采集设备。
void CaptureModule::shutdown() {
    stop();
    capture_->shutdown();
    state_ = core::ModuleState::kShutdown;
}

// 返回线程安全的模块状态。
core::ModuleState CaptureModule::state() const {
    return state_.load();
}

// 注册帧回调；加锁防止与工作线程并发修改。
void CaptureModule::SetFrameHandler(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    frame_handler_ = handler;
}

// 单线程路径：采集与编码在同一线程中完成（适用于不支持原始帧采集的设备）。
void CaptureModule::CaptureLoop() {
    while (running_.load()) {
        common::model::EncodedFramePtr frame = capture_->CaptureFrame();
        if (!frame) {
            // 暂无可用帧（如缓冲区为空）时短暂休眠，避免忙等。
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // 在锁内拷贝回调句柄，锁外执行回调，避免长时间持锁。
        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

// 采集泵：从设备取出原始帧 → 入队等待编码。
// 队列满时丢弃最旧帧（PushDropOldest），保证画面实时性优先。
void CaptureModule::CapturePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw = capture_->CaptureRawFrame();
        if (!raw) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        raw_queue_.PushDropOldest(raw, 3);
    }
    raw_queue_.NotifyAll();  // 退出前唤醒编码线程，避免其永久阻塞
}

// 编码泵：从队列取出原始帧 → 编码 → 分发给下游。
void CaptureModule::EncodePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw;
        // 带超时等待，便于及时响应停止请求。
        if (!raw_queue_.WaitPopFor(&raw, std::chrono::milliseconds(10))) {
            continue;
        }
        if (!raw) {
            continue;
        }

        common::model::EncodedFramePtr frame = capture_->EncodeRawFrame(raw);
        if (!frame) {
            continue;
        }

        // 同样采用“锁内拷贝、锁外回调”的方式分发编码帧。
        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
