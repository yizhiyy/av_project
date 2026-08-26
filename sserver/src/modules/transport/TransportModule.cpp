#include "modules/transport/TransportModule.h"

#include <utility>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {

// 构造函数：创建底层 Transport 门面。
TransportModule::TransportModule(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : transport_(std::make_unique<Transport>(send_latency_recorder)) {
}

// 析构函数：确保传输资源被释放。
TransportModule::~TransportModule() {
    shutdown();
}

// 模块名称，供日志与框架识别。
std::string TransportModule::name() const {
    return "TransportModule";
}

// 初始化：委托给 Transport，失败时记录日志。
bool TransportModule::initialize(const core::ApplicationContext &context) {
    std::string error_message;
    if (!transport_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize transport: " + error_message);
        }
        return false;
    }
    return true;
}

// 启动：委托给 Transport，失败时记录日志。
bool TransportModule::start() {
    std::string error_message;
    if (!transport_->start(&error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start transport: " + error_message);
        }
        return false;
    }
    return true;
}

// 停止：委托给 Transport。
void TransportModule::stop() {
    transport_->stop();
}

// 关闭：委托给 Transport。
void TransportModule::shutdown() {
    transport_->shutdown();
}

// 返回模块状态。
core::ModuleState TransportModule::state() const {
    return transport_->state();
}

// 广播一帧数据给所有接收端。
void TransportModule::Broadcast(common::model::EncodedFramePtr frame) {
    transport_->Broadcast(frame);
}

// 返回实际绑定端口。
int TransportModule::bound_port() const {
    return transport_->bound_port();
}

// 返回实际后端名称。
std::string TransportModule::backend_name() const {
    return transport_->backend_name();
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
