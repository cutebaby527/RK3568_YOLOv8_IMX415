# RK3568 YOLOv8 IMX415 智能检测项目

## 项目简介

本项目基于 RK3568 开发板、ATK-MCIMX415 摄像头和 YOLOv8 RKNN 模型，实现摄像头实时采集、NPU 推理、Web 实时显示检测框和 MQTT 检测结果发布。

## 硬件环境

- 开发板：ATK-DLRK3568 / RK3568
- 摄像头：ATK-MCIMX415 V1.5
- 系统：Debian 11 aarch64
- NPU：Rockchip RKNN Runtime

## 软件链路

```text
IMX415 Camera
→ /dev/video0
→ 640x480 NV12
→ YOLOv8 RKNN
→ shared memory
→ web_viewer
→ browser
→ MQTT
