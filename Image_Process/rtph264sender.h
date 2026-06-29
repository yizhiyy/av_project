#ifndef RTPH264SENDER_H
#define RTPH264SENDER_H

#include <QObject>
#include <QRunnable>
#include <QUdpSocket>
#include <QImage>
#include <QThread>
#include <QtEndian>
#include <QDateTime>
#include <opencv2/opencv.hpp>


#include <QVector>
#include <QtGlobal>
#include <arpa/inet.h>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
//#include <openssl/evp.h>
#include <libswscale/swscale.h>

}


//struct RTPHeader {
//    quint8 version_p_x_cc;  // 版本 (2 bits) | P(1 bit) | X(1 bit) | CC(4 bits)
//    quint8 m_pt;            // PT (7 bits)
//    quint8 m_marker;        // Marker 位 (1 bit)
//    quint16 sequenceNumber; // 序列号
//    quint32 timestamp;      // 时间戳
//    quint32 ssrc;           // SSRC 标识
//};

#pragma pack(push, 1) // 强制1字节对齐
struct RTPHeader {
    uint8_t version_p_x_cc;  // 版本(2)+P(1)+X(1)+CC(4)
    uint8_t pt_m;            // PT(7)+M(1)
    uint16_t sequenceNumber; // 序列号（大端）
    uint32_t timestamp;      // 时间戳（大端）
    uint32_t ssrc;           // SSRC（大端）
};
#pragma pack(pop)

using namespace cv;

class RtpH264Sender : public QObject
{
    Q_OBJECT
public:
    explicit RtpH264Sender(QObject *parent = nullptr);
    ~RtpH264Sender();

    void sendFrame(const QImage &image);
    //virtual void run();

private:
    AVCodecContext *codecContext;
    AVPacket *pkt;
    AVFrame *avFrame;

    quint16 sequenceNumber = 0;

    SwsContext *swsCtx;

    bool initializeEncoder();
    bool encodeFrameH264(const cv::Mat &frame, QByteArray &encodedData);
    void sendUdpPackets(const QByteArray &data);

signals:
    void dataReadyToUpload(const QByteArray &data);
};

#endif // RTPH264SENDER_H
