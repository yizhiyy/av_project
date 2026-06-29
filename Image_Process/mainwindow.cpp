#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "cameraworker.h"
#include <QMessageBox>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QtMath>  // 用于数学运算
#include <linux/videodev2.h> // 包含V4L2控制ID定义


extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/version.h>
#include <libavcodec/avcodec.h>
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    workerThread = new QThread(this);
    worker = new CameraWorker();
    worker->moveToThread(workerThread);

    imageThread = new QThread(this);
    imageProcessor = new ImageProcessor();
    imageProcessor->moveToThread(imageThread);
    imageThread->start();

    // 信号槽连接（确保线程安全）
    connect(worker, &CameraWorker::sendImage, this, &MainWindow::showImage, Qt::QueuedConnection);
    connect(worker, &CameraWorker::errorOccurred, this, &MainWindow::showError, Qt::QueuedConnection);
    connect(this, &MainWindow::startCamera, worker, &CameraWorker::start, Qt::QueuedConnection);
    connect(this, &MainWindow::stopCamera, worker, &CameraWorker::stop, Qt::QueuedConnection);
    connect(this, &MainWindow::captureImage, worker, &CameraWorker::capture, Qt::QueuedConnection);
    connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &MainWindow::setFrameRateSignal, worker, &CameraWorker::setFrameRate, Qt::QueuedConnection);  // 设置帧率

    // 新增录制控制的信号槽
    connect(this, &MainWindow::startRecordSignal, worker, &CameraWorker::startRecording, Qt::QueuedConnection);
    connect(this, &MainWindow::stopRecordSignal, worker, &CameraWorker::stopRecording, Qt::QueuedConnection);


    connect(this, &MainWindow::processImage, imageProcessor, &ImageProcessor::process, Qt::QueuedConnection);
    connect(imageProcessor, &ImageProcessor::grayProcessed, this, &MainWindow::onGrayProcessed);
    connect(imageProcessor, &ImageProcessor::binaryProcessed, this, &MainWindow::onBinaryProcessed);
    connect(imageProcessor, &ImageProcessor::cannyProcessed, this, &MainWindow::onCannyProcessed);




    connect(ui->brightnessSlider, &QSlider::valueChanged,
            this, &MainWindow::on_brightnessSlider_valueChanged);
    connect(ui->contrastSlider, &QSlider::valueChanged,
            this, &MainWindow::on_contrastSlider_valueChanged);
    connect(ui->saturationSlider, &QSlider::valueChanged,
            this, &MainWindow::on_saturationSlider_valueChanged);
    connect(ui->gammaSlider, &QSlider::valueChanged,
                this, &MainWindow::on_gammaSlider_valueChanged);
    connect(ui->sharpnessSlider, &QSlider::valueChanged,
            this, &MainWindow::on_sharpnessSlider_valueChanged);

    connect(worker, &CameraWorker::videoSaved, this, [this](bool success, const QString &msg) {
        // 恢复按钮状态
        ui->btn_record->setEnabled(true);
        ui->btn_start->setEnabled(true);
        ui->btn_stop->setEnabled(true);

        if (success) {
            QMessageBox::information(this, "保存成功", msg);
        } else {
            QMessageBox::warning(this, "保存失败", msg);
        }
    });



    // 先禁用滑块（摄像头启动后再启用）
    ui->brightnessSlider->setEnabled(false);
    ui->contrastSlider->setEnabled(false);
    ui->saturationSlider->setEnabled(false);
    ui->gammaSlider->setEnabled(false);
    ui->sharpnessSlider->setEnabled(false);

    ui->btn_capture->setEnabled(false);  // 初始禁用，摄像头启动后启用
    ui->btn_record->setEnabled(false);  // 初始禁用，摄像头启动后启用

    workerThread->start();
}

MainWindow::~MainWindow()
{
    // 停止摄像头
    emit stopCamera();

    workerThread->quit();
    workerThread->wait(); // 等待线程安全退出

    // 停止图像处理线程
    imageThread->quit();
    imageThread->wait();
    delete imageProcessor;

    // 安全清理上传线程（可能从未启动）
    if (uploadServerThread) {
        uploadServerThread->quit();
        uploadServerThread->wait();
    }
    if (rtpH264Sender) {
        delete rtpH264Sender;
    }


    delete ui;
}



void MainWindow::onGrayProcessed(const QImage &img)
{
    //lastGrayImage = img;
    ui->lb_gray->setPixmap(QPixmap::fromImage(img.scaled(
        ui->lb_gray->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation
    )));
}

void MainWindow::onBinaryProcessed(const QImage &img)
{
    //lastBinaryImage = img;
    ui->lb_binary->setPixmap(QPixmap::fromImage(img.scaled(
        ui->lb_binary->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation
    )));
}

void MainWindow::onCannyProcessed(const QImage &img)
{
    //lastCannyImage = img;
    ui->lb_canny->setPixmap(QPixmap::fromImage(img.scaled(
        ui->lb_canny->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation
    )));
}


void MainWindow::showImage(const QImage &img)
{
    // 原始图像显示
    ui->lb_display->setPixmap(QPixmap::fromImage(img.scaled(
        ui->lb_display->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation
    )));

    if(isGrayRunning || isBinaryRunning || isCannyRunning)
    {
        emit processImage(img, isGrayRunning, isBinaryRunning, isCannyRunning);
    }

}



void MainWindow::showError(const QString &message)
{
    QMessageBox::warning(this, "操作失败", message);
}

//void MainWindow::testFFmpeg() {
//    // 1. 打印FFmpeg版本（验证库加载）
//    qDebug() << "=== FFmpeg 基础信息 ===";
//    qDebug() << "FFmpeg avutil 版本号（数字）：" << avutil_version();
//    qDebug() << "FFmpeg 版本字符串：" << av_version_info();
//    qDebug() << "libavformat 版本：" << avformat_version();

//    // 2. 核心修复：FFmpeg 3.x必须手动注册，强制调用（删除所有版本判断）
//    av_register_all();
//    // 可选：额外注册编解码器（确保编解码模块也初始化）
//    avcodec_register_all();

//    // 3. 遍历并统计输入封装格式（添加打印，便于验证）
//    AVInputFormat *fmt = nullptr;
//    int fmtCount = 0;
//    while ((fmt = av_iformat_next(fmt))) {
//        fmtCount++;
//        // 打印支持的封装格式名称，直观验证
//        qDebug() << "支持的封装格式：" << fmt->name;
//    }

//    // 4. 验证结果
//    qDebug() << "=== FFmpeg 功能验证 ===";
//    qDebug() << "FFmpeg 支持的输入封装格式数量：" << fmtCount;
//    if (fmtCount > 0) {
//        qDebug() << "✅ FFmpeg库调用正常，功能可用！";
//    } else {
//        qDebug() << "❌ FFmpeg库初始化失败！";
//    }
//}

void MainWindow::on_btn_start_clicked()
{
    // 先启动摄像头，再获取参数
    emit startCamera("/dev/video0"); // 可根据实际设备调整
    // 添加延迟或等待信号确认摄像头已启动
    QTimer::singleShot(500, this, &MainWindow::onCameraStarted);
    updateFpsDisplay(supportedFps[currentFpsIndex]);

    ui->btn_capture->setEnabled(true);  // 摄像头启动后启用录制按钮
    ui->btn_record->setEnabled(true);  // 摄像头启动后启用录制按钮

    ui->btn_upload->setEnabled(true); // 启用上传按钮

    //testFFmpeg();

}

void MainWindow::on_btn_stop_clicked()
{
    emit stopCamera();

    ui->btn_record->setEnabled(false);  // 摄像头停止后禁用录制按钮

    isGrayRunning = false;
    isBinaryRunning = false;
    isCannyRunning = false;

    ui->btn_gray->setText("开始灰度化");
    ui->btn_binary->setText("开始二值化");
    ui->btn_canny->setText("开始边缘检测");

    ui->btn_upload->setEnabled(false);
    ui->btn_upload->setText("开始上传"); // 重置按钮文本


}

void MainWindow::on_btn_capture_clicked()
{
    emit captureImage();
}


void MainWindow::on_btn_select_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择图片"),
        "/root/Image_Process",
        tr("图片文件 (*.png *.jpg *.jpeg *.bmp);;所有文件 (*)"
    ));

    if (filePath.isEmpty()) return;

    // 停止摄像头
    emit stopCamera();

    QFileInfo fileInfo(filePath);
    QString suffix = fileInfo.suffix().toLower();  //得到小写的文件后缀名字符串

    // 处理图片
    if (suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "bmp") {
        QImage image(filePath);
        if (!image.isNull()) {
            ui->lb_display->setPixmap(QPixmap::fromImage(
                image.scaled(ui->lb_display->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
            ));

            //发送图像处理信号，处理完后在各个label上进行显示
            if(isGrayRunning || isBinaryRunning || isCannyRunning)
            {
                emit processImage(image, isGrayRunning, isBinaryRunning, isCannyRunning);
            }

        } else {
            QMessageBox::warning(this, "错误", "无法加载图片文件");
        }
    }
    else {
        QMessageBox::warning(this, "错误", "不支持的文件格式");
    }
}



void MainWindow::on_btn_gray_clicked()
{
//    if (!worker->isCameraRunning()) {
//        QMessageBox::information(this, "提示", "请先启动摄像头");
//        return;
//    }

    isGrayRunning = !isGrayRunning;
    ui->btn_gray->setText(isGrayRunning ? "停止灰度化" : "开始灰度化");
}


void MainWindow::on_btn_binary_clicked()
{

    isBinaryRunning = !isBinaryRunning;
    ui->btn_binary->setText(isBinaryRunning ? "停止二值化" : "开始二值化");
}



void MainWindow::on_btn_canny_clicked()
{

    isCannyRunning = !isCannyRunning;
    ui->btn_canny->setText(isCannyRunning ? "停止边缘检测" : "开始边缘检测");
}

void MainWindow::onCameraStarted()
{
    // 获取亮度默认值）
    int brightnessDefault = worker->getControlDefaultValue(V4L2_CID_BRIGHTNESS);
    // 获取对比度默认值
    int contrastDefault = worker->getControlDefaultValue(V4L2_CID_CONTRAST);
    // 获取饱和度默认值
    int saturationDefault = worker->getControlDefaultValue(V4L2_CID_SATURATION);
    // 获取伽马默认值
    int gammaDefault = worker->getControlDefaultValue(V4L2_CID_GAMMA);
    // 获取锐度默认值
    int sharpnessDefault = worker->getControlDefaultValue(V4L2_CID_SHARPNESS);


    // 检查默认值是否有效（无效则用中间值兜底）
    if (brightnessDefault == -1) brightnessDefault = 0; // -255~255的中间值
    if (contrastDefault == -1) contrastDefault = 15;    // 0~30的中间值
    if (saturationDefault == -1) saturationDefault = 63;  // 0~127的中间值
    if (gammaDefault == -1) gammaDefault = 135;  // 20~250的中间值
    if (sharpnessDefault == -1) sharpnessDefault = 7;  // 0~15的中间值


    // 设置亮度滑块
    ui->brightnessSlider->setRange(-255, 255);          // 匹配摄像头亮度范围
    ui->brightnessSlider->setValue(brightnessDefault);   // 设为默认值
    ui->lb_brightness->setText(QString("%1").arg(brightnessDefault));

    // 设置对比度滑块
    ui->contrastSlider->setRange(0, 30);                // 匹配摄像头对比度范围
    ui->contrastSlider->setValue(contrastDefault);       // 设为默认值
    ui->lb_contrast->setText(QString("%1").arg(contrastDefault));

    // 设置饱和度滑块
    ui->saturationSlider->setRange(0, 127);               // 设置饱和度范围
    ui->saturationSlider->setValue(saturationDefault);    // 设置默认值
    ui->lb_saturation->setText(QString("%1").arg(saturationDefault));

    // 设置伽马滑块
    ui->gammaSlider->setRange(20, 250);
    ui->gammaSlider->setValue(gammaDefault);    // 设置默认值（摄像头硬件默认值）
    ui->lb_gamma->setText(QString("%1").arg(gammaDefault));

    // 设置锐度滑块
    ui->sharpnessSlider->setRange(0, 15);              // 锐度范围0~15
    ui->sharpnessSlider->setValue(sharpnessDefault);
    ui->lb_sharpness->setText(QString("%1").arg(sharpnessDefault));


    // 启用滑块
    ui->brightnessSlider->setEnabled(true);
    ui->contrastSlider->setEnabled(true);
    ui->saturationSlider->setEnabled(true);
    ui->gammaSlider->setEnabled(true);
    ui->sharpnessSlider->setEnabled(true);

}

void MainWindow::on_brightnessSlider_valueChanged(int value)
{
    // 更新显示值
    ui->lb_brightness->setText(QString("%1").arg(value));

    // 检查摄像头是否运行
    if (worker && worker->isCameraRunning()) {
        // 通过V4L2设置亮度（标准控制ID）
        worker->setV4L2Control(V4L2_CID_BRIGHTNESS, value);
    }
}

void MainWindow::on_contrastSlider_valueChanged(int value)
{
    // 更新显示值
    ui->lb_contrast->setText(QString("%1").arg(value));

    // 检查摄像头是否运行
    if (worker && worker->isCameraRunning()) {
        // 通过V4L2设置对比度（标准控制ID）
        worker->setV4L2Control(V4L2_CID_CONTRAST, value);
    }
}

void MainWindow::on_saturationSlider_valueChanged(int value)
{
    // 更新显示值
    ui->lb_saturation->setText(QString("%1").arg(value));

    // 检查摄像头是否运行
    if (worker && worker->isCameraRunning()) {
        // 通过V4L2设置饱和度（标准控制ID）
        worker->setV4L2Control(V4L2_CID_SATURATION, value);
    }
}

void MainWindow::on_gammaSlider_valueChanged(int value)
{
    // 更新显示当前伽马值
    ui->lb_gamma->setText(QString("%1").arg(value));

    // 检查摄像头是否运行，运行时才调节
    if (worker && worker->isCameraRunning()) {
        // 通过V4L2接口设置伽马值（标准控制ID：V4L2_CID_GAMMA）
        worker->setV4L2Control(V4L2_CID_GAMMA, value);
    }
}

void MainWindow::on_sharpnessSlider_valueChanged(int value)
{
    // 更新显示值
    ui->lb_sharpness->setText(QString("%1").arg(value));

    // 检查摄像头是否运行
    if (worker && worker->isCameraRunning()) {
        // 通过V4L2设置锐度（标准控制ID）
        worker->setV4L2Control(V4L2_CID_SHARPNESS, value);
    }
}



void MainWindow::on_btn_fps_decrease_clicked()
{
    if (!worker->isCameraRunning()) return; // 确保摄像头在运行

    if (currentFpsIndex > 0) {
        currentFpsIndex--;
        int newFps = supportedFps[currentFpsIndex];

        if (worker) {
            emit setFrameRateSignal(newFps);
        }

        updateFpsDisplay(newFps);
    }
}

void MainWindow::on_btn_fps_increase_clicked()
{
    if (!worker->isCameraRunning()) return; // 确保摄像头在运行

    if (currentFpsIndex < supportedFps.size() - 1) {
        currentFpsIndex++;
        int newFps = supportedFps[currentFpsIndex];

        // 更新定时器间隔
        if (worker) {
            emit setFrameRateSignal(newFps);
        }

        updateFpsDisplay(newFps);
    }
}

void MainWindow::updateFpsDisplay(int fps)
{
    ui->lb_fps->setText(QString("%1").arg(fps));
}


void MainWindow::on_btn_record_clicked()
{
    if (!worker->isCameraRunning()) {
        QMessageBox::information(this, "提示", "请先启动摄像头");
        return;
    }

    if (!isRecording) {
        // 开始录制
        isRecording = true;
        ui->btn_record->setText("停止录制");
        emit startRecordSignal();
    } else {
        // 停止录制
        isRecording = false;
        ui->btn_record->setText("录制");
        emit stopRecordSignal();

        // 让用户选择保存路径（仍在 UI 线程）
        QString defaultFileName = QString("video_%1.mp4")
                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        QString fileName = QFileDialog::getSaveFileName(this, tr("保存视频"), defaultFileName,
                                                        tr("MP4 Files (*.mp4)"));
        if (fileName.isEmpty()) {
            QMetaObject::invokeMethod(worker, "clearRecordFrames", Qt::QueuedConnection);
            return;
        }

        // 禁用按钮，防止重复操作
        ui->btn_record->setEnabled(false);
        ui->btn_start->setEnabled(false);
        ui->btn_stop->setEnabled(false);

        // 调用 worker 异步保存（线程安全）
        QMetaObject::invokeMethod(worker, "saveRecordedVideo", Qt::QueuedConnection,
                                  Q_ARG(QString, fileName));
    }
}


void MainWindow::on_btn_upload_clicked()
{
    if (!isConnectServer) {
        // 初始化连接，创建 QUdpSocket 对象
        udpSocket = new QUdpSocket();
        isConnectServer = true;
        ui->btn_upload->setText("断开连接");
        qDebug() << "准备发送数据到服务器";

        if(uploadServerThread == NULL)
        {
            uploadServerThread = new QThread(this);
            rtpH264Sender = new RtpH264Sender();
            rtpH264Sender->moveToThread(uploadServerThread);
            connect(rtpH264Sender, &RtpH264Sender::dataReadyToUpload, this, &MainWindow::uploadDataToServer);
            connect(this, &MainWindow::sendFrameToServer, rtpH264Sender, &RtpH264Sender::sendFrame, Qt::QueuedConnection);
            uploadServerThread->start();
        }
        // 创建新的定时器，连接到正确的定时器对象
        if (!uploadServerTimer) {  // 确保只创建一次定时器
            uploadServerTimer = new QTimer();
            connect(uploadServerTimer, &QTimer::timeout, this, &MainWindow::startUploadServer);
            uploadServerTimer->start(g_fps*10);
        }
    } else {
        // 断开连接
        isConnectServer = false;
        ui->btn_upload->setText("连接服务器");
        qDebug() << "停止发送数据";
    }
}

void MainWindow::startUploadServer() {

    if (isConnectServer) {
        // 从 `origal_label` 获取图像
        QLabel *origalLabel = ui->lb_display;
        QImage image = origalLabel->pixmap()->toImage();
        // 调整图像大小为 640x480
        resizedImageProcessImage = image.scaled(640, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        emit sendFrameToServer(resizedImageProcessImage);

    } else {
        // 停止定时器
        if (uploadServerTimer) {
            uploadServerTimer->stop();
            delete uploadServerTimer;
            uploadServerTimer = nullptr;
        }

    }
}

// 发送 RTP 数据
void MainWindow::uploadDataToServer(const QByteArray &data) {
    qDebug() << "Uploading data to server, size:" << data.size();

    // 通过 UDP 发送 RTP 包
    if (udpSocket->writeDatagram(data, QHostAddress("127.0.0.1"), 20000) == -1) {
        qDebug() << "Error sending RTP packet:" << udpSocket->errorString();
    } else {
        qDebug() << "RTP packet sent successfully!";
    }
}
