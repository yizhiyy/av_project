QT       += core gui multimedia network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    cameraworker.cpp \
    imageprocessor.cpp \
    main.cpp \
    mainwindow.cpp \
    rtph264sender.cpp

HEADERS += \
    cameraworker.h \
    imageprocessor.h \
    mainwindow.h \
    rtph264sender.h

FORMS += \
    mainwindow.ui


# ARM cross-compilation paths
FFMPEG_ARM_DIR = /home/book/ffmpeg/ffmpeg_install
X264_ARM_DIR = /home/book/ffmpeg/x264_install
OPENCV_ARM_DIR = /home/book/opencv-3.4.1_install_arm/install

contains(QMAKE_HOST.arch, x86_64) {
    # Local x86_64 development
    OPENCV_DIR = /home/yy/opencv_install
    INCLUDEPATH += $$OPENCV_DIR/include/opencv4
    LIBS += -L$$OPENCV_DIR/lib -lopencv_core -lopencv_imgproc -lopencv_videoio -lopencv_imgcodecs
    LIBS += -lavformat -lavcodec -lavutil -lswscale -lswresample
    LIBS += -lpthread -Wl,-rpath=$$OPENCV_DIR/lib
} else {
    # ARM cross-compilation
    INCLUDEPATH += $$FFMPEG_ARM_DIR/include $$X264_ARM_DIR/include $$OPENCV_ARM_DIR/include
    LIBS += -L$$FFMPEG_ARM_DIR/lib -lavformat -lavcodec -lavutil -lswscale -lswresample
    LIBS += -L$$X264_ARM_DIR/lib -lx264
    LIBS += -L$$OPENCV_ARM_DIR/lib -lopencv_core -lopencv_imgproc -lopencv_videoio -lopencv_imgcodecs
    LIBS += -L/opt/fsl-imx-x11/4.1.15-2.1.0/sysroots/cortexa7hf-neon-poky-linux-gnueabi/usr/lib -lbz2
    LIBS += -lpthread -Wl,-rpath-link=$$FFMPEG_ARM_DIR/lib
}



# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
