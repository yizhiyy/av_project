#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QImage>
#include <QTimer>
#include "imageprocessor.h"
#include "rtph264sender.h"

//#include <QElapsedTimer>

#include <opencv2/opencv.hpp>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class CameraWorker;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    QUdpSocket *udpSocket;



private slots:
    void showImage(const QImage &img);
    void showError(const QString &message);

    void on_btn_start_clicked();
    void on_btn_stop_clicked();
    void on_btn_capture_clicked();
    void on_btn_select_clicked();

    void on_btn_gray_clicked();

    void on_btn_binary_clicked();

    void on_btn_canny_clicked();

    void onGrayProcessed(const QImage &img);
    void onBinaryProcessed(const QImage &img);
    void onCannyProcessed(const QImage &img);


    void on_brightnessSlider_valueChanged(int value);
    void on_contrastSlider_valueChanged(int value);
    void onCameraStarted();  // 摄像头启动后获取默认参数并初始化滑块

    void on_saturationSlider_valueChanged(int value);

    void on_gammaSlider_valueChanged(int value);

    void on_sharpnessSlider_valueChanged(int value);


    void on_btn_fps_decrease_clicked();
    void on_btn_fps_increase_clicked();
    void updateFpsDisplay(int fps);

    void on_btn_record_clicked();

    void on_btn_upload_clicked();
    void uploadDataToServer(const QByteArray &data);
    void startUploadServer();

    //void testFFmpeg();

signals:
    void startCamera(const QString &device);
    void stopCamera();
    void captureImage();

    void processGray(const QImage &img);
    void processBinary(const QImage &img);
    void processCanny(const QImage &img);

    void setFrameRateSignal(int fps);

    void startRecordSignal(); // 开始录制
    void stopRecordSignal(); // 停止录制
    void processImage(const QImage &img, bool doGray, bool doBinary, bool doCanny);

    void sendFrameToServer(const QImage &img);





private:
    Ui::MainWindow *ui;

    QThread *workerThread = nullptr;
    CameraWorker *worker = nullptr;

    QThread *imageThread = nullptr;    // 图像处理线程
    ImageProcessor *imageProcessor = nullptr;
    bool isGrayRunning = false;
    bool isBinaryRunning = false;
    bool isCannyRunning = false;
    bool isConnectServer = false;

    QThread *uploadServerThread = nullptr;    // 线程
    RtpH264Sender *rtpH264Sender = nullptr;
    QTimer* uploadServerTimer = nullptr;
    QImage resizedImageProcessImage;



    QList<int> supportedFps = {5, 10, 15, 20, 25, 30}; // 支持的帧率列表
    int currentFpsIndex = 5; // 默认30fps（索引5）
    int g_fps = 33;

    bool isRecording = false;  // 跟踪录制状态

};
#endif // MAINWINDOW_H
