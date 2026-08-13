#ifndef EVENTDETECTOR_H
#define EVENTDETECTOR_H

#include <QObject>
#include <QString>
#include <QHash>
#include <memory>
#include <vector>
#include <opencv2/opencv.hpp>

#include "FeatureExtractor.h"

// 事件检测：背景减除(MOG2) + 面积阈值 + 冷却时间，输出 EventRecord
class EventDetector : public QObject
{
    Q_OBJECT

public:
    // 单条事件记录（结构化日志，供 LLM 摘要消费）
    struct EventRecord
    {
        int      streamId = 0;
        qint64   timestampMs = 0;    // 事件发生时间（ms）
        qint64   videoPositionMs = 0;// ★ 视频内位置 ms（seek 跳转用，Controller 填充）
        double   motionRatio = 0.0;  // 运动像素占比 0~1
        cv::Rect boundingBox;        // 运动区域
        cv::Mat  keyFrame;           // 关键帧（深拷贝，已绘标注；多人时仅第一条携带）
        QString  description;        // 文本描述（供 LLM）
        bool     saveKeyFrame = true;   // ★ 是否保存截图/入库：仅检测到人体或人脸时为 true
        QString  keyframePath;       // ★ 已保存的关键帧路径（多人共享时填这个）
        int      personIndex = 0;    // ★ 人物编号 P1/P2/P3（0=未编号）
    };

    explicit EventDetector(QObject *parent = nullptr);
    ~EventDetector() override;

    // 参数配置
    void setAreaThreshold(double ratio);   // 运动面积阈值（默认 0.05）
    void setCooldownMs(int ms);            // 冷却时间（默认 5000）
    void setDetectInterval(int frames);    // 每 N 帧检测一次（默认 5）
    void setWarmupFrames(int frames);      // 背景建模预热帧数（默认 50）

    // 初始化特征提取（人脸检测 + 颜色分析）；模型缺失时优雅降级
    bool initFeatureExtractor(const QString &modelPath);

    // 单帧处理：返回 true 表示触发事件（结果通过 signal 发出）
    bool processFrame(int streamId, const cv::Mat &frame);
    // 在帧上绘制缓存的检测框（人体蓝色框 + 人脸绿色框）
    // 供播放画面实时显示标注；返回带框帧（深拷贝，BGR）
    cv::Mat annotateFrame(const cv::Mat &frame);
    // 重置背景模型（视频跳转后调用，避免跳转首帧误报）
    void reset();

signals:
    void eventTriggered(const EventDetector::EventRecord &record);

private:
    cv::Ptr<cv::BackgroundSubtractorMOG2> m_bgSubtractor;
    std::unique_ptr<FeatureExtractor> m_featureExtractor;   // 人脸/颜色分析
    double m_areaThreshold = 0.01;   // 默认 1%（配合 UI 滑块范围）
    int    m_cooldownMs = 5000;
    int    m_detectInterval = 2;   // DNN 检测抽帧间隔（帧）：2 = 框实时跟随，延迟 ~60ms
    int    m_warmupFrames = 50;
    int    m_frameCount = 0;
    // 每路视频的最近触发时间（ms）
    QHash<int, qint64> m_lastTriggerByStream;
    // ── 检测框缓存（供 annotateFrame 实时画框，避免每帧跑 DNN） ──
    std::vector<cv::Rect> m_lastPersonRects;   // 所有人体框
    std::vector<cv::Rect> m_lastFaceRects;     // 所有人脸框
    bool     m_lastHasPerson = false;
    bool     m_lastHasFace = false;
    int      m_lastDetectFrame = 0;   // 上次运行 DNN 的帧序号

    // ── ★ 跨帧跟踪（IoU 贪心匹配，简化 SORT） ──
    struct Track
    {
        int      trackId = 0;       // 稳定编号（P1/P2/...），跨帧沿用
        cv::Rect box;               // 最近一次检测框
        int      missedFrames = 0;  // 连续未匹配帧数（>5 回收）
    };
    std::vector<Track> m_activeTracks;   // 活跃轨迹
    int m_nextTrackId = 1;               // 自增 ID（reset 后归 1）

    // 新检测框与上帧轨迹做 IoU 贪心匹配；
    // 输出 outTrackIds[i] = 第 i 个检测框的稳定编号（1 起）
    void updateTracks(const std::vector<cv::Rect> &personRects,
                      std::vector<int> &outTrackIds);
    // 画框时按 IoU 就近取轨迹 ID；无匹配返回 0（调用方降级 "Person"）
    int trackIdForRect(const cv::Rect &rect) const;

    // 最近一次检测对应的跨帧稳定编号（与 m_lastPersonRects 下标对齐）
    std::vector<int> m_trackIds;
};

#endif // EVENTDETECTOR_H
