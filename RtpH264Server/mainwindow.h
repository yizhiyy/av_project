#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QImage>
#include <QPixmap>
#include <QVector>
#include <QMap>
#include <QDebug>
#include <QtCore>
#include <QTimer>
#include <arpa/inet.h>



extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/opt.h>
}


#pragma pack(push, 1) // 强制1字节对齐
struct RTPHeader {
    uint8_t version_p_x_cc;  // 版本(2)+P(1)+X(1)+CC(4)
    uint8_t pt_m;            // PT(7)+M(1)
    uint16_t sequenceNumber; // 序列号（大端）
    uint32_t timestamp;      // 时间戳（大端）
    uint32_t ssrc;           // SSRC（大端）
};
#pragma pack(pop)


QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindow *ui;
    void initializeDecoder(); // Initialize FFmpeg decoder
    QUdpSocket *udpSocket = nullptr;   // UDP socket for communication
    AVCodecContext *codecContext = nullptr;  // Codec context for video decoding
    AVFrame *avFrame = nullptr;            // Decoded frame
    AVPacket *pkt = nullptr;                // Packet for holding received data
    SwsContext *swsContext;
    int swsContextSrcWidth;
    int swsContextSrcHeight;
    AVPixelFormat swsContextSrcFormat;
    QMap<quint16, QByteArray> packetBuffer; // Buffer to hold packet data (using packet index)


private slots:
    void receiveDatagram();
    void decodeFrame();
    void displayFrame();

};
#endif // MAINWINDOW_H
