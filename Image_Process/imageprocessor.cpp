#include "imageprocessor.h"

ImageProcessor::ImageProcessor(QObject *parent) : QObject(parent)
{

}


void ImageProcessor:: process(const QImage &img, bool doGray, bool doBinary, bool doCanny)
{
    if (img.isNull()) return;

    // 1. 转换QImage到OpenCV Mat（复用你现有转换逻辑）
    QImage imgCopy = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat mat(imgCopy.height(), imgCopy.width(), CV_8UC3,
                const_cast<uchar*>(imgCopy.bits()), imgCopy.bytesPerLine());
    cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);

    // 2. 灰度化（按需执行）
    cv::Mat grayMat;
    if (doGray) {
        cv::cvtColor(mat, grayMat, cv::COLOR_BGR2GRAY);
        // 转换回QImage发送
        QImage grayImg(grayMat.data, grayMat.cols, grayMat.rows,
                       grayMat.step, QImage::Format_Grayscale8);
        emit grayProcessed(grayImg.copy()); // 拷贝避免内存失效
    }

    // 3. 二值化（按需执行，依赖灰度结果）
    cv::Mat binaryMat;
    if (doBinary) {
        if (grayMat.empty()) cv::cvtColor(mat, grayMat, cv::COLOR_BGR2GRAY); // 兜底
        cv::threshold(grayMat, binaryMat, 127, 255, cv::THRESH_BINARY);
        QImage binaryImg(binaryMat.data, binaryMat.cols, binaryMat.rows,
                         binaryMat.step, QImage::Format_Grayscale8);
        emit binaryProcessed(binaryImg.copy());
    }

    // 4. 边缘检测（按需执行，依赖灰度结果）
    cv::Mat cannyMat;
    if (doCanny) {
        if (grayMat.empty()) cv::cvtColor(mat, grayMat, cv::COLOR_BGR2GRAY); // 兜底
        cv::Canny(grayMat, cannyMat, 50, 150);
        QImage cannyImg(cannyMat.data, cannyMat.cols, cannyMat.rows,
                        cannyMat.step, QImage::Format_Grayscale8);
        emit cannyProcessed(cannyImg.copy());
    }
}
