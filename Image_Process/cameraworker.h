#ifndef CAMERAWORKER_H
#define CAMERAWORKER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QTimer>
#include <QFile>
#include <linux/videodev2.h>
#include <QDateTime>
#include <QString>
#include <QThread>
#include <QVector>
#include <QElapsedTimer>

#include "rtph264sender.h"

#include <opencv2/opencv.hpp>
//#include <opencv2/core.hpp>
//#include <opencv2/videoio.hpp>

#include <opencv2/imgproc.hpp>


class CameraWorker : public QObject
{
    Q_OBJECT
public:
    explicit CameraWorker(QObject *parent = nullptr);
    ~CameraWorker();
    // 获取摄像头运行状态
    bool isCameraRunning() const { return isRunning; }
    bool setV4L2Control(int controlId, int value);
    // 获取指定控制项的默认值
    int getControlDefaultValue(int controlId);

    bool setFrameRate(int fps);
    int getFrameRate() const { return currentFps; }
    // 暴露帧尺寸的getter（给录制线程传参）
    int getFrameWidth() const { return frameWidth; }
    int getFrameHeight() const { return frameHeight; }

    cv::Mat qimageToCvMat(const QImage &img);

signals:
    void sendImage(const QImage &img);
    void errorOccurred(const QString &message);
    void uploadStateChanged(bool isUploading);
    void videoSaved(bool success, const QString &message);


public slots:
    void start(const QString &device);
    void stop();
    void capture();
    void startRecording(); // 开始录制
    void stopRecording(); // 停止录制
    void saveRecordedVideo(const QString &fileName);

    void clearRecordFrames();


private slots:
    void captureFrame();



private:
    int fd = -1;
    QMutex mutex;
    QImage currentImg;
    QTimer *timer = nullptr;
    bool isRunning = false;
    int frameWidth = 320;  // 调整默认分辨率为320x240
    int frameHeight = 240;
    int currentFps = 30; // 默认30fps
    QString currentDevice;

    bool isRecording = false; // 录制状态标记
    std::vector<cv::Mat> recordFrames; // 存储录制帧
    std::vector<qint64> recordTimestamps;       // 对应的时间戳（毫秒）
    QElapsedTimer recordElapsedTimer;           // 录制计时器

    struct Buffer {
        unsigned char *start;
        size_t length;
    };
    std::vector<Buffer> buffers;


    bool initV4L2(const char *dev);
    QImage readFrame();   // 返回转换后的 RGB 图像
    QImage yuyvToRgb(unsigned char *data, int w, int h, size_t bufferSize);

#ifdef DEBUG
    bool checkControlSupport(int controlId);
    void listAllControls(); // 调试用，列出所有支持的控制项

#endif

};
#endif // CAMERAWORKER_H
