// ITransportBackend.h
// 传输后端抽象接口（Interface）。
// 所有传输实现（TCP / UDP / RTP）都必须实现该接口，
// 上层 Transport 门面通过它统一管理传输后端的生命周期与数据发送。

#ifndef SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
#define SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/transport/Transport.h"

namespace sserver {
namespace modules {
namespace transport {

// ITransportBackend：传输后端的统一接口。
class ITransportBackend {
public:
    virtual ~ITransportBackend() = default;

    // 初始化：读取配置并准备资源（如创建监听套接字）。
    virtual bool initialize(const core::ApplicationContext &context) = 0;

    // 启动：开始监听/绑定端口，进入可发送状态。
    virtual bool start() = 0;

    // 停止：停止收发（资源仍保留，可再次启动）。
    virtual void stop() = 0;

    // 关闭：释放全部资源（不可再启动）。
    virtual void shutdown() = 0;

    // 当前模块状态。
    virtual core::ModuleState state() const = 0;

    // 将一帧编码数据广播给所有已连接的接收端。
    virtual void Broadcast(common::model::EncodedFramePtr frame) = 0;

    // 实际绑定的本地端口（用于日志/客户端连接）。
    virtual int bound_port() const = 0;

    // 返回后端类型。
    virtual TransportBackend backend() const = 0;

    // 返回后端名称（用于日志）。
    virtual const std::string &backend_name() const = 0;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_ITRANSPORTBACKEND_H
