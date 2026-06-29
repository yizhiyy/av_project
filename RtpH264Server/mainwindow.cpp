#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <algorithm>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , swsContext(nullptr)
    , swsContextSrcWidth(0)
    , swsContextSrcHeight(0)
    , swsContextSrcFormat(AV_PIX_FMT_NONE)
{
    ui->setupUi(this);
    ui->origal_label->setGeometry(10, 10, 640, 480);

    // FFmpeg全局初始化（兼容嵌入式新旧版本）
    avformat_network_init();   // 初始化网络
    avcodec_register_all();    // 强制注册所有编解码器

    // 初始化UDP Socket（接收端口20000）
    udpSocket = new QUdpSocket(this);
    if (!udpSocket->bind(QHostAddress::Any, 20000)) {
        qDebug() << "UDP绑定端口失败！";
        return;
    }
    connect(udpSocket, &QUdpSocket::readyRead, this, &MainWindow::receiveDatagram);

    // 初始化解码器
    qDebug() << "initializeDecoder";
    initializeDecoder();
}

MainWindow::~MainWindow()
{
    avcodec_free_context(&codecContext);
    av_packet_unref(pkt);
    av_frame_free(&avFrame);
    if (swsContext) sws_freeContext(swsContext);
    delete ui;
}



void MainWindow::initializeDecoder() {
    qDebug() << "FFmpeg 版本:" << av_version_info();

    // 查找H.264解码器
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qDebug() << "错误：未找到H.264软解码器！";
        codecContext = nullptr;
        avFrame = nullptr;
        pkt = nullptr;
        swsContext = nullptr;
        return;
    }
    qDebug() << "找到H.264解码器:" << codec->name;

    // 分配解码器上下文
    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        qDebug() << "错误：分配解码器上下文失败！";
        return;
    }

    // 低延迟解码配置
    codecContext->thread_count = 1;
    codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
    codecContext->pkt_timebase = {1, 30};

    // 打开解码器
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        qDebug() << "错误：打开H.264解码器失败！";
        avcodec_free_context(&codecContext);
        codecContext = nullptr;
        return;
    }

    // 分配帧、包
    pkt = av_packet_alloc();
    avFrame = av_frame_alloc();

    // 注意：swsContext 将在第一次解码成功时动态创建
    swsContext = nullptr;

    qDebug() << "H.264解码器初始化成功！";
}

void MainWindow::receiveDatagram() {
    // 解码器未初始化则丢弃数据
    if (!codecContext || !avFrame || !pkt) {
        while (udpSocket->hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(udpSocket->pendingDatagramSize());
            udpSocket->readDatagram(datagram.data(), datagram.size());
        }
        return;
    }

    while (udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(udpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.size() < 12) continue; // 无效RTP包

        // 解析RTP头
        RTPHeader *rtpHeader = reinterpret_cast<RTPHeader *>(datagram.data());
        quint16 packetIndex = qFromBigEndian(rtpHeader->sequenceNumber);
        bool isLastPacket = (rtpHeader->pt_m & 0x80) != 0;
        QByteArray packetData = datagram.mid(12);

        // 缓存策略：丢弃过旧的包
        static quint16 lastFrameMaxIndex = 0;
        if (isLastPacket) lastFrameMaxIndex = packetIndex;
        if (packetIndex < lastFrameMaxIndex - 100) continue;

        packetBuffer[packetIndex] = packetData;

        if (isLastPacket) {
            QList<quint16> sortedIndexes = packetBuffer.keys();
            std::sort(sortedIndexes.begin(), sortedIndexes.end());

            // --- 新增：检查序号连续性（简单丢包检测）---
            bool missingPacket = false;
            for (int i = 1; i < sortedIndexes.size(); ++i) {
                if (sortedIndexes[i] != sortedIndexes[i-1] + 1) {
                    qDebug() << "【服务端】检测到丢包，期望序号" << sortedIndexes[i-1]+1
                             << "实际收到" << sortedIndexes[i];
                    missingPacket = true;
                    break;
                }
            }

            if (missingPacket) {
                    qDebug() << "【服务端】帧数据不完整，丢弃该帧";
                    packetBuffer.clear();
                    continue;
            }

            // 拼接数据
            QByteArray fullData;
            for (quint16 idx : sortedIndexes) fullData.append(packetBuffer[idx]);
            packetBuffer.clear();

            qDebug() << "【服务端】帧拼接完成，总大小：" << fullData.size();

            // 使用 av_packet_from_data 安全地管理包数据
            av_packet_unref(pkt);
            uint8_t *buf = (uint8_t *)av_malloc(fullData.size());
            if (!buf) return;
            memcpy(buf, fullData.data(), fullData.size());
            av_packet_from_data(pkt, buf, fullData.size());

            decodeFrame();
        }
    }
}

void MainWindow::decodeFrame() {
    if (!codecContext || !pkt || !avFrame) {
        av_packet_unref(pkt);
        return;
    }

    // 发送数据到解码器
    int ret = avcodec_send_packet(codecContext, pkt);
    if (ret < 0) {
        qDebug() << "【服务端】解码发送失败";
        av_packet_unref(pkt);
        return;
    }

    // 接收解码后的YUV420P帧
    while (true) {
        ret = avcodec_receive_frame(codecContext, avFrame);
        if (ret == 0) {
            qDebug() << "【服务端】解码成功！尺寸：" << avFrame->width << "x" << avFrame->height
                     << " 格式：YUV420P（" << avFrame->format << "）";
            displayFrame(); // 转换并显示
            av_frame_unref(avFrame);
        } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else {
            qDebug() << "【服务端】解码接收失败，错误码：" << ret;
            break;
        }
    }

    av_packet_unref(pkt);
}

void MainWindow::displayFrame() {
    if (!avFrame) return;

    int srcWidth = avFrame->width;
    int srcHeight = avFrame->height;
    if (srcWidth <= 0 || srcHeight <= 0) return;

    AVPixelFormat srcFmt = (AVPixelFormat)avFrame->format;

    // 动态创建/更新 SwsContext（目标格式改为 BGRA）
    if (!swsContext ||
        swsContextSrcWidth != srcWidth ||
        swsContextSrcHeight != srcHeight ||
        swsContextSrcFormat != srcFmt) {

        if (swsContext) sws_freeContext(swsContext);

        swsContext = sws_getContext(srcWidth, srcHeight, srcFmt,
                                    srcWidth, srcHeight, AV_PIX_FMT_BGRA,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsContext) {
            qDebug() << "【服务端】创建颜色转换上下文失败";
            return;
        }

        swsContextSrcWidth = srcWidth;
        swsContextSrcHeight = srcHeight;
        swsContextSrcFormat = srcFmt;
        qDebug() << "【服务端】SwsContext 已创建/更新，尺寸：" << srcWidth << "x" << srcHeight;
    }

    // 分配 BGRA 帧
    AVFrame *bgraFrame = av_frame_alloc();
    if (!bgraFrame) {
        qDebug() << "【服务端】分配BGRA帧失败";
        return;
    }

    bgraFrame->format = AV_PIX_FMT_BGRA;
    bgraFrame->width  = srcWidth;
    bgraFrame->height = srcHeight;

    if (av_frame_get_buffer(bgraFrame, 0) < 0) {
        qDebug() << "【服务端】获取BGRA帧缓冲区失败";
        av_frame_free(&bgraFrame);
        return;
    }

    int ret = sws_scale(swsContext,
                        avFrame->data, avFrame->linesize, 0, srcHeight,
                        bgraFrame->data, bgraFrame->linesize);
    if (ret < 0) {
        qDebug() << "【服务端】颜色转换失败";
        av_frame_free(&bgraFrame);
        return;
    }

    // 构造 QImage（Format_RGB32 需要 BGRA 内存布局）
    QImage img(bgraFrame->data[0], srcWidth, srcHeight,
               bgraFrame->linesize[0], QImage::Format_RGB32);
    if (img.isNull()) {
        qDebug() << "【服务端】QImage构造失败";
        av_frame_free(&bgraFrame);
        return;
    }

    // 深拷贝并显示
    QImage imgCopy = img.copy();
    QPixmap pix = QPixmap::fromImage(imgCopy);
    ui->origal_label->setScaledContents(true);
    ui->origal_label->setPixmap(pix.scaled(ui->origal_label->size(),
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation));
    ui->origal_label->update();

    av_frame_free(&bgraFrame);
    qDebug() << "【服务端】解码显示成功，尺寸：" << srcWidth << "x" << srcHeight;
}
