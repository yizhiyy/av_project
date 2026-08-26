// TcpStreamingBackend.h
// TCP 传输后端头文件。
// 作为 TCP 服务端监听连接，为每个客户端创建 TcpClientSession，
// 并将编码帧广播给所有已连接的客户端。

#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/tcp/TcpClientSession.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// TcpStreamingBackend：TCP 传输后端的实现。
class TcpStreamingBackend : public ITransportBackend {
public:
    explicit TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~TcpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    // 创建、绑定并监听 TCP 套接字。
    bool OpenListenSocket();

    // 关闭监听套接字。
    void CloseListenSocket();

    // 后台线程：循环 accept 新连接并创建会话。
    void AcceptLoop();

    // 移除已停止/断开的客户端会话。
    void PruneClosedClients();

private:
    config::TransportConfig config_;            // 传输配置
    std::string backend_name_;                  // 后端名称（"tcp"）
    int latency_log_interval_frames_;           // 延迟统计日志的帧间隔
    int listen_socket_fd_;                      // 监听套接字描述符，未打开时为 -1
    int bound_port_;                            // 实际绑定的本地端口
    std::atomic_bool running_;                  // 接受线程运行标志
    std::thread accept_thread_;                 // 接受新连接的线程
    std::mutex clients_mutex_;                  // 保护客户端列表
    std::vector<std::shared_ptr<TcpClientSession>> clients_;  // 当前客户端会话列表
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;  // 发送延迟统计器
    std::atomic<core::ModuleState> state_;      // 模块状态
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
