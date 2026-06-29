// imageprocessor.h
#ifndef IMAGEPROCESSOR_H
#define IMAGEPROCESSOR_H

#include <QObject>
#include <QImage>
#include <opencv2/opencv.hpp>

class ImageProcessor : public QObject
{
    Q_OBJECT
public:
    explicit ImageProcessor(QObject *parent = nullptr);

signals:
    // 分别发送处理后的图像
    void grayProcessed(const QImage &img);
    void binaryProcessed(const QImage &img);
    void cannyProcessed(const QImage &img);

public slots:
    // 统一处理入口（携带开关控制）
    void process(const QImage &img, bool doGray, bool doBinary, bool doCanny);
};

#endif // IMAGEPROCESSOR_H
