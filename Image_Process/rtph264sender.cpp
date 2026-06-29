#include "rtph264sender.h"
#include <QDebug>
#include <opencv2/opencv.hpp>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>


RtpH264Sender::RtpH264Sender(QObject *parent) : QObject(parent)
{
    codecContext = nullptr;
    pkt = nullptr;
    avFrame = nullptr;
    swsCtx = nullptr;
    sequenceNumber = 0;
    initializeEncoder();
}

RtpH264Sender::~RtpH264Sender()
{
    if (swsCtx) sws_freeContext(swsCtx);
    if (codecContext) {
        avcodec_close(codecContext);
        avcodec_free_context(&codecContext);
    }
    if (pkt) av_packet_free(&pkt);
    if (avFrame) av_frame_free(&avFrame);
}

bool RtpH264Sender::initializeEncoder()
{
    // 注册编码器（老版本FFmpeg需要）
    avcodec_register_all();

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        qDebug() << "无法找到 H.264 编码器";
        return false;
    }

    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        qDebug() << "无法分配编码器上下文";
        return false;
    }

    // 编码器参数（零延迟+固定尺寸）
    codecContext->bit_rate = 5000000;
    codecContext->width = 640;
    codecContext->height = 480;
    codecContext->time_base = (AVRational){1, 30};
    codecContext->framerate = (AVRational){30, 1};
    codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    codecContext->gop_size = 1;          // 无GOP延迟
    codecContext->max_b_frames = 0;      // 禁用B帧
    codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // x264参数（零延迟）
    if (av_opt_set(codecContext->priv_data, "preset", "ultrafast", 0) < 0) {
        qDebug() << "无法设置ultrafast预设";
        return false;
    }
    if (av_opt_set(codecContext->priv_data, "tune", "zerolatency", 0) < 0) {
        qDebug() << "无法设置零延迟模式";
        return false;
    }
    if (av_opt_set(codecContext->priv_data, "profile", "baseline", 0) < 0) {
        qDebug() << "无法设置baseline profile";
        return false;
    }

    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        qDebug() << "无法打开 H.264 编码器";
        return false;
    }

    pkt = av_packet_alloc();
    avFrame = av_frame_alloc();
    avFrame->format = codecContext->pix_fmt;
    avFrame->width = codecContext->width;
    avFrame->height = codecContext->height;
    av_frame_get_buffer(avFrame, 32);

    qDebug() << "编码器初始化成功";

    // 创建颜色空间转换上下文（BGR24 -> YUV420P），整个生命周期复用
    swsCtx = sws_getContext(codecContext->width, codecContext->height, AV_PIX_FMT_BGR24,
                            codecContext->width, codecContext->height, AV_PIX_FMT_YUV420P,
                            SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        qDebug() << "无法创建SwsContext";
        return false;
    }
    return true;
}

bool RtpH264Sender::encodeFrameH264(const cv::Mat &frame, QByteArray &encodedData)
{
    if (frame.empty() || !codecContext || !avFrame || !swsCtx) {
        qDebug() << "编码输入无效";
        return false;
    }

    // 调整尺寸至编码器要求
    cv::Mat bgrFrame;
    cv::resize(frame, bgrFrame, cv::Size(codecContext->width, codecContext->height));

    // 确保AVFrame可写
    av_frame_make_writable(avFrame);

    // 设置输入数据指针（BGR24）
    const int in_linesize[1] = { 3 * bgrFrame.cols };
    uint8_t* inData[1] = { bgrFrame.data };

    // 使用预先创建的swsCtx进行转换
    int ret = sws_scale(swsCtx,
                        inData, in_linesize, 0, bgrFrame.rows,
                        avFrame->data, avFrame->linesize);
    if (ret < 0) {
        qDebug() << "颜色空间转换失败";
        return false;
    }

    // 设置PTS
    avFrame->pts = av_frame_get_best_effort_timestamp(avFrame) + 1;

    // 发送帧到编码器
    ret = avcodec_send_frame(codecContext, avFrame);
    if (ret < 0) {
        qDebug() << "发送帧到编码器失败:" << ret;
        return false;
    }

    encodedData.clear();
    while (true) {
        ret = avcodec_receive_packet(codecContext, pkt);
        if (ret == 0) {
            encodedData.append((const char*)pkt->data, pkt->size);
            av_packet_unref(pkt);
        } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else {
            qDebug() << "接收编码包失败:" << ret;
            return false;
        }
    }

    return !encodedData.isEmpty();
}

void RtpH264Sender::sendFrame(const QImage &image)
{
    if (image.isNull()) {
        qDebug() << "输入QImage为空";
        return;
    }

    // 确保图像格式为 RGB888（24位）以便安全转换为 cv::Mat
    QImage rgbImage = image.convertToFormat(QImage::Format_RGB888);
    if (rgbImage.isNull()) {
        qDebug() << "QImage格式转换失败";
        return;
    }

    // 构造 cv::Mat（注意：QImage::Format_RGB888 存储顺序为 R,G,B）
    cv::Mat frame(rgbImage.height(), rgbImage.width(), CV_8UC3,
                  (void*)rgbImage.bits(), rgbImage.bytesPerLine());

    // OpenCV 默认 BGR 顺序，需要将 RGB -> BGR
    cv::Mat bgrFrame;
    cv::cvtColor(frame, bgrFrame, cv::COLOR_RGB2BGR);

    QByteArray encodedData;
    if (!encodeFrameH264(bgrFrame, encodedData)) {
        qDebug() << "编码失败";
        return;
    }

    sendUdpPackets(encodedData);
}

// 3. 同步修改sendUdpPackets函数的参数类型
void RtpH264Sender::sendUdpPackets(const QByteArray &data)
{
    const int MTU = 1460; // 1500-20-8-12
    int totalSize = data.size();
    int packetCount = (totalSize + MTU - 1) / MTU;

    for (int i = 0; i < packetCount; i++) {
        int offset = i * MTU;
        int size = qMin(MTU, totalSize - offset);
        // 直接用QByteArray的mid方法截取，无需类型转换
        QByteArray payload = data.mid(offset, size);

        // 构造RTP包
        QByteArray rtpPacket;
        rtpPacket.resize(12 + size);
        RTPHeader* header = (RTPHeader*)rtpPacket.data();

        header->version_p_x_cc = 0x80; // 版本2
        header->pt_m = 96 | (i == packetCount - 1 ? 0x80 : 0); // 最后一包置M位
        // 用Qt原生字节序转换（避免ntohs/htonl报错）
        header->sequenceNumber = qToBigEndian(sequenceNumber++);
        header->timestamp = qToBigEndian(static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF));
        header->ssrc = qToBigEndian(static_cast<uint32_t>(0x12345678));

        memcpy(rtpPacket.data() + 12, payload.data(), size);
        emit dataReadyToUpload(rtpPacket);
    }
}


