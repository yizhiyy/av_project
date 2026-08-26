#include "modules/encoding/video/x264/X264VideoEncoderBackend.h"

#include <cstdlib>
#include <cstring>

#ifdef SSERVER_HAS_NEON
#include <arm_neon.h>
#endif

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace encoding {

namespace {

// NEON 加速版本：将 YUYV422 原始帧转换为 I420 平面格式。
// 仅在编译时启用 SSERVER_HAS_NEON 的 ARM 平台上使用。
#ifdef SSERVER_HAS_NEON
void ConvertYuyv422ToI420Neon(
        const std::uint8_t *src,
        std::uint8_t *y_plane,
        std::uint8_t *u_plane,
        std::uint8_t *v_plane,
        int width,
        int height) {
    const int row_bytes = width * 2;      // YUYV 每行字节数
    const int neon_pixels = 16;           // 每次 SIMD 处理 16 个像素
    const int aligned_width = (width / neon_pixels) * neon_pixels;  // 可 SIMD 对齐的宽度
    const int uv_row_stride = width / 2;  // 色度平面每行样本数

    // I420 中每两行共享一行色度，因此按 2 行一组处理。
    for (int row = 0; row < height; row += 2) {
        const std::uint8_t *src_even = src + static_cast<std::size_t>(row) * row_bytes;
        std::uint8_t *y_row_even = y_plane + static_cast<std::size_t>(row) * width;
        std::uint8_t *u_row = u_plane + static_cast<std::size_t>(row / 2) * uv_row_stride;
        std::uint8_t *v_row = v_plane + static_cast<std::size_t>(row / 2) * uv_row_stride;

        // 偶数行：提取 Y 值和 U/V 色度对
        for (int col = 0; col < aligned_width; col += neon_pixels) {
            const std::uint8_t *p = src_even + col * 2;
            // vld2 按两个分量交错读取：val[0] 为偶数下标（Y），val[1] 为奇数下标（U/V 交错）。
            uint8x16x2_t yuyv = vld2q_u8(p);
            // Y 序列直接写入亮度平面。
            vst1q_u8(y_row_even + col, yuyv.val[0]);

            // val[1] 中包含交错排列的 U0 V0 U1 V1 ...，需要解交错分离
            uint8x8_t u_lo = vget_low_u8(yuyv.val[1]);
            uint8x8_t v_lo = vget_high_u8(yuyv.val[1]);
            uint8x8x2_t uzp = vuzp_u8(u_lo, v_lo);
            vst1_u8(u_row + col / 2, uzp.val[0]);
            vst1_u8(v_row + col / 2, uzp.val[1]);
        }
        // 尾部不足 16 像素的部分用标量方式处理。
        for (int col = aligned_width; col < width; col += 2) {
            const std::uint8_t *p = src_even + col * 2;
            y_row_even[col] = p[0];
            y_row_even[col + 1] = p[2];
            u_row[col / 2] = p[1];
            v_row[col / 2] = p[3];
        }

        // 奇数行：只需提取 Y 值（U/V 由偶数行共享）
        if (row + 1 < height) {
            const std::uint8_t *src_odd = src + static_cast<std::size_t>(row + 1) * row_bytes;
            std::uint8_t *y_row_odd = y_plane + static_cast<std::size_t>(row + 1) * width;

            for (int col = 0; col < aligned_width; col += neon_pixels) {
                uint8x16x2_t yuyv = vld2q_u8(src_odd + col * 2);
                vst1q_u8(y_row_odd + col, yuyv.val[0]);
            }
            for (int col = aligned_width; col < width; col += 2) {
                const std::uint8_t *p = src_odd + col * 2;
                y_row_odd[col] = p[0];
                y_row_odd[col + 1] = p[2];
            }
        }
    }
}
#endif

// 标量版本：将 YUYV422 原始帧转换为 I420 平面格式。
// YUYV 布局：每像素 2 字节，两像素共享一对 U/V（Y0 U Y1 V）。
// I420 布局：全部 Y 平面 + 1/4 大小的 U 平面 + 1/4 大小的 V 平面。
void ConvertYuyv422ToI420Scalar(
        const std::uint8_t *src,
        std::uint8_t *y_plane,
        std::uint8_t *u_plane,
        std::uint8_t *v_plane,
        int width,
        int height) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 2;

    // 提取所有亮度 Y 值：YUYV 中 Y 位于每像素对偏移 0 和 2。
    int y_index = 0;
    for (int row = 0; row < height; ++row) {
        const std::size_t row_start = static_cast<std::size_t>(row) * row_bytes;
        for (int col = 0; col < width; col += 2) {
            y_plane[y_index++] = src[row_start + col * 2];
            y_plane[y_index++] = src[row_start + col * 2 + 2];
        }
    }

    // 从偶数行对中提取色度 U 和 V 值（I420 每两行共享一行色度）。
    for (int row = 0; row < height; row += 2) {
        const std::size_t row_start = static_cast<std::size_t>(row) * row_bytes;
        const int uv_row = (row / 2) * (width / 2);
        for (int col = 0; col < width; col += 2) {
            const std::size_t offset = row_start + col * 2;
            u_plane[uv_row + col / 2] = src[offset + 1];
            v_plane[uv_row + col / 2] = src[offset + 3];
        }
    }
}

}  // namespace

// x264 后端工厂：创建 X264VideoEncoderBackend 实例。
std::unique_ptr<VideoEncoderBackend> CreateX264VideoEncoderBackend() {
    return std::unique_ptr<VideoEncoderBackend>(new X264VideoEncoderBackend());
}

// 构造函数：所有指针初始化为空，PTS 从 0 开始。
X264VideoEncoderBackend::X264VideoEncoderBackend()
        : backend_name_("x264"),
          param_(nullptr),
          handle_(nullptr),
          picture_(nullptr),
          nal_(nullptr),
          pts_(0) {
}

// 析构函数：确保释放全部 x264 资源。
X264VideoEncoderBackend::~X264VideoEncoderBackend() {
    Shutdown();
}

// 初始化 x264 编码器：
// 1. 分配参数与输入画面对象；
// 2. 加载默认参数与预设（preset/tune），再套用配置项；
// 3. 打开编码器并分配 I420 画面平面。
bool X264VideoEncoderBackend::Initialize(
        int width,
        int height,
        int fps,
        const config::CodecConfig &config,
        std::string *error_message) {
    // 支持重复初始化：先释放旧资源。
    Shutdown();

    // 分配参数对象与输入画面对象。
    param_ = static_cast<x264_param_t *>(std::malloc(sizeof(x264_param_t)));
    picture_ = static_cast<x264_picture_t *>(std::malloc(sizeof(x264_picture_t)));
    if (param_ == nullptr || picture_ == nullptr) {
        common::log::Logger::Error("x264 allocation failed");
        if (error_message != nullptr) {
            *error_message = "x264 allocation failed";
        }
        Shutdown();
        return false;
    }

    // 先取 x264 默认参数，再应用预设与调优选项。
    x264_param_default(param_);
    x264_param_default_preset(param_, config.x264_preset.c_str(), config.x264_tune.c_str());

    // 应用画面与编码配置项。
    param_->i_width = width;
    param_->i_height = height;
    param_->rc.i_lookahead = config.x264_lookahead;
    param_->i_sync_lookahead = config.x264_sync_lookahead;
    param_->i_fps_num = fps;       // 帧率：fps 帧/秒
    param_->i_fps_den = 1;
    param_->b_annexb = config.x264_annexb ? 1 : 0;            // Annex-B 码流格式（便于 RTP 打包）
    param_->i_keyint_max = config.x264_keyint_max;            // 最大关键帧间隔
    param_->i_keyint_min = config.x264_keyint_min;            // 最小关键帧间隔
    param_->i_bframe = config.x264_bframes;                   // B 帧数量
    param_->b_repeat_headers = config.x264_repeat_headers ? 1 : 0;  // 每帧重复 SPS/PPS
    param_->i_threads = config.x264_threads;                  // 编码线程数
    param_->b_sliced_threads = config.x264_sliced_threads ? 1 : 0;
    param_->i_slice_count = config.x264_slice_count;
    param_->i_slice_count_max = config.x264_slice_count_max;
    param_->i_frame_reference = config.x264_frame_reference;  // 参考帧数
    param_->analyse.i_subpel_refine = config.x264_subpel_refine;
    param_->rc.b_mb_tree = config.x264_mb_tree ? 1 : 0;
    param_->i_scenecut_threshold = config.x264_scenecut;

    // 应用 profile（如 baseline/main/high）。
    x264_param_apply_profile(param_, config.x264_profile.c_str());

    // 打开编码器。
    handle_ = x264_encoder_open(param_);
    if (handle_ == nullptr) {
        common::log::Logger::Error("x264_encoder_open failed");
        if (error_message != nullptr) {
            *error_message = "x264_encoder_open failed";
        }
        Shutdown();
        return false;
    }

    // 为输入画面分配 I420 平面（Y/U/V 共 3 个平面）。
    x264_picture_alloc(picture_, X264_CSP_I420, width, height);
    picture_->img.i_csp = X264_CSP_I420;
    picture_->img.i_plane = 3;
    pts_ = 0;
    return true;
}

// 编码一帧 YUYV422 原始图像为 H264 码流
// 流程：色彩空间转换 (YUYV→I420) → x264 编码 → 收集 NAL 输出
bool X264VideoEncoderBackend::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe,
        std::string *error_message) {
    // 校验输入输出与编码器状态。
    if (input == nullptr || output == nullptr || is_keyframe == nullptr ||
        handle_ == nullptr || picture_ == nullptr || param_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "x264 encoder is not initialized";
        }
        return false;
    }

    const int width = param_->i_width;
    const int height = param_->i_height;
    // YUYV422 每像素 2 字节。
    const std::size_t expected_length = static_cast<std::size_t>(width * height * 2);
    if (input_length < expected_length) {
        common::log::Logger::Warn("input frame is shorter than expected YUYV422 buffer");
        if (error_message != nullptr) {
            *error_message = "input frame is shorter than expected YUYV422 buffer";
        }
        return false;
    }

    // 将 YUYV422 转换为 I420 填入输入画面的三个平面。
#ifdef SSERVER_HAS_NEON
    ConvertYuyv422ToI420Neon(
            input,
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[0]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[1]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[2]),
            width,
            height);
#else
    ConvertYuyv422ToI420Scalar(
            input,
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[0]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[1]),
            reinterpret_cast<std::uint8_t *>(picture_->img.plane[2]),
            width,
            height);
#endif

    // 帧类型由 x264 自动决策（I/P/B），PTS 逐帧自增。
    picture_->i_type = X264_TYPE_AUTO;
    picture_->i_pts = pts_++;

    x264_picture_t picture_out{};
    int nal_count = 0;
    output->clear();
    *is_keyframe = false;
    // 送入编码器；输出 NAL 数组由 x264 内部管理（仅在下一次调用前有效）。
    if (x264_encoder_encode(handle_, &nal_, &nal_count, picture_, &picture_out) < 0) {
        common::log::Logger::Error("x264_encoder_encode failed");
        if (error_message != nullptr) {
            *error_message = "x264_encoder_encode failed";
        }
        return false;
    }
    // 从输出画面信息中获取关键帧标志。
    *is_keyframe = picture_out.b_keyframe != 0;

    // 先统计 NAL 总长度，预留输出空间，再逐个拷贝 NAL 载荷。
    std::size_t total_size = 0;
    for (int index = 0; index < nal_count; ++index) {
        total_size += static_cast<std::size_t>(nal_[index].i_payload);
    }
    output->reserve(total_size);
    for (int index = 0; index < nal_count; ++index) {
        const std::uint8_t *payload = nal_[index].p_payload;
        output->insert(output->end(), payload, payload + nal_[index].i_payload);
    }

    // 个别情况（如需要刷新延迟帧）可能不产出数据，这里视为“无输出”。
    if (output->empty() && error_message != nullptr) {
        *error_message = "x264 encoder did not produce output";
    }
    return !output->empty();
}

// 释放全部 x264 资源：画面平面、参数、编码器句柄。
void X264VideoEncoderBackend::Shutdown() {
    if (picture_ != nullptr) {
        x264_picture_clean(picture_);   // 释放内部平面内存
        std::free(picture_);            // 释放 picture 对象本身
        picture_ = nullptr;
    }

    if (param_ != nullptr) {
        std::free(param_);
        param_ = nullptr;
    }

    if (handle_ != nullptr) {
        x264_encoder_close(handle_);
        handle_ = nullptr;
    }

    nal_ = nullptr;
    pts_ = 0;
}

// 返回后端类型：x264。
EncodeBackend X264VideoEncoderBackend::backend() const {
    return EncodeBackend::kX264;
}

// 返回后端名称。
const std::string &X264VideoEncoderBackend::backend_name() const {
    return backend_name_;
}

}  // namespace encoding
}  // namespace modules
}  // namespace sserver
