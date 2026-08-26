# av_project

实时音视频采集与低延时流媒体传输系统。适用于嵌入式 ARM/Linux 平台。

## 项目结构

- [sserver](sserver/) — 流媒体服务端：V4L2 采集、x264 编码、TCP/UDP/RTP 传输
- [sclient](sclient/) — 接收端客户端：FFmpeg 解码（H.264）、OpenGL 渲染（GLFW + ImGui）、NACK/FEC 丢包恢复、抖动缓冲

> 历史说明：早期 Qt 版本（`Image_Process` + `RtpH264Server`，含 OpenCV 图像处理与 Qt GUI 预览）已重构为上述 sserver/sclient 结构，旧代码保留在 git 历史中。
