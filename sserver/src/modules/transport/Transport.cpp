#include "modules/transport/Transport.h"

#include <utility>

#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpStreamingBackend.h"
#include "modules/transport/tcp/TcpStreamingBackend.h"
#include "modules/transport/udp/UdpStreamingBackend.h"

namespace sserver {
namespace modules {
namespace transport {

// 传输后端工厂：按解析结果创建对应后端实例。
std::unique_ptr<ITransportBackend> TransportBackendFactory::Create(
        const TransportBackendSelection &selection,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        std::string *error_message) {
    switch (selection.backend) {
        case TransportBackend::kTcp:
            return std::unique_ptr<ITransportBackend>(new tcp::TcpStreamingBackend(send_latency_recorder));
        case TransportBackend::kUdp:
            return std::unique_ptr<ITransportBackend>(new udp::UdpStreamingBackend(send_latency_recorder));
        case TransportBackend::kRtp:
            return std::unique_ptr<ITransportBackend>(new rtp::RtpStreamingBackend(send_latency_recorder));
        case TransportBackend::kAuto:
        default:
            // kAuto 表示尚未解析，正常流程不应到达这里。
            if (error_message != nullptr) {
                *error_message = "requested transport backend is not supported";
            }
            return nullptr;
    }
}

// Transport 的内部实现（Pimpl）。
struct Transport::TransportImpl {
    TransportBackend requested_backend = TransportBackend::kAuto;   // 配置请求的后端
    std::string requested_backend_name = "auto";                     // 请求后端名称
    TransportBackend active_backend = TransportBackend::kTcp;       // 实际启用的后端
    std::string active_backend_name = "tcp";                        // 实际启用后端名称
    std::unique_ptr<ITransportBackend> backend;                     // 具体传输后端实例
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder;  // 发送延迟统计器
    core::ModuleState state = core::ModuleState::kCreated;          // 模块生命周期状态
};

// 构造函数：创建内部实现并保存延迟统计器。
Transport::Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : impl_(std::make_unique<TransportImpl>()) {
    impl_->send_latency_recorder = send_latency_recorder;
}

// 析构函数：确保后端资源被释放。
Transport::~Transport() {
    shutdown();
}

// 初始化：先清理旧状态，再解析后端、创建并初始化后端实例。
bool Transport::initialize(const core::ApplicationContext &context, std::string *error_message) {
    // 支持重复初始化。
    shutdown();

    // 根据配置解析传输后端。
    TransportBackendSelection selection;
    if (!ResolveTransportBackendSelection(context.config.transport, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;

    // 创建后端实例（传入延迟统计器供发送侧记录指标）。
    impl_->backend = TransportBackendFactory::Create(selection, impl_->send_latency_recorder, error_message);
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 后端初始化（如创建监听套接字）。
    if (!impl_->backend->initialize(context)) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to initialize";
        }
        impl_->backend.reset();
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    // 初始化成功后才更新实际后端与状态。
    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();
    impl_->state = impl_->backend->state();
    return true;
}

// 启动传输：委托给后端。
bool Transport::start(std::string *error_message) {
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "transport is not initialized";
        }
        return false;
    }

    if (!impl_->backend->start()) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to start";
        }
        impl_->state = impl_->backend->state();
        return false;
    }

    impl_->state = impl_->backend->state();
    return true;
}

// 停止传输：委托给后端。
void Transport::stop() {
    if (impl_->backend) {
        impl_->backend->stop();
        impl_->state = impl_->backend->state();
        return;
    }
    impl_->state = core::ModuleState::kStopped;
}

// 完全关闭：释放后端实例并重置状态。
void Transport::shutdown() {
    if (impl_->backend) {
        impl_->backend->shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = TransportBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = TransportBackend::kTcp;
    impl_->active_backend_name = "tcp";
    impl_->state = core::ModuleState::kShutdown;
}

// 返回当前状态：有后端时以后端状态为准。
core::ModuleState Transport::state() const {
    if (impl_->backend) {
        return impl_->backend->state();
    }
    return impl_->state;
}

// 广播一帧数据；后端不存在时静默丢弃。
void Transport::Broadcast(common::model::EncodedFramePtr frame) {
    if (impl_->backend) {
        impl_->backend->Broadcast(frame);
    }
}

// 返回实际绑定端口；后端不存在时返回 0。
int Transport::bound_port() const {
    if (!impl_->backend) {
        return 0;
    }
    return impl_->backend->bound_port();
}

// 返回实际后端类型。
TransportBackend Transport::backend() const {
    return impl_->active_backend;
}

// 返回实际后端名称。
const std::string &Transport::backend_name() const {
    return impl_->active_backend_name;
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
