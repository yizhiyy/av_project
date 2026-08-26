#include "modules/transport/tcp/TcpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

// 构造函数：初始化后端状态，后端名称固定为 "tcp"。
TcpStreamingBackend::TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("tcp"),
          latency_log_interval_frames_(120),
          listen_socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated) {
}

// 析构函数：确保监听套接字与客户端会话全部释放。
TcpStreamingBackend::~TcpStreamingBackend() {
    shutdown();
}

// 初始化：保存传输配置与统计参数。
bool TcpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    state_ = core::ModuleState::kInitialized;
    return true;
}

// 启动：打开监听套接字，并启动后台接受线程。
bool TcpStreamingBackend::start() {
    // 传输未启用时直接进入 Running，让上层流程照常继续。
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    if (!OpenListenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&TcpStreamingBackend::AcceptLoop, this);
    state_ = core::ModuleState::kRunning;
    return true;
}

// 停止：关闭监听套接字、回收接受线程，并逐个停止客户端会话。
void TcpStreamingBackend::stop() {
    // 只有 Running 状态需要真正停止。
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    CloseListenSocket();  // 关闭后 accept 立即返回错误，线程可退出

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // 把客户端列表取出来再逐个停止，避免长时间持锁。
    std::vector<std::shared_ptr<TcpClientSession>> clients_copy;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_copy.swap(clients_);
    }
    for (std::size_t index = 0; index < clients_copy.size(); ++index) {
        clients_copy[index]->Stop();
    }

    state_ = core::ModuleState::kStopped;
}

// 关闭：停止后置为 Shutdown。
void TcpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

// 返回模块状态（线程安全）。
core::ModuleState TcpStreamingBackend::state() const {
    return state_.load();
}

// 广播一帧：先快照客户端列表，再把帧入队到每个运行中的会话。
void TcpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    if (!config_.enabled || !frame) {
        return;
    }

    // 拷贝快照，避免发送期间阻塞 accept 线程。
    std::vector<std::shared_ptr<TcpClientSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        snapshot = clients_;
    }

    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        if (snapshot[index]->IsRunning()) {
            snapshot[index]->EnqueueFrame(frame);
        }
    }

    // 顺带清理已断开/停止的客户端。
    PruneClosedClients();
}

// 返回实际绑定端口。
int TcpStreamingBackend::bound_port() const {
    return bound_port_;
}

// 返回后端类型：TCP。
TransportBackend TcpStreamingBackend::backend() const {
    return TransportBackend::kTcp;
}

// 返回后端名称。
const std::string &TcpStreamingBackend::backend_name() const {
    return backend_name_;
}

// 打开监听套接字：创建、允许地址复用、设置非阻塞、绑定并开始监听。
bool TcpStreamingBackend::OpenListenSocket() {
    listen_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_fd_ < 0) {
        common::log::Logger::Error("failed to create tcp listen socket");
        return false;
    }

    // 允许 TIME_WAIT 状态下快速重启服务。
    int reuse = 1;
    setsockopt(listen_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 设置非阻塞，使 accept 循环可以带轮询间隔运行。
    const int current_flags = fcntl(listen_socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(listen_socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    if (bind(listen_socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind tcp transport server");
        CloseListenSocket();
        return false;
    }

    // 开始监听，积压队列长度为 16。
    if (listen(listen_socket_fd_, 16) < 0) {
        common::log::Logger::Error("failed to listen on tcp transport socket");
        CloseListenSocket();
        return false;
    }

    // 记录实际绑定端口（listen_port 为 0 时由内核分配临时端口）。
    if (config_.listen_port == 0) {
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);
        if (getsockname(listen_socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("tcp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

// 关闭监听套接字。
void TcpStreamingBackend::CloseListenSocket() {
    if (listen_socket_fd_ >= 0) {
        close(listen_socket_fd_);
        listen_socket_fd_ = -1;
    }
}

// 接受线程：循环 accept，为每个新连接创建并启动会话。
void TcpStreamingBackend::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        const int client_fd = accept(listen_socket_fd_, reinterpret_cast<sockaddr *>(&client_address), &address_length);
        if (client_fd < 0) {
            // 非阻塞模式下没有新连接或信号中断时，稍作休眠后继续。
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.accept_loop_interval_ms));
                continue;
            }
            common::log::Logger::Warn("accept failed on tcp transport socket");
            break;
        }

        // 创建会话并启动收发线程。
        std::shared_ptr<TcpClientSession> session(
                new TcpClientSession(client_fd, config_, send_latency_recorder_, latency_log_interval_frames_));
        if (!session->Start()) {
            session->Stop();
            continue;
        }

        common::log::Logger::Info("tcp client connected: " + session->remote_endpoint());
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(session);
        }
        PruneClosedClients();
    }
}

// 移除已断开/停止的客户端会话。
void TcpStreamingBackend::PruneClosedClients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [](const std::shared_ptr<TcpClientSession> &client) {
                        return client == nullptr || !client->IsRunning();
                    }),
            clients_.end());
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
