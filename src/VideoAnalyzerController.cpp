#include "VideoAnalyzerController.h"
#include "StreamManager.h"
#include "EventDetector.h"
#include "DatabaseManager.h"
#include "LLMClient.h"

#include <QImage>
#include <QVector>
#include <QStringList>

// ──────────────────────────────────────────────────────────────────────
// cv::Mat → QImage 转换（跨线程安全：深拷贝）
// ──────────────────────────────────────────────────────────────────────
namespace {

QImage matToQImage(const cv::Mat &mat)
{
    if (mat.empty())
        return QImage();

    switch (mat.type()) {
    case CV_8UC1: {
        QVector<QRgb> colorTable(256);
        for (int i = 0; i < 256; ++i)
            colorTable[i] = qRgb(i, i, i);
        QImage img(mat.data, mat.cols, mat.rows,
                   static_cast<int>(mat.step), QImage::Format_Indexed8);
        img.setColorTable(colorTable);
        return img.copy();   // 深拷贝：脱离 Mat 内存
    }
    case CV_8UC3: {
        QImage img(mat.data, mat.cols, mat.rows,
                   static_cast<int>(mat.step), QImage::Format_RGB888);
        return img.rgbSwapped().copy();   // BGR→RGB + 深拷贝
    }
    default:
        return QImage();
    }
}

} // namespace

// ──────────────────────────────────────────────────────────────────────

VideoAnalyzerController::VideoAnalyzerController(QObject *parent)
    : QObject(parent)
{
    // 允许 EventDetector::EventRecord 通过 QueuedConnection 跨线程传递
    qRegisterMetaType<EventDetector::EventRecord>("EventDetector::EventRecord");
}

VideoAnalyzerController::~VideoAnalyzerController()
{
    // 先停采集，再释放子系统
    if (m_streamManager)
        m_streamManager->stopCapture(m_activeStreamId);
}

bool VideoAnalyzerController::init(const QString &dbPath, const QString &picsDir)
{
    m_streamManager = std::make_unique<StreamManager>(this);
    m_eventDetector = std::make_unique<EventDetector>(this);
    m_databaseManager = std::make_unique<DatabaseManager>(this);
    m_llmClient = std::make_unique<LLMClient>(this);

    m_databaseManager->setPicsDir(picsDir);
    m_databaseManager->open(dbPath);

    // 启动时加载人脸检测模型（预设路径 E:/VideoProject/models/）
    // 失败仅输出日志，不阻断程序启动（优雅降级）
    if (m_eventDetector)
        m_eventDetector->initFeatureExtractor(QStringLiteral("E:/VideoProject/models/"));

    // 采集线程 → 帧路由
    connect(m_streamManager.get(), &StreamManager::frameCaptured,
            this, &VideoAnalyzerController::onFrameCaptured);
    connect(m_streamManager.get(), &StreamManager::captureError,
            this, &VideoAnalyzerController::onCaptureError);
    // 视频元数据 → 对外转发（初始化进度条范围）
    connect(m_streamManager.get(), &StreamManager::videoMetadataReady,
            this, &VideoAnalyzerController::videoMetadataReady);
    // 跳转后重置背景模型（避免跳转首帧误报）
    connect(m_streamManager.get(), &StreamManager::backgroundResetRequested,
            this, [this](int streamId) {
                Q_UNUSED(streamId);
                if (m_eventDetector)
                    m_eventDetector->reset();
            });

    // 事件检测 → 落库 + 对外转发
    connect(m_eventDetector.get(), &EventDetector::eventTriggered,
            this, &VideoAnalyzerController::onEventTriggered);

    // LLM 摘要 → 回填 + 对外转发
    connect(m_llmClient.get(), &LLMClient::summaryReady,
            this, &VideoAnalyzerController::onSummaryReady);
    connect(m_llmClient.get(), &LLMClient::requestFailed,
            this, [this](int streamId, const QString &err) {
                emit errorOccurred(streamId, err);
            });

    return true;
}

bool VideoAnalyzerController::startStream(int streamId, int cameraIndex)
{
    //如果有正在运行的流，先停止它
    if(m_activeStreamId >= 0 && m_streamManager){
        m_streamManager->stopCapture(m_activeStreamId);
    }

    //再启动新流
    if (!m_streamManager)
        return false;

    m_activeStreamId = streamId;

    bool ok = m_streamManager->startCapture(streamId, cameraIndex);
    if (!ok)
        {
            emit errorOccurred(streamId, QStringLiteral("无法启动采集流 %1").arg(streamId));
        }

    //刷新事件，只显示新流
    emit eventsRefreshed(m_activeStreamId);
    
    return ok;
}

bool VideoAnalyzerController::startStreamFromFile(int streamId, const QString &filePath)
{
    //如果有正在运行的流，先停止它
    if(m_activeStreamId >= 0 && m_streamManager){
        m_streamManager->stopCapture(m_activeStreamId);
    }

    //再启动新流
    if (!m_streamManager)
        return false;

    m_activeStreamId = streamId;

    bool ok = m_streamManager->startCaptureFromFile(streamId, filePath);
    if (!ok)
        emit errorOccurred(streamId,
                           QStringLiteral("无法打开视频文件: %1").arg(filePath));

    // ★ 新视频启动后通知 UI 刷新（清空旧视频事件，与摄像头模式保持一致）
    if (ok)
        emit eventsRefreshed(m_activeStreamId);

    return ok;
}

bool VideoAnalyzerController::stopStream(int streamId)
{
    if (!m_streamManager)
        return false;

    bool ok = m_streamManager->stopCapture(streamId);
    if (ok && m_activeStreamId == streamId)
        m_activeStreamId = -1;
    return ok;
}

int VideoAnalyzerController::pendingEventCount() const
{
    return m_databaseManager ? m_databaseManager->pendingCount() : 0;
}

bool VideoAnalyzerController::seekPosition(int streamId, qint64 ms)
{
    if (!m_streamManager)
        return false;
    return m_streamManager->seekPosition(streamId, ms);
}

bool VideoAnalyzerController::setPaused(int streamId, bool paused)
{
    if (!m_streamManager)
        return false;
    return m_streamManager->setPaused(streamId, paused);
}

bool VideoAnalyzerController::setPlaybackSpeed(int streamId, double speed)
{
    if (!m_streamManager)
        return false;
    return m_streamManager->setPlaybackSpeed(streamId, speed);
}

qint64 VideoAnalyzerController::currentPositionMs(int streamId) const
{
    if (!m_streamManager)
        return -1;
    return m_streamManager->currentPositionMs(streamId);
}

int VideoAnalyzerController::streamCount() const
{
    return m_streamManager ? m_streamManager->activeCount() : 0;
}

QString VideoAnalyzerController::dbPath() const
{
    return m_databaseManager ? m_databaseManager->picsDir() : QString();
}

QString VideoAnalyzerController::picsDir() const
{
    return m_databaseManager ? m_databaseManager->picsDir() : QString();
}

void VideoAnalyzerController::setApiKey(const QString &key)
{
    if (m_llmClient)
        m_llmClient->setApiKey(key);
}

void VideoAnalyzerController::requestSummary(int streamId)
{
    if (!m_databaseManager || !m_llmClient)
        return;
    // 汇总当前流最近 20 条事件描述，拼成结构化文本交给 LLM
    const auto rows = m_databaseManager->recentEvents(streamId, 20);
    QStringList lines;
    for (const auto &row : rows) {
        lines << row.value(QStringLiteral("description")).toString();
    }

    if (lines.isEmpty()) {
        emit errorOccurred(streamId, QStringLiteral("该视频流暂无事件，无法生成摘要"));
        return;
    }

    m_llmClient->requestSummary(streamId, lines.join(QStringLiteral("\n")));
}

// ────────────────────────────── 私有槽 ──────────────────────────────

void VideoAnalyzerController::onFrameCaptured(int streamId, const cv::Mat &frame)
{
    // ── 防御性检查：防止空指针 / 无效数据导致崩溃 ────────────────
    // 1) Mat 本身为空（未读到有效帧）
    if (frame.empty())
        return;

    // 2) 尺寸合法性（避免 0 尺寸 QImage 进入 UI 缩放路径）
    if (frame.cols <= 0 || frame.rows <= 0)
        return;

    // ── 送入事件检测（内部自行抽帧/冷却） ────────────────────────
    cv::Mat displayFrame = frame.clone();   // 显示帧（默认原始帧）
    if (m_eventDetector) {
        m_eventDetector->processFrame(streamId, frame);
        // ★ 用带检测框的帧显示：人体蓝色框 + 人脸绿色框
        //   annotateFrame 使用缓存框，每帧低成本绘制（DNN 按抽帧间隔运行）
        displayFrame = m_eventDetector->annotateFrame(frame);
    }

    // 3) 异常类型 / 数据损坏（matToQImage 内部会返回空 QImage）
    QImage img = matToQImage(displayFrame);
    if (img.isNull())
        return;

    // 通过全部检查后才发送帧
    emit frameReady(streamId, img);
}

void VideoAnalyzerController::onCaptureError(int streamId, const QString &message)
{
    emit errorOccurred(streamId, message);
}

void VideoAnalyzerController::onEventTriggered(const EventDetector::EventRecord &record)
{
    // ★ 填视频内位置：EventDetector 只看 Mat 不知道播放进度，
    //   由 Controller 在转发时读取（seek 跳转用）
    EventDetector::EventRecord rec = record;
    if (m_streamManager) {
        const qint64 pos = m_streamManager->currentPositionMs(rec.streamId);
        rec.videoPositionMs = (pos >= 0) ? pos : 0;   // 未打开流 → 0（老数据语义）
    }

    // ★ 只有检测到人体/人脸（saveKeyFrame=true）才截图+入库；
    //   无人（物体移动）仅转发给 UI 显示信息流，不落盘
    //   多人事件共享同一 timestampMs → 同一张关键帧，只写盘一次
    QString keyframePath;
    if (rec.saveKeyFrame && m_databaseManager) {
        const qint64 ts = rec.timestampMs;
        if (rec.keyframePath.isEmpty() && m_keyframeCache.contains(ts)) {
            // 同帧第 2..N 条：复用已保存的共享关键帧
            keyframePath = m_keyframeCache.value(ts);
        } else if (!rec.keyframePath.isEmpty()) {
            // 显式指定路径（预留）
            keyframePath = rec.keyframePath;
        } else {
            // 第一条：保存关键帧 + 入库 + 记缓存
            keyframePath = m_databaseManager->saveKeyFrameImage(
                rec.streamId, rec.timestampMs, rec.keyFrame);
            m_keyframeCache.insert(ts, keyframePath);
            // LRU 防膨胀：保留最近 200 条
            while (m_keyframeCache.size() > 200)
                m_keyframeCache.erase(m_keyframeCache.begin());
        }

        m_databaseManager->insertEvent(rec.streamId, rec.timestampMs,
                                       rec.description, keyframePath,
                                       rec.videoPositionMs);   // ★ 新参数
    }

    // 3) 对外转发（UI 事件列表；无人事件 keyframePath 为空，双击无图）
    emit eventDetected(rec.streamId, rec.timestampMs,
                       rec.description, keyframePath);
}

void VideoAnalyzerController::onSummaryReady(int streamId, const QString &summary,
                                             const QStringList &keywords)
{
    // ★ B3: 摘要写入独立 summaries 表（每次追加一行，不覆盖历史）
    //   兼容：同时回填最近事件记录的 generated_summary（旧搜索路径仍可用）
    if (m_databaseManager) {
        // 收集本次摘要涉及的事件 id（最近 50 条，供 summaries.event_ids 反查）
        const auto rows = m_databaseManager->recentEvents(streamId, 50);
        QStringList eventIds;
        for (const auto &row : rows)
            eventIds << row.value(QStringLiteral("id")).toString();

        m_databaseManager->insertSummary(
            streamId, summary, keywords.join(QStringLiteral(",")),
            eventIds.join(QChar(',')));

        // 兼容旧路径：回填最近一条事件的 generated_summary
        if (!rows.isEmpty()) {
            m_databaseManager->updateSummary(
                rows.first().value(QStringLiteral("id")).toLongLong(),
                summary, keywords.join(QStringLiteral(",")));
        }
    }

    emit summaryReady(streamId, summary, keywords);
}

void VideoAnalyzerController::searchEvents(const QString &keyword)
{
    if (!m_databaseManager) {
        emit searchResults(QList<QHash<QString, QVariant>>());
        return;
    }
    emit searchResults(m_databaseManager->searchEvents(keyword));
}

QList<QHash<QString, QVariant>>
VideoAnalyzerController::recentEventsByStream(int streamId, int limit) const
{
    if (!m_databaseManager)
        return {};
    return m_databaseManager->recentEvents(streamId, limit);
}

// ────────────── 事件检测参数调优（转发给 EventDetector）──────────────

void VideoAnalyzerController::setAreaThreshold(double ratio)
{
    if (m_eventDetector)
        m_eventDetector->setAreaThreshold(ratio);
}

void VideoAnalyzerController::setCooldownMs(int ms)
{
    if (m_eventDetector)
        m_eventDetector->setCooldownMs(ms);
}

void VideoAnalyzerController::setDetectInterval(int frames)
{
    if (m_eventDetector)
        m_eventDetector->setDetectInterval(frames);
}

void VideoAnalyzerController::setWarmupFrames(int frames)
{
    if (m_eventDetector)
        m_eventDetector->setWarmupFrames(frames);
}
