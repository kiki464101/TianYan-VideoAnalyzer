#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QList>
#include <QTimer>

#include <opencv2/opencv.hpp>

// 数据库管理：SQLite（Qt6::Sql 内置驱动）+ 关键帧图片落盘（E:/pics）
// 写入策略：内存缓冲队列 + 定时批量 flush（减少磁盘 IO 阻塞采集线程）
class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    // 待入库事件（内存缓冲队列元素）
    struct EventRecord
    {
        int       streamId = 0;
        qint64    timestampMs = 0;
        QString   description;
        QString   keyframePath;
        qint64    videoPositionMs = 0;   // ★ 视频内位置 ms（seek 跳转用）
    };

    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager() override;

    // 打开数据库并建表（Events: id, video_id, timestamp, description,
    //                       generated_summary, keywords, keyframe_path）
    bool open(const QString &dbPath);
    void close();
    bool isOpen() const;

    // 关键帧图片保存（返回落盘路径）
    QString saveKeyFrameImage(int streamId, qint64 timestampMs, const cv::Mat &frame);
    // 事件入缓冲队列（高频调用，立即返回，不阻塞调用线程）
    bool insertEvent(int streamId, qint64 timestampMs,
                     const QString &description, const QString &keyframePath,
                     qint64 videoPositionMs = 0);
    // 立即将缓冲队列批量写入数据库（事务）
    bool flushBuffer();
    // 缓冲队列当前积压条数
    int  pendingCount() const;

    // 摘要回填
    bool updateSummary(qint64 eventId, const QString &summary, const QString &keywords);

    // ★ B2: 摘要独立表（每次生成追加一行，不覆盖历史）
    bool insertSummary(int streamId, const QString &summary,
                       const QString &keywords, const QString &eventIds);
    // 按关键词搜索摘要（返回 summaries 行，含 event_ids）
    QList<QHash<QString, QVariant>> searchSummariesByKeyword(const QString &keyword) const;
    // 最近 N 条摘要
    QList<QHash<QString, QVariant>> recentSummaries(int streamId, int limit = 20) const;
    // 关键词检索
    QList<QHash<QString, QVariant>> searchEvents(const QString &keyword) const;
    // 最近事件（供摘要模块汇总；streamId<0 表示全部）
    QList<QHash<QString, QVariant>> recentEvents(int streamId, int limit = 50) const;

    // 图片保存目录（默认 E:/pics）
    void setPicsDir(const QString &dir);
    QString picsDir() const;

private slots:
    // 定时批量落盘（由 m_flushTimer 周期触发）
    void onFlushTimeout();

private:
    bool createTables();

    QSqlDatabase m_db;
    QString      m_dbPath;
    QString      m_picsDir = "E:/pics";

    // 内存缓冲队列（写路径：insertEvent 只入队，不碰磁盘）
    QList<EventRecord> m_pendingEvents;
    // 定时 flush（默认 2 秒）
    QTimer m_flushTimer;
    int    m_flushIntervalMs = 2000;
};

#endif // DATABASEMANAGER_H
