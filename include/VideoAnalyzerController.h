#ifndef VIDEOANALYZERCONTROLLER_H
#define VIDEOANALYZERCONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <opencv2/core.hpp>

#include "EventDetector.h"

class StreamManager;
class DatabaseManager;
class LLMClient;

// 门面/协调者：串联采集、检测、存储、摘要各模块
class VideoAnalyzerController : public QObject
{
    Q_OBJECT

public:
    explicit VideoAnalyzerController(QObject *parent = nullptr);
    ~VideoAnalyzerController() override;

    // 初始化各子系统
    bool init(const QString &dbPath, const QString &picsDir);

    // 视频源管理（摄像头 / 视频文件）
    bool startStream(int streamId, int cameraIndex);
    bool startStreamFromFile(int streamId, const QString &filePath);
    // 跳转 / 暂停 / 倍速 / 当前位置（播放器控制）
    bool seekPosition(int streamId, qint64 ms);
    bool setPaused(int streamId, bool paused);
    bool setPlaybackSpeed(int streamId, double speed);
    qint64 currentPositionMs(int streamId) const;
    bool stopStream(int streamId);
    int  streamCount() const;

    // 数据库
    QString dbPath() const;
    QString picsDir() const;
    // 数据库缓冲队列积压条数（性能监控用）
    int pendingEventCount() const;

    // LLM
    void setApiKey(const QString &key);
    void requestSummary(int streamId);

    // 检索（转发给 DatabaseManager，结果通过 searchResults 信号返回）
    void searchEvents(const QString &keyword);
    // 某路流最近事件（供进度条事件标记；streamId<0 表示全部）
    QList<QHash<QString, QVariant>> recentEventsByStream(int streamId, int limit = 200) const;

    // ── 事件检测参数调优（转发给 EventDetector）──────────────────
    void setAreaThreshold(double ratio);    // 运动面积阈值 0~1
    void setCooldownMs(int ms);             // 冷却时间 ms
    void setDetectInterval(int frames);     // 每 N 帧检测一次
    void setWarmupFrames(int frames);       // 背景建模预热帧数

signals:
    void frameReady(int streamId, const QImage &image);
    void eventDetected(int streamId, qint64 timestampMs, const QString &description,
                       const QString &keyframePath);
    void summaryReady(int streamId, const QString &summary, const QStringList &keywords);
    void searchResults(const QList<QHash<QString, QVariant>> &results);
    void errorOccurred(int streamId, const QString &message);
    // 视频元数据（文件总时长 ms，初始化进度条范围）
    void videoMetadataReady(int streamId, qint64 totalMs);
    // 新增：当视频流切换时，通知UI刷新事件列表
    void eventsRefreshed(int streamId); 

private slots:
    // StreamManager::frameCaptured → Mat→QImage → frameReady
    void onFrameCaptured(int streamId, const cv::Mat &frame);
    // StreamManager::captureError → errorOccurred
    void onCaptureError(int streamId, const QString &message);
    // EventDetector::eventTriggered → 保存关键帧 + 入库 + 对外转发
    void onEventTriggered(const EventDetector::EventRecord &record);
    // LLMClient::summaryReady → 回填数据库 + 对外转发
    void onSummaryReady(int streamId, const QString &summary,
                        const QStringList &keywords);

private:
    std::unique_ptr<StreamManager>    m_streamManager;
    std::unique_ptr<EventDetector>    m_eventDetector;
    std::unique_ptr<DatabaseManager> m_databaseManager;
    std::unique_ptr<LLMClient> m_llmClient;
    // ★ 多人共享关键帧缓存：timestampMs → keyframePath（LRU，最多 200 条）
    QHash<qint64, QString> m_keyframeCache;
    int m_activeStreamId = -1;
};

#endif // VIDEOANALYZERCONTROLLER_H
