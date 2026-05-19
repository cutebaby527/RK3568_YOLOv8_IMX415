# MQTT 协议说明文档

> 本文档记录 RK3568 EdgeGuard 人员监测系统当前使用的 MQTT Topic、Payload 格式、字段含义和调试命令。  
> MQTT Broker 默认运行在开发板本机。

---

## 1. MQTT Broker 配置

默认配置：

```text
host: 127.0.0.1
port: 1883
```

启动 Mosquitto：

```bash
sudo systemctl restart mosquitto
systemctl status mosquitto --no-pager
```

监听全部 MQTT 消息：

```bash
mosquitto_sub -h 127.0.0.1 -t "#" -v
```

---

## 2. Topic 总览

当前系统使用 3 个核心 Topic：

| Topic                 | 方向    | 发布方             | 订阅方                  | 用途               |
| --------------------- | ------- | ------------------ | ----------------------- | ------------------ |
| `edge/detect`         | publish | `edge_ai_pipeline` | `web_viewer` / 调试端   | 原始检测结果       |
| `edge/person/alarm`   | publish | `edge_ai_pipeline` | `web_viewer` / 告警平台 | 结构化人员入侵告警 |
| `edge/person/metrics` | publish | `edge_ai_pipeline` | `web_viewer` / 监控平台 | 系统运行指标       |

---

## 3. 原始检测结果：edge/detect

### 3.1 Topic

```text
edge/detect
```

### 3.2 用途

该 Topic 用于发布每帧或近实时检测结果，主要用于：

```text
1. Web 页面 Raw MQTT Events 显示；
2. Web 端检测框调试；
3. 外部系统验证模型检测输出；
4. 排查 person 置信度、bbox 坐标和类别映射问题。
```

### 3.3 订阅命令

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
```

### 3.4 Payload 示例

```json
{
  "seq": 123,
  "objects": [
    {
      "class": "person",
      "conf": 0.873,
      "box": [178, 92, 321, 431]
    },
    {
      "class": "car",
      "conf": 0.762,
      "box": [20, 100, 120, 300]
    }
  ]
}
```

### 3.5 字段说明

| 字段              | 类型    | 说明                                    |
| ----------------- | ------- | --------------------------------------- |
| `seq`             | integer | 当前检测帧序号                          |
| `objects`         | array   | 当前帧检测到的目标数组                  |
| `objects[].class` | string  | 目标类别名称，例如 `person`             |
| `objects[].conf`  | number  | 检测置信度，范围一般为 0~1              |
| `objects[].box`   | array   | 检测框坐标 `[left, top, right, bottom]` |

### 3.6 注意事项

```text
1. edge/detect 是原始检测结果，不代表一定触发告警。
2. 是否触发告警还需要经过 RuleEngine 判断。
3. RuleEngine 会进一步检查 class、confidence、ROI、dwell_time_ms 和 cooldown_ms。
```

---

## 4. 结构化告警事件：edge/person/alarm

### 4.1 Topic

```text
edge/person/alarm
```

### 4.2 用途

该 Topic 用于发布业务告警事件。

只有满足以下条件时才会发布：

```text
1. 检测类别等于 config.json 中 rule.target_class，当前为 person；
2. 检测置信度大于等于 model.conf_threshold；
3. bbox 中心点位于 rule.roi 多边形内；
4. 目标停留时间超过 rule.dwell_time_ms；
5. 当前不处于 rule.cooldown_ms 冷却周期内。
```

### 4.3 订阅命令

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
```

### 4.4 Payload 示例

```json
{
  "event_id": "20260518-200221-0001",
  "event_type": "person_intrusion",
  "camera_id": "rk3568-imx415-01",
  "timestamp": 1779110640405,
  "level": "warning",
  "object": {
    "class": "person",
    "confidence": 0.873,
    "bbox": [178, 92, 321, 431]
  },
  "rule": {
    "roi_name": "restricted_area_1",
    "dwell_time_ms": 1100
  },
  "snapshot": "../events/20260518-200221-0001.jpg"
}
```

### 4.5 字段说明

| 字段                 | 类型    | 说明                                          |
| -------------------- | ------- | --------------------------------------------- |
| `event_id`           | string  | 告警事件 ID，格式为 `YYYYMMDD-HHMMSS-序号`    |
| `event_type`         | string  | 告警类型，当前为 `person_intrusion`           |
| `camera_id`          | string  | 摄像头 ID，当前默认 `rk3568-imx415-01`        |
| `timestamp`          | integer | 告警时间戳，单位毫秒                          |
| `level`              | string  | 告警级别，当前为 `warning`                    |
| `object.class`       | string  | 触发告警的目标类别                            |
| `object.confidence`  | number  | 触发告警的目标置信度                          |
| `object.bbox`        | array   | 触发告警的检测框 `[left, top, right, bottom]` |
| `rule.roi_name`      | string  | ROI 区域名称                                  |
| `rule.dwell_time_ms` | integer | 目标在 ROI 内持续停留时间                     |
| `snapshot`           | string  | 告警截图相对路径                              |

### 4.6 Snapshot 访问方式

MQTT 中的截图路径通常为：

```text
../events/<event_id>.jpg
```

开发板实际文件路径：

```bash
/userdata/RK3568_Yolo/events/<event_id>.jpg
```

Web 访问路径：

```text
http://<开发板IP>:8080/snapshot/<event_id>.jpg
```

示例：

```text
http://192.168.1.100:8080/snapshot/20260518-200221-0001.jpg
```

---

## 5. 系统运行指标：edge/person/metrics

### 5.1 Topic

```text
edge/person/metrics
```

### 5.2 用途

该 Topic 用于周期性发布系统运行指标。当前默认每 5 秒发布一次。

主要用于：

```text
1. Web 页面 System Metrics 显示；
2. 查看推理 FPS；
3. 查看累计告警次数；
4. 查看 MQTT 发布计数；
5. 判断系统是否持续运行。
```

### 5.3 订阅命令

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

### 5.4 Payload 示例

```json
{
  "uptime_sec": 25,
  "inference_fps": 19.57,
  "inference_frame_count": 486,
  "alarm_count": 3,
  "mqtt_detect_publish_count": 486,
  "mqtt_alarm_publish_count": 3,
  "last_alarm_ts": 1779110646471
}
```

### 5.5 字段说明

| 字段                        | 类型    | 说明                                         |
| --------------------------- | ------- | -------------------------------------------- |
| `uptime_sec`                | integer | 程序运行时长，单位秒                         |
| `inference_fps`             | number  | 最近统计周期内的推理 FPS                     |
| `inference_frame_count`     | integer | 累计推理帧数                                 |
| `alarm_count`               | integer | 累计告警次数                                 |
| `mqtt_detect_publish_count` | integer | 原始检测 MQTT 发布次数                       |
| `mqtt_alarm_publish_count`  | integer | 告警 MQTT 发布成功次数                       |
| `last_alarm_ts`             | integer | 最近一次告警时间戳，单位毫秒；没有告警时为 0 |

---

## 6. 配置文件中的 MQTT 参数

配置位置：

```bash
/userdata/RK3568_Yolo/config/config.json
```

相关字段：

```json
"mqtt": {
  "host": "127.0.0.1",
  "port": 1883,
  "topic_detect": "edge/detect",
  "topic_alarm": "edge/person/alarm",
  "topic_status": "edge/person/status",
  "topic_metrics": "edge/person/metrics"
}
```

字段说明：

| 字段            | 说明                 |
| --------------- | -------------------- |
| `host`          | MQTT Broker 地址     |
| `port`          | MQTT Broker 端口     |
| `topic_detect`  | 原始检测结果 Topic   |
| `topic_alarm`   | 告警事件 Topic       |
| `topic_status`  | 状态 Topic，当前预留 |
| `topic_metrics` | 系统指标 Topic       |

修改配置后需要重启服务：

```bash
sudo systemctl restart edge-person-monitor
sudo systemctl restart edge-person-web
```

---

## 7. Web Viewer 订阅关系

`web_viewer` 当前会订阅：

```text
edge/detect
edge/person/alarm
edge/person/metrics
```

启动命令：

```bash
cd /userdata/RK3568_Yolo/install

export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

./web_viewer   127.0.0.1   1883   edge/detect   8080   edge/person/alarm   edge/person/metrics   ../config/config.json
```

正常启动日志：

```text
[web] MQTT connected: 127.0.0.1:1883 detect_topic=edge/detect alarm_topic=edge/person/alarm metrics_topic=edge/person/metrics
```

---

## 8. 调试命令速查

### 8.1 监听全部 Topic

```bash
mosquitto_sub -h 127.0.0.1 -t "#" -v
```

### 8.2 监听原始检测

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
```

### 8.3 监听告警事件

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/alarm" -v
```

### 8.4 监听系统指标

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/person/metrics" -v
```

### 8.5 手动发布测试消息

测试 Web Raw MQTT Events：

```bash
mosquitto_pub -h 127.0.0.1 -t "edge/detect" -m '{"seq":1,"objects":[]}'
```

测试 Web Alarm Events：

```bash
mosquitto_pub -h 127.0.0.1 -t "edge/person/alarm" -m '{"event_id":"test-0001","event_type":"person_intrusion","level":"warning","object":{"class":"person","confidence":0.99,"bbox":[10,10,100,100]},"rule":{"roi_name":"test","dwell_time_ms":1200},"snapshot":""}'
```

测试 Web System Metrics：

```bash
mosquitto_pub -h 127.0.0.1 -t "edge/person/metrics" -m '{"uptime_sec":10,"inference_fps":20.5,"inference_frame_count":205,"alarm_count":1,"mqtt_detect_publish_count":205,"mqtt_alarm_publish_count":1,"last_alarm_ts":1779110646471}'
```

---

## 9. 告警未触发排查

如果 `edge/detect` 有 person，但 `edge/person/alarm` 没有告警，按以下顺序排查。

### 9.1 检查 person 置信度

查看原始检测：

```bash
mosquitto_sub -h 127.0.0.1 -t "edge/detect" -v
```

如果 person 置信度低于：

```json
"conf_threshold": 0.45
```

则不会触发告警。

调试时可临时降低：

```json
"conf_threshold": 0.30
```

### 9.2 检查 ROI

当前 ROI：

```json
"roi": [[120, 80], [540, 80], [580, 420], [90, 420]]
```

判断逻辑使用 bbox 中心点。

如果中心点不在 ROI 内，不会触发告警。

调试时可临时改成全画面：

```json
"roi": [[10, 10], [630, 10], [630, 470], [10, 470]]
```

### 9.3 检查停留时间

当前配置：

```json
"dwell_time_ms": 1000
```

人员进入 ROI 后，需要持续停留超过该时间才会触发告警。

调试时可临时改成：

```json
"dwell_time_ms": 300
```

### 9.4 检查冷却时间

当前配置：

```json
"cooldown_ms": 5000
```

触发一次告警后，5 秒内不会重复触发。

调试时可临时改成：

```json
"cooldown_ms": 3000
```

---

## 10. 协议兼容性说明

### 10.1 edge/detect 保持兼容

`edge/detect` 保留原始检测结果，主要用于兼容旧 Web 或调试工具。

### 10.2 edge/person/alarm 是业务事件

业务系统应优先订阅：

```text
edge/person/alarm
```

而不是直接根据 `edge/detect` 判断告警。

### 10.3 edge/person/metrics 是运行状态

监控系统可订阅：

```text
edge/person/metrics
```

用于判断边缘端是否在线、FPS 是否正常、告警计数是否异常。

---

## 11. 后续可扩展字段

后续如需对接平台，可以考虑扩展以下字段。

### 11.1 告警事件扩展

```json
{
  "device_id": "rk3568-001",
  "site_id": "factory-a",
  "zone_id": "zone-1",
  "rule_id": "person-intrusion-001",
  "image_url": "http://<board-ip>:8080/snapshot/<event_id>.jpg"
}
```

### 11.2 Metrics 扩展

```json
{
  "cpu_usage": 35.2,
  "memory_usage": 48.1,
  "temperature": 62.5,
  "dropped_frames": 10,
  "mqtt_publish_latency_ms": 3
}
```

---

## 12. 当前协议版本

```text
version: 1.0
date: 2026-05-19
project: RK3568 EdgeGuard Person Monitoring
```