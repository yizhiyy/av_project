// TransportModule.h
// 传输模块（TransportModule）头文件。
// 将传输能力包装为 core::IModule 模块，供应用框架统一管理生命周期，
// 并提供向所有接收端广播编码帧的入口。

#ifndef SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H
#define SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "core/IModule.h"
#include "modules/transport/Transport.h"

namespace sserver {
namespace modules {
namespace transport {

// TransportModule：传输模块的 IModule 适配层。
class TransportModule : public core::IModule {
public:
    // send_latency_recorder 用于统计发送延迟指标。
    explicit TransportModule(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~TransportModule() override;

    std::string name() const override;
    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;

    // 将一帧编码数据广播给所有接收端。
    void Broadcast(common::model::EncodedFramePtr frame);

    // 实际绑定的本地端口。
    int bound_port() const;

    // 实际使用的传输后端名称。
    std::string backend_name() const;

private:
    std::unique_ptr<Transport> transport_;  // 底层传输门面
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TRANSPORTMODULE_H
