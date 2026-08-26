#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log/Logger.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

namespace {

// 将 V4L2 内核时间戳（秒 + 微秒）转换为纳秒。
std::uint64_t TimevalToNs(const timeval &timestamp) {
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_usec) * 1000ULL;
}

}  // namespace

// 构造函数：保存配置，并将设备状态初始化为“未打开、未推流”。
V4L2CaptureDevice::V4L2CaptureDevice(
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config)
        : capture_config_(capture_config),
          codec_config_(codec_config),
          device_fd_(-1),
          opened_(false),
          streaming_(false),
          logged_timestamp_source_(false),
          sequence_(0) {
}

// 析构函数：确保停止推流并关闭设备文件。
V4L2CaptureDevice::~V4L2CaptureDevice() {
    Stop();
    Close();
}

// 打开设备：校验路径是字符设备后以读写方式打开文件描述符。
bool V4L2CaptureDevice::Open() {
    // 先用 stat 检查设备路径是否存在。
    struct stat device_stat{};
    if (stat(capture_config_.device.c_str(), &device_stat) < 0) {
        common::log::Logger::Error("cannot identify video device: " + capture_config_.device);
        return false;
    }

    // V4L2 设备必须是字符设备（如 /dev/video0）。
    if (!S_ISCHR(device_stat.st_mode)) {
        common::log::Logger::Error("configured capture device is not a character device: " + capture_config_.device);
        return false;
    }

    // 以可读写方式打开设备，后续 ioctl 需要该描述符。
    device_fd_ = open(capture_config_.device.c_str(), O_RDWR, 0);
    if (device_fd_ < 0) {
        common::log::Logger::Error("failed to open capture device: " + capture_config_.device);
        return false;
    }

    opened_ = true;
    return true;
}

// 启动 V4L2 采集设备：依次完成能力查询、格式配置、帧率设置、内存映射、缓冲区入队，
// 然后初始化编码器并开始推流。
bool V4L2CaptureDevice::Start() {
    if (!opened_) {
        return false;
    }

    // 采集链路准备：任何一步失败都释放已映射的缓冲区并返回失败。
    if (!QueryCapabilities() || !ConfigureFormat() || !ConfigureFrameRate() || !InitializeMemoryMapping() ||
        !QueueCaptureBuffers()) {
        ReleaseMappedBuffers();
        return false;
    }

    // 创建并初始化 H.264 编码器，用于把 YUYV 原始帧编码为编码帧。
    encoder_ = std::make_unique<modules::encoding::VideoEncoder>();

    std::string error_message;
    if (!encoder_->Initialize(
                capture_config_.width,
                capture_config_.height,
                capture_config_.fps,
                codec_config_,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize video encoder: " + error_message);
        }
        ReleaseMappedBuffers();
        encoder_.reset();
        return false;
    }

    // 通知驱动开始出帧。
    if (!StartStreaming()) {
        encoder_->Shutdown();
        encoder_.reset();
        ReleaseMappedBuffers();
        return false;
    }

    streaming_ = true;
    return true;
}

// 采集并编码一帧（单线程路径）：
// 从驱动取出已填充的缓冲区（DQBUF）-> 编码 -> 归还缓冲区（QBUF）。
common::model::EncodedFramePtr V4L2CaptureDevice::CaptureFrame() {
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    // 准备取帧请求：视频采集类型 + MMAP 内存方式。
    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    // 从驱动取出一帧（DQBUF）；EAGAIN 表示暂时没有新帧。
    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return common::model::EncodedFramePtr();
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return common::model::EncodedFramePtr();
    }

    // 优先使用驱动提供的单调时钟时间戳；否则回退为取出时刻。
    std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    if (has_monotonic_timestamp) {
        capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }
    // 只在第一次记录时间戳来源，避免刷屏。
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    // 指向缓冲区数据；bytesused 为实际有效字节数，取不到时用缓冲区长度。
    std::vector<std::uint8_t> encoded_payload;
    bool is_keyframe = false;
    const void *start = buffers_[buffer.index].start;
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;
    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;

    // 将 YUYV 原始帧编码为 H.264。
    if (encoder_ == nullptr ||
        !encoder_->EncodeYuyv422Frame(
                static_cast<const std::uint8_t *>(start),
                length,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        // 编码失败也要归还缓冲区，否则驱动缓冲区会被耗尽。
        IoctlWithRetry(VIDIOC_QBUF, &buffer);
        return common::model::EncodedFramePtr();
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    // 编码完成后归还缓冲区，让驱动继续复用。
    if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
        common::log::Logger::Warn("VIDIOC_QBUF failed");
        return common::model::EncodedFramePtr();
    }

    // 组装编码帧：序号、类型、采集/编码时间戳、关键帧标志与载荷。
    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;
    frame->payload.swap(encoded_payload);
    return frame;
}

// V4L2 设备支持原始帧采集（双线程路径）。
bool V4L2CaptureDevice::SupportsRawCapture() const {
    return true;
}

// 采集一帧原始数据：从驱动取出缓冲区并拷贝数据，随后立即归还缓冲区。
RawCaptureFramePtr V4L2CaptureDevice::CaptureRawFrame() {
    if (!streaming_) {
        return nullptr;
    }

    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return nullptr;
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return nullptr;
    }

    // 记录时间戳，逻辑与 CaptureFrame 相同。
    auto raw = std::make_shared<RawCaptureFrame>();
    raw->capture_timestamp_ns = common::time::MonotonicNowNs();
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    if (has_monotonic_timestamp) {
        raw->capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    // 把驱动缓冲区数据拷贝到 RawCaptureFrame，避免归还缓冲区后数据失效。
    const void *start = buffers_[buffer.index].start;
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;
    raw->data.assign(
            static_cast<const std::uint8_t *>(start),
            static_cast<const std::uint8_t *>(start) + length);
    raw->bytes_used = length;

    // 立即归还缓冲区。
    IoctlWithRetry(VIDIOC_QBUF, &buffer);
    return raw;
}

// 编码一帧原始数据（由独立编码线程调用）。
common::model::EncodedFramePtr V4L2CaptureDevice::EncodeRawFrame(RawCaptureFramePtr raw) {
    if (!raw || encoder_ == nullptr) {
        return nullptr;
    }

    std::vector<std::uint8_t> encoded_payload;
    bool is_keyframe = false;
    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;
    if (!encoder_->EncodeYuyv422Frame(
                raw->data.data(),
                raw->bytes_used,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        return nullptr;
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    // 复用原始帧上的采集时间戳，补齐编码时间戳后返回。
    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = raw->capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;
    frame->payload.swap(encoded_payload);
    return frame;
}

// 停止：停止推流、释放编码器与映射缓冲区（设备保持打开，可再次 Start）。
void V4L2CaptureDevice::Stop() {
    if (!opened_) {
        return;
    }

    if (streaming_) {
        StopStreaming();
        streaming_ = false;
    }

    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }
    ReleaseMappedBuffers();
}

// 关闭：关闭设备文件描述符。
void V4L2CaptureDevice::Close() {
    if (device_fd_ >= 0) {
        close(device_fd_);
        device_fd_ = -1;
    }
    opened_ = false;
}

// 返回设备描述，包含设备路径便于日志区分。
std::string V4L2CaptureDevice::Describe() const {
    return "v4l2-capture-device(" + capture_config_.device + ")";
}

// 执行 ioctl：若被信号中断（EINTR）则自动重试，保证调用完整执行。
int V4L2CaptureDevice::IoctlWithRetry(unsigned long request, void *arg) {
    int result = -1;
    do {
        result = ioctl(device_fd_, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

// 查询设备能力：必须支持视频采集与流式 I/O。
bool V4L2CaptureDevice::QueryCapabilities() {
    struct v4l2_capability capability{};
    if (IoctlWithRetry(VIDIOC_QUERYCAP, &capability) < 0) {
        common::log::Logger::Error("VIDIOC_QUERYCAP failed");
        return false;
    }

    // 设备必须能输出视频帧。
    if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        common::log::Logger::Error("device does not support video capture");
        return false;
    }

    // 设备必须支持流式 I/O（我们使用的是 MMAP 方式）。
    if ((capability.capabilities & V4L2_CAP_STREAMING) == 0) {
        common::log::Logger::Error("device does not support streaming I/O");
        return false;
    }

    return true;
}

// 配置采集格式：设置分辨率并要求 YUYV 像素格式。
bool V4L2CaptureDevice::ConfigureFormat() {
    struct v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = capture_config_.width;
    format.fmt.pix.height = capture_config_.height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_INTERLACED;

    // S_FMT 会实际设置格式（驱动可能调整参数）。
    if (IoctlWithRetry(VIDIOC_S_FMT, &format) < 0) {
        common::log::Logger::Error("VIDIOC_S_FMT failed");
        return false;
    }

    return true;
}

// 配置帧率：尽量设置为配置值；驱动不支持时仅告警，不阻塞启动。
bool V4L2CaptureDevice::ConfigureFrameRate() {
    struct v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 先读取当前流参数。
    if (IoctlWithRetry(VIDIOC_G_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_G_PARM failed, skipping frame rate configuration");
        return true;
    }

    // 驱动不支持设置每帧时间时直接跳过。
    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
        common::log::Logger::Warn("capture device does not expose V4L2_CAP_TIMEPERFRAME");
        return true;
    }

    // 每帧时间 = 1 / fps（秒）。
    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = static_cast<unsigned int>(capture_config_.fps);
    if (IoctlWithRetry(VIDIOC_S_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_S_PARM failed, continuing with driver default fps");
        return true;
    }

    return true;
}

// 初始化内存映射：向驱动申请缓冲区并逐个 mmap 到用户空间。
bool V4L2CaptureDevice::InitializeMemoryMapping() {
    // 申请指定数量的 MMAP 缓冲区。
    struct v4l2_requestbuffers request{};
    request.count = static_cast<unsigned int>(capture_config_.device_buffer_count);
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (IoctlWithRetry(VIDIOC_REQBUFS, &request) < 0) {
        common::log::Logger::Error("VIDIOC_REQBUFS failed");
        return false;
    }

    // 至少需要 2 个缓冲区，否则采集与编码无法流水线化。
    if (request.count < 2) {
        common::log::Logger::Error("insufficient capture buffers");
        return false;
    }

    buffers_.clear();
    buffers_.resize(request.count);

    // 逐个查询缓冲区信息并映射到用户空间。
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (IoctlWithRetry(VIDIOC_QUERYBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QUERYBUF failed");
            return false;
        }

        // MAP_SHARED：与内核共享内存，驱动写入后用户可直接读取。
        buffers_[index].length = buffer.length;
        buffers_[index].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd_, buffer.m.offset);
        if (buffers_[index].start == MAP_FAILED) {
            buffers_[index].start = nullptr;
            common::log::Logger::Error("mmap failed for capture buffer");
            return false;
        }
    }

    return true;
}

// 将所有缓冲区放入驱动队列，等待摄像头写入画面数据。
bool V4L2CaptureDevice::QueueCaptureBuffers() {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QBUF failed while queueing capture buffer");
            return false;
        }
    }
    return true;
}

// 通知驱动开始推流。
bool V4L2CaptureDevice::StartStreaming() {
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (IoctlWithRetry(VIDIOC_STREAMON, &buffer_type) < 0) {
        common::log::Logger::Error("VIDIOC_STREAMON failed");
        return false;
    }
    return true;
}

// 通知驱动停止推流。
void V4L2CaptureDevice::StopStreaming() {
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    IoctlWithRetry(VIDIOC_STREAMOFF, &buffer_type);
}

// 解除全部 mmap 映射并清空缓冲区列表。
void V4L2CaptureDevice::ReleaseMappedBuffers() {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        if (buffers_[index].start != nullptr) {
            munmap(buffers_[index].start, buffers_[index].length);
            buffers_[index].start = nullptr;
            buffers_[index].length = 0;
        }
    }
    buffers_.clear();
}

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver
