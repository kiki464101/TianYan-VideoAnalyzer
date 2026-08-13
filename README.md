# 天眼（TianYan）— 基于 Qt 6 + OpenCV 的智能视频分析系统

实时视频流智能分析工具：多路视频接入、双 DNN 多目标检测、人物全貌特征提取、事件关键帧入库、DeepSeek AI 摘要与智能检索回放。

[C++17][Qt][OpenCV][Platform]

---

## ✨ 功能总览

| 模块 | 能力 |
| --- | --- |
| **视频接入** | USB 摄像头 / 本地视频文件；微信 HEVC 视频自动 ffmpeg 转码 MJPG 兜底播放 |
| **多目标检测** | MobileNet-SSD 人体 + ResNet-10 人脸双 OpenCV DNN 并行检测，NMS 去重，P1/P2 多目标编号 |
| **跨帧跟踪** | IoU 贪心匹配（简化 SORT）：同一个人跨帧沿用稳定编号，消失 >5 帧轨迹回收，编号从 P1 连续 |
| **人物特征** | 每人独立分析：上衣/下装/鞋 HSV 颜色分类 + Gabor 4×3 滤波器组条纹/格子纹理识别 |
| **事件驱动** | MOG2 运动检测 + 轮廓面积阈值触发；DNN 与运动检测解耦（静止人物同样出框）；多人画面共享关键帧、逐人独立事件 |
| **数据存储** | SQLite WAL 模式 + 批量缓冲写入；`timestamp_ms`（显示）+ `video_position_ms`（帧级定位）双时间戳 |
| **AI 摘要** | DeepSeek 结构化"智能侦查"prompt，生成摘要 + 关键词；独立 `summaries` 表（不覆盖历史） |
| **智能检索** | 双路模糊搜索（事件描述 + 摘要关键词反查 `event_ids`）覆盖全历史；点击定位 + 上一处/下一处循环跳转 |
| **智能播放** | 0.3x–2.0x 倍速、进度条事件红点、悬停时间预览、±5s 步进、暂停 |
| **工程化** | 多线程流水线（采集/检测/UI 经 Channel 解耦）、模型缺失/视频损坏优雅降级、SQLite 老库自动 ALTER 迁移 |

---

## 🏗 架构设计

```
┌───────────────── 采集层 StreamManager ─────────────────┐
│  USB摄像头 / 本地文件 / 微信HEVC→MJPG(ffmpeg转码)       │
│  倍速控制(原子 m_speed) / seek / 暂停 / 非阻塞停止       │
└────────────────────────┬───────────────────────────────┘
                         ▼ 帧
┌───────────────── 检测层 EventDetector ─────────────────┐
│  MOG2 运动检测(每2帧) + 轮廓面积阈值                     │
│  FeatureExtractor:                                      │
│    人体 MobileNet-SSD 300×300 (conf≥0.3)               │
│    人脸 ResNet-10 SSD 300×300 (conf≥0.4)               │
│    NMS 去重 → 多目标框 → 最近人脸配对                    │
│    跨帧跟踪: IoU 贪心匹配, P 编号跨帧稳定                │
│    每人: HSV 颜色(上衣/下装/鞋) + Gabor 4×3 纹理         │
│    共享关键帧(画全部框 + P1/P2 编号)                     │
└────────────────────────┬───────────────────────────────┘
                         ▼ EventRecord 信号
┌───────────────── 控制层 VideoAnalyzerController ────────┐
│  mediator：线程桥接 / 填 video_position_ms / 共享关键帧   │
│  LRU 缓存(timestampMs→keyframePath) / LLM 编排            │
└──────────┬──────────────────────────┬───────────────────┘
           ▼                          ▼
┌──────── 存储层 ────────┐  ┌──────── AI 层 ──────────────┐
│ DatabaseManager         │  │ LLMClient (QNetworkAccess) │
│ SQLite WAL + 批量缓冲    │  │ DeepSeek chat/completions  │
│ events 表(事件+关键帧)   │  │ 智能侦查 prompt + 关键词    │
│ summaries 表(摘要,独立)  │  └────────────────────────────┘
└────────┬────────────────┘
         ▼ 信号
┌───────────────── UI 层 MainWindow ─────────────────────┐
│  实时画面(annotateFrame 缓存框) / 事件列表 / AI摘要 Tab   │
│  智能检索 Tab(双路搜索 + 跳转) / 倍速 / 进度条红点        │
│  参数调优 Tab(置信度/面积阈值/API Key)                    │
└─────────────────────────────────────────────────────────┘
```

> 📄 交互式架构图：`docs/architecture.html`（浏览器打开）

---

## 🧱 模块说明

| 文件 | 职责 |
| --- | --- |
| `StreamManager` | 视频采集线程：OpenCV VideoCapture 逐帧读取、倍速（原子变量）、seek/暂停、元数据、ffmpeg 转码 fallback |
| `EventDetector` | 事件引擎：MOG2 运动检测、DNN 检测调度（与运动解耦）、检测框缓存 + `annotateFrame` 实时画框、多事件触发；**跨帧跟踪**（IoU 贪心匹配，P 编号跨帧稳定） |
| `FeatureExtractor` | 双 DNN 推理 + NMS + 最近人脸配对；`analyzeClothing` 三区 HSV 颜色分类；`textureOf` Gabor 4×3 纹理识别 |
| `DatabaseManager` | SQLite（WAL）+ 内存批量缓冲 + 事务落库；`summaries` 独立表；双路搜索（description + keywords 反查） |
| `LLMClient` | 异步 POST DeepSeek `/chat/completions`，结构化摘要 + 关键词提取 |
| `VideoAnalyzerController` | 系统中枢：信号桥接、`video_position_ms` 填充、共享关键帧 LRU 缓存、摘要编排 |
| `MainWindow` | 四 Tab UI：事件记录 / AI 摘要 / 智能检索 / 参数调优；播放控制 + 进度条标记 + 检索跳转 |
| `Channel` / `ThreadPool` / `EventLoop` | 线程间通信与任务调度基础设施 |

---

## 🔧 构建

### 环境要求

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| CMake | ≥ 3.20 | 构建系统 |
| Qt | 6.8.3 (MinGW 13.1.0) | Widgets / Sql / Network 模块 |
| OpenCV | 4.5.5 (MinGW x64) | `E:/share/opencv`，`libopencv_world455` |
| MinGW | 13.1.0 | 编译器（`E:/Qt/Tools/mingw1310_64`） |
| ffmpeg | ≥ 4.0（可选） | 微信 HEVC 视频转码兜底 |

### 编译步骤（Windows + MinGW）

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=E:/Qt/Tools/mingw1310_64/bin/g++.exe \
  -DCMAKE_C_COMPILER=E:/Qt/Tools/mingw1310_64/bin/gcc.exe \
  -DCMAKE_MAKE_PROGRAM=E:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe

cmake --build build
```

> CMake 内 `CMAKE_PREFIX_PATH` / `OpenCV_DIR` 已指向本机路径，如环境不同请调整 `CMakeLists.txt` 顶部配置。

---

## 🚀 运行

1. **准备模型**（双 DNN，缺失时自动优雅降级，仅影响人体/人脸检测）：

   ```
   E:/VideoProject/models/
   ├── MobileNetSSD_deploy.prototxt        (44KB, chuanqi305 官方版)
   ├── mobilenet_iter_73000.caffemodel     (23.3MB, 人体检测)
   ├── deploy.prototxt                     (28KB, 人脸)
   └── res10_300x300_ssd_iter_140000.caffemodel (10.6MB, 人脸检测)
   ```

2. **配置 API Key**：程序「参数调优」Tab 填入 DeepSeek API Key（默认端点 `https://api.deepseek.com/chat/completions`）

3. **启动**：`./build/VideoAnalyzer.exe`（首次运行自动创建 `E:/VideoProject/video.db` 与 `E:/pics/` 关键帧目录）

4. **使用流程**：
   - 「选择视频文件」→「▶ 启动采集」→ 实时画面出现蓝框（人体）/绿框（人脸）+ P1/P2 编号
   - 检测到人物自动截图入库，右侧「事件记录」实时刷新
   - 播放一段时间后「✨ 生成摘要」→「AI 摘要」Tab 查看 + 摘要相关事件列表（单击跳转）
   - 「智能检索」输入关键词（如"蓝色条纹上衣"）→ 双路搜索全历史 → 单击定位 / 上一处 / 下一处

---

## 📁 数据存储

```
E:/VideoProject/
├── video.db            # SQLite (WAL)：events + summaries 两表
├── pics/               # 关键帧图片（BGR 直存，多人共享一张）
└── models/             # DNN 模型文件
```

### events 表
| 字段 | 说明 |
| --- | --- |
| `timestamp_ms` | 事件发生 epoch 毫秒（显示用） |
| `video_position_ms` | 视频内位置 ms（帧级跳转定位用） |
| `description` | 智能描述（人物编号 + 位置 + 特征） |
| `keyframe_path` | 关键帧图片路径（多人事件共享同一张） |
| `generated_summary` / `keywords` | 兼容回填的摘要字段 |

### summaries 表（方案 B：与 events 解耦）
| 字段 | 说明 |
| --- | --- |
| `summary_text` | LLM 生成的自然语言摘要 |
| `keywords` | 逗号分隔关键词（搜索路径 B） |
| `event_ids` | 本次摘要涉及的事件 id 列表（反查定位） |

搜索双路：**路径 A** 匹配 `events.description`；**路径 B** 匹配 `summaries.keywords` → 反查 `event_ids` → 全历史覆盖。

---

## 🛠 关键技术决策

1. **DNN 与运动检测解耦**：DNN 每 2 帧无条件推理并更新缓存框——人站着不动也有框，事件帧特征与画面同步
2. **多人共享关键帧**：同帧 N 条事件共用一张画好全部框的图，只写盘一次（Controller LRU 缓存按 timestampMs 复用）
3. **双时间戳**：`timestamp_ms`（epoch，展示）与 `video_position_ms`（视频内 ms，跳转）分离，解决 seek 语义错位
4. **summaries 独立表**：每次摘要追加一行不覆盖历史，关键词搜索反查 `event_ids` 命中历史事件
5. **Gabor 纹理识别**：4 方向 × 3 尺度滤波器组 + 响应图标准差 + 正交对对比率，区分条纹/格子/纯色
6. **SQLite WAL + 批量缓冲**：事件高频写入不阻塞检测线程，事务批量落库
7. **优雅降级**：模型缺失 / 视频损坏 / API Key 为空，均有 qWarning + UI 提示，不影响其余功能
8. **跨帧跟踪（简化 SORT）**：新检测框与上帧轨迹 IoU 贪心匹配（>0.3 沿用 ID），未匹配开新轨迹，连续 5 帧未匹配回收——P 编号跨帧稳定，事件描述中的人在同一画面多次出现时编号一致

---

## 📊 性能基准（tools/perf_bench.cpp，本机实测）

> 环境：i7 级 CPU，MinGW Debug 构建，模型 300×300 输入。DNN 耗时与分辨率无关（blob 固定 300×300），MOG2 随分辨率线性增长。

| 分辨率 | 人体检测 | 人脸检测 | 特征提取 | MOG2 | 端到端 | FPS |
| --- | --- | --- | --- | --- | --- | --- |
| 360p | ≈38.9ms | ≈24.7ms | ≈7.1ms | ≈13.4ms | 84.0ms | ~12 |
| 720p | ≈38.1ms | ≈24.2ms | ≈6.9ms | ≈41.0ms | 110.3ms | ~9 |
| 1080p | ≈37.5ms | ≈23.9ms | ≈6.8ms | ≈47.6ms | 115.8ms | ~9 |

复现：`cd tools && g++ perf_bench.cpp ../src/FeatureExtractor.cpp ../src/EventDetector.cpp <moc> -o perf_bench.exe ... -lopencv_world455 && ./perf_bench.exe`

---

## 📌 已知限制 / 待改进

- 单视频流模式（`streamId=0`），多流并发为预留设计
- 跨帧跟踪为 IoU 贪心（无卡尔曼预测 / 外观 ReID）；两人交叉时可能 ID 互换——SORT 类算法通病，可进阶：颜色直方图 ReID 兜底 + 卡尔曼预测
- 无正式单元测试框架（当前为 ad-hoc 验证脚本）
