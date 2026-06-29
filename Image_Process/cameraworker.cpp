#include "cameraworker.h"

#include <QDebug>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <QDir>
#include <qendian.h>
#include <errno.h>
#include <string.h>


using namespace cv;
using namespace std;

CameraWorker::CameraWorker(QObject *parent) : QObject(parent)
{
    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &CameraWorker::captureFrame);
    timer->setInterval(1000 / currentFps); // ~30fps

}

CameraWorker::~CameraWorker()
{
    stop();
    delete timer;
}



void CameraWorker::start(const QString &device)
{
    QMutexLocker locker(&mutex);
    if (isRunning) {
        emit errorOccurred("摄像头已在运行");
        return;
    }
    currentDevice = device; // 保存设备路径
    if (initV4L2(device.toUtf8().constData())) {
        isRunning = true;
        timer->start();
    } else {
        emit errorOccurred("初始化摄像头失败");
    }
}

void CameraWorker::stop()
{
    QMutexLocker locker(&mutex);
    if (!isRunning) return;

    isRunning = false;
    timer->stop();

    // 停止视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    // 释放所有映射的缓冲区
    for (auto &buf : buffers) {
        munmap(buf.start, buf.length);
    }
    buffers.clear();

    if (fd != -1) {
        close(fd);
        fd = -1;
    }
}

bool CameraWorker::setFrameRate(int fps)
{
    // 1.检查是否是支持的帧率
    QList<int> supported = {5, 10, 15, 20, 25, 30};
    if (!supported.contains(fps)) {
        emit errorOccurred("不支持的帧率值");
        return false;
    }
    // 2. 仅在读取共享资源时加锁
    bool wasRunning;
    QString currentDevice;
    {
        QMutexLocker locker(&mutex); // 局部加锁，仅保护以下两行
        wasRunning = isRunning;
        currentDevice = this->currentDevice;
    }

    // 3. 调用stop()（此时已释放锁，stop()的锁不会冲突）
    if (wasRunning) {
        stop();
    }

    // 4. 仅在修改共享资源时加锁
    {
        QMutexLocker locker(&mutex);
        currentFps = fps;
        timer->setInterval(1000 / fps);
    }

    // 5. 重新启动（无锁状态）
    if (wasRunning) {
        start(currentDevice); // start()内部加锁，无冲突
    }

    return true;
}

void CameraWorker::capture()
{
    QMutexLocker locker(&mutex);
    if (currentImg.isNull()) {
        emit errorOccurred("无有效图像可保存");
        return;
    }

    // 确保目录存在
    QDir dir(QDir::homePath() + "/Pictures/");
    if (!dir.exists() && !dir.mkpath(".")) {
        emit errorOccurred("无法创建保存目录");
        return;
    }

    QString filename = QString(QDir::homePath() + "/Pictures/pic_%1.jpg")
            .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmsszzz"));
    if (!currentImg.save(filename)) {
        emit errorOccurred("保存图片失败: " + filename);
    } else {
        qDebug() << "图片保存成功: " << filename;
    }
}


void CameraWorker::captureFrame()
{
    if (!isRunning) return;

    QImage newImg = readFrame();
    if (newImg.isNull()) return;

    {
        QMutexLocker locker(&mutex);
        currentImg = newImg;
    }

    if (isRecording) {
        QMutexLocker locker(&mutex);
        // 直接使用 currentImg 的位数据构造 cv::Mat（Format_RGB32 内存为 BGRA）
        cv::Mat frame(currentImg.height(), currentImg.width(), CV_8UC4,
                      (void*)currentImg.bits(), currentImg.bytesPerLine());
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        // 录制容器需要独立拷贝
        recordFrames.push_back(bgr.clone());
        recordTimestamps.push_back(recordElapsedTimer.elapsed());
        qDebug() << "录制中，已保存帧数量：" << recordFrames.size()
                 << "时间戳：" << recordTimestamps.back() << "ms";
    }

    emit sendImage(currentImg);
}

bool CameraWorker::initV4L2(const char *dev)
{
    // 关闭已打开的设备
    if (fd != -1) {
        close(fd);
        fd = -1;
    }

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        emit errorOccurred("打开设备失败: " + QString(strerror(errno)));
        return false;
    }

    // 设置视频格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = frameWidth;
    fmt.fmt.pix.height = frameHeight;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;  // 修改为 NONE
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        emit errorOccurred("设置格式失败: " + QString(strerror(errno)));
        close(fd);
        fd = -1;
        return false;
    }

    // 设置帧率
    struct v4l2_streamparm streamparm;
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = currentFps;
    if (ioctl(fd, VIDIOC_S_PARM, &streamparm) < 0) {
        qWarning() << "设置帧率失败，将使用默认帧率: " << strerror(errno);
    }

    // 验证实际设置的尺寸
    frameWidth = fmt.fmt.pix.width;
    frameHeight = fmt.fmt.pix.height;
    qDebug() << "实际视频尺寸: " << frameWidth << "x" << frameHeight;

    // 申请多个缓冲区（例如4个）
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        emit errorOccurred("申请缓冲区失败: " + QString(strerror(errno)));
        close(fd);
        fd = -1;
        return false;
    }

    // 清理旧的缓冲区（如果有）
    for (auto &buf : buffers) {
        munmap(buf.start, buf.length);
    }
    buffers.clear();

    // 循环映射所有缓冲区并加入队列
    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer v4l2_buf;
        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;
        v4l2_buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &v4l2_buf) < 0) {
            emit errorOccurred("查询缓冲区失败: " + QString(strerror(errno)));
            close(fd);
            fd = -1;
            return false;
        }

        Buffer buffer;
        buffer.length = v4l2_buf.length;
        buffer.start = (unsigned char*)mmap(nullptr, buffer.length,
                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                            fd, v4l2_buf.m.offset);
        if (buffer.start == MAP_FAILED) {
            emit errorOccurred("映射缓冲区失败: " + QString(strerror(errno)));
            close(fd);
            fd = -1;
            return false;
        }
        buffers.push_back(buffer);

        // 将缓冲区加入采集队列
        if (ioctl(fd, VIDIOC_QBUF, &v4l2_buf) < 0) {
            emit errorOccurred("队列缓冲区失败: " + QString(strerror(errno)));
            close(fd);
            fd = -1;
            return false;
        }
    }

    // 开始视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        emit errorOccurred("启动流失败: " + QString(strerror(errno)));
        // 清理已映射内存...
        for (auto &buf : buffers) {
            munmap(buf.start, buf.length);
        }
        buffers.clear();
        close(fd);
        fd = -1;
        return false;
    }

    return true;
}


QImage CameraWorker::readFrame()
{
    if (fd == -1) return QImage();

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // 取出一个已填充好的缓冲区（可能阻塞直到有新帧）
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return QImage();  // 无新帧，返回空图像
        emit errorOccurred("取出缓冲区失败: " + QString(strerror(errno)));
        return QImage();
    }

    if (buf.index >= buffers.size()) return QImage();

    // 使用当前缓冲区的数据进行 YUYV -> RGB 转换
    QImage img = yuyvToRgb(buffers[buf.index].start, frameWidth, frameHeight, buffers[buf.index].length);

    // 重新将缓冲区入队，供下一帧使用
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        emit errorOccurred("重新队列缓冲区失败: " + QString(strerror(errno)));
        // 即使入队失败，仍然返回图像（尽力而为）
    }

    return img;
}


QImage CameraWorker::yuyvToRgb(unsigned char *data, int w, int h, size_t bufferSize)
{
    QImage img(w, h, QImage::Format_RGB32);
    uchar *ptr = img.bits();
    size_t totalBytes = w * h * 2;

    if (bufferSize < totalBytes) {
        emit errorOccurred("缓冲区大小不足");
        return img;
    }

    for (int i = 0; i < h; ++i) {
        for (int j = 0; j < w; j += 2) {
            size_t pos = i * w * 2 + j * 2;
            if (pos + 3 >= totalBytes) {
                emit errorOccurred("YUYV数据越界");
                return img;
            }

            int y0 = data[pos];
            int u  = data[pos + 1] - 128;
            int y1 = data[pos + 2];
            int v  = data[pos + 3] - 128;

            // 定点系数 (8位精度)
            // R = 1.402 * V  → (359 * V) >> 8
            // G = -0.344*U -0.714*V → -(88*U + 183*V) >> 8
            // B = 1.772 * U  → (454 * U) >> 8
            auto clamp = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };

            int r0 = y0 + ((359 * v) >> 8);
            int g0 = y0 - ((88 * u + 183 * v) >> 8);
            int b0 = y0 + ((454 * u) >> 8);
            ptr[0] = clamp(b0);
            ptr[1] = clamp(g0);
            ptr[2] = clamp(r0);
            ptr[3] = 0;
            ptr += 4;

            int r1 = y1 + ((359 * v) >> 8);
            int g1 = y1 - ((88 * u + 183 * v) >> 8);
            int b1 = y1 + ((454 * u) >> 8);
            ptr[0] = clamp(b1);
            ptr[1] = clamp(g1);
            ptr[2] = clamp(r1);
            ptr[3] = 0;
            ptr += 4;
        }
    }
    return img;
}

// V4L2控制通用函数
bool CameraWorker::setV4L2Control(int controlId, int value)
{
    struct v4l2_control ctrl;
    ctrl.id = controlId;
    ctrl.value = value;

    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == -1) {
        qDebug() << "设置控制失败，ID:" << controlId << "错误:" << strerror(errno);
        return false;
    }
    return true;
}

#ifdef DEBUG
// 检查设备是否支持指定控制项
bool CameraWorker::checkControlSupport(int controlId) {
    struct v4l2_queryctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = controlId;

    if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == 0) {
        // 检查控制项是否可用
        return !(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED);
    }
    return false;
}


// 列出所有支持的控制项（调试用）
void CameraWorker::listAllControls() {
    struct v4l2_queryctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;

    qDebug() << "设备支持的控制项列表：";
    while (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == 0) {
        if (queryctrl.id == V4L2_CTRL_FLAG_NEXT_CTRL) break;

        qDebug() << "ID: " << queryctrl.id
                 << " 名称: " << (char*)queryctrl.name
                 << " 类型: " << queryctrl.type
                 << " 最小值: " << queryctrl.minimum
                 << " 最大值: " << queryctrl.maximum;

        // 移动到下一个控制项
        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
}
#endif

// 获取控制项的默认值
int CameraWorker::getControlDefaultValue(int controlId)
{
    QMutexLocker locker(&mutex);
    if (fd == -1) {
        qDebug() << "获取默认值失败：摄像头未初始化";
        return -1; // 无效值
    }

    struct v4l2_queryctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = controlId;

    if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == -1) {
        qDebug() << "查询控制项失败，ID:" << controlId << "错误:" << strerror(errno);
        return -1; // 无效值
    }
    return queryctrl.default_value; // 返回硬件默认值
}


// 开始录制
void CameraWorker::startRecording() {
    QMutexLocker locker(&mutex);
    isRecording = true;
    recordFrames.clear();
    recordTimestamps.clear();
    recordElapsedTimer.start();  // 开始计时
    qDebug() << "开始录制视频，计时器已启动";
}

// 停止录制
void CameraWorker::stopRecording() {
    QMutexLocker locker(&mutex);
    isRecording = false;
    qDebug() << "停止录制，累计帧数量：" << recordFrames.size()
             << "总时长：" << recordElapsedTimer.elapsed() << "ms";
}

void CameraWorker::saveRecordedVideo(const QString &fileName) {
    QMutexLocker locker(&mutex); // 保护 recordFrames / recordTimestamps

    if (recordFrames.empty() || recordTimestamps.empty()) {
        emit videoSaved(false, "无录制帧可保存");
        return;
    }

    // 计算平均帧率
    qint64 totalDuration = recordTimestamps.back() - recordTimestamps.front();
    double actualFps = (totalDuration > 0) ? (recordFrames.size() * 1000.0 / totalDuration) : 30.0;
    int fps = qRound(actualFps);
    fps = qBound(1, fps, 60);

    cv::Size frameSize = recordFrames[0].size();
    int fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');

    cv::VideoWriter writer;
    writer.open(fileName.toStdString(), fourcc, fps, frameSize);
    if (!writer.isOpened()) {
        emit videoSaved(false, QString("无法打开视频编码器，fps=%1，尺寸=%2x%3")
                        .arg(fps).arg(frameSize.width).arg(frameSize.height));
        return;
    }

    // 逐帧写入（重复帧逻辑）
    qint64 lastTimestamp = recordTimestamps[0];
    for (size_t i = 0; i < recordFrames.size(); ++i) {
        qint64 currentTimestamp = recordTimestamps[i];
        int repeatCount = qRound((currentTimestamp - lastTimestamp) * fps / 1000.0);
        repeatCount = qMax(1, repeatCount);
        for (int r = 0; r < repeatCount; ++r) {
            writer.write(recordFrames[i]);
        }
        lastTimestamp = currentTimestamp;
    }

    writer.release();

    // 保存成功后清空缓存
    recordFrames.clear();
    recordTimestamps.clear();

    emit videoSaved(true, QString("视频已保存至：%1").arg(fileName));
}


// 清空帧
void CameraWorker::clearRecordFrames() {
    QMutexLocker locker(&mutex);
    recordFrames.clear();
    recordTimestamps.clear();
    qDebug() << "录制缓存已清空";
}


