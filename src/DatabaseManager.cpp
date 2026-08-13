#include "DatabaseManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlRecord>
#include <QDebug>
#include <QDateTime>
#include <QSet>

// ──────────────────────────────────────────────────────────────────────
// 数据库管理实现：
//   - SQLite（Qt6::Sql 内置 QSQLITE 驱动）
//   - WAL 模式：读写并发不互斥（采集线程写 + UI 线程检索读）
//   - 预编译语句 + 事务批处理：高频事件写入
//   - 关键帧图片保存到 E:/pics
// ──────────────────────────────────────────────────────────────────────

DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent)
{
    // 定时批量落盘（2 秒一次，写路径不阻塞采集线程）
    m_flushTimer.setInterval(m_flushIntervalMs);
    m_flushTimer.setSingleShot(false);
    connect(&m_flushTimer, &QTimer::timeout,
            this, &DatabaseManager::onFlushTimeout);
}

DatabaseManager::~DatabaseManager()
{
    // 析构前强制落盘，防止缓冲队列数据丢失
    flushBuffer();
    close();
}

bool DatabaseManager::open(const QString &dbPath)
{
    m_dbPath = dbPath;

    // Qt6 中 QSQLITE 驱动需要唯一连接名
    const QString connName = QStringLiteral("videoanalyzer_conn_%1").arg(
        reinterpret_cast<quintptr>(this));
    m_db = QSqlDatabase::contains(connName)
               ? QSqlDatabase::database(connName)
               : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);

    if (!m_db.isValid()) {
        qWarning() << "SQLite driver not available:" << m_db.lastError().text();
        return false;
    }

    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qWarning() << "Failed to open database:" << m_db.lastError().text();
        return false;
    }

    // WAL 模式 + 同步级别（性能/安全平衡）
    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    return createTables();
}

void DatabaseManager::close()
{
    if (m_db.isOpen())
        m_db.close();
}

bool DatabaseManager::isOpen() const
{
    return m_db.isOpen();
}

bool DatabaseManager::createTables()
{
    QSqlQuery query(m_db);
    const bool ok = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  video_id INTEGER NOT NULL,"
        "  timestamp_ms INTEGER NOT NULL,"
        "  description TEXT NOT NULL,"
        "  keyframe_path TEXT,"
        "  generated_summary TEXT,"
        "  keywords TEXT,"
        "  video_position_ms INTEGER DEFAULT 0,"
        "  created_at TEXT DEFAULT (datetime('now','localtime'))"
        ")"));

    if (!ok)
        qWarning() << "createTables failed:" << query.lastError().text();

    // ★ 老库升级：ALTER TABLE 加 video_position_ms 列
    //   成功 = 老库升级；失败(duplicate column) = 字段已存在，忽略
    QSqlQuery alter(m_db);
    if (!alter.exec(QStringLiteral(
            "ALTER TABLE events ADD COLUMN video_position_ms INTEGER DEFAULT 0"))) {
        const QString errText = alter.lastError().text();
        if (!errText.contains(QStringLiteral("duplicate column")))
            qWarning() << "ALTER TABLE video_position_ms failed:" << errText;
        // 字段已存在则静默忽略
    }

    // ★ B1: summaries 表（独立存 AI 摘要，与 events 解耦）
    //   每次生成摘要追加一行，不覆盖历史；event_ids 关联本次摘要涉及的事件
    QSqlQuery sQuery(m_db);
    const bool sOk = sQuery.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS summaries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  stream_id INTEGER NOT NULL,"
        "  summary_text TEXT NOT NULL,"
        "  keywords TEXT,"
        "  event_ids TEXT,"
        "  generated_at TEXT DEFAULT (datetime('now','localtime'))"
        ")"));
    if (!sOk)
        qWarning() << "createTables summaries failed:" << sQuery.lastError().text();

    // 关键词索引（搜索提速）
    QSqlQuery idx(m_db);
    idx.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_summaries_keywords ON summaries(keywords)"));

    return ok;
}

QString DatabaseManager::saveKeyFrameImage(int streamId, qint64 timestampMs,
                                           const cv::Mat &frame)
{
    if (frame.empty())
        return QString();

    // 确保目录存在
    QDir dir;
    if (!dir.exists(m_picsDir))
        dir.mkpath(m_picsDir);

    // 文件名：stream_<id>_<epochMs>.jpg
    const QString fileName = QStringLiteral("stream%1_%2.jpg")
                                 .arg(streamId)
                                 .arg(timestampMs);
    const QString fullPath = QDir(m_picsDir).filePath(fileName);

    // ★ 颜色规范：imwrite 直接保存 BGR 帧，OpenCV 会自动生成颜色正常的 JPG。
    //   严禁在此处转 RGB（错误转换会导致保存图片颜色反转——历史 bug）。
    const bool ok = cv::imwrite(fullPath.toStdString(), frame);
    return ok ? fullPath : QString();
}

bool DatabaseManager::insertEvent(int streamId, qint64 timestampMs,
                                  const QString &description, const QString &keyframePath,
                                  qint64 videoPositionMs)
{
    // 内存缓冲：只入队，立即返回，不阻塞调用线程（采集/检测线程）
    EventRecord rec;
    rec.streamId = streamId;
    rec.timestampMs = timestampMs;
    rec.description = description;
    rec.keyframePath = keyframePath;
    rec.videoPositionMs = videoPositionMs;   // ★ 视频内位置 ms（seek 用）
    m_pendingEvents.append(rec);

    // 首次入队时启动定时器（避免空转）
    if (!m_flushTimer.isActive())
        m_flushTimer.start();
    return true;
}

bool DatabaseManager::flushBuffer()
{
    if (m_pendingEvents.isEmpty())
        return true;
    if (!m_db.isOpen())
        return false;

    // 开启事务：批量提交，摊薄磁盘 IO 开销
    if (!m_db.transaction()) {
        qWarning() << "flushBuffer: transaction begin failed:"
                   << m_db.lastError().text();
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO events (video_id, timestamp_ms, description, keyframe_path, video_position_ms)"
        " VALUES (?, ?, ?, ?, ?)"));

    bool allOk = true;
    for (const EventRecord &rec : m_pendingEvents) {
        query.addBindValue(rec.streamId);
        query.addBindValue(rec.timestampMs);
        query.addBindValue(rec.description);
        query.addBindValue(rec.keyframePath);
        query.addBindValue(rec.videoPositionMs);
        if (!query.exec()) {
            qWarning() << "flushBuffer: insert failed:"
                       << query.lastError().text();
            allOk = false;
            break;
        }
    }

    if (allOk) {
        if (!m_db.commit()) {
            qWarning() << "flushBuffer: commit failed:"
                       << m_db.lastError().text();
            allOk = false;
        }
    } else {
        m_db.rollback();
    }

    // 无论成功失败都清空队列（失败条目的日志已输出，避免无限重试）
    m_pendingEvents.clear();
    return allOk;
}

int DatabaseManager::pendingCount() const
{
    return m_pendingEvents.size();
}

void DatabaseManager::onFlushTimeout()
{
    flushBuffer();
    // 队列清空后停止定时器
    if (m_pendingEvents.isEmpty())
        m_flushTimer.stop();
}

bool DatabaseManager::updateSummary(qint64 eventId, const QString &summary,
                                    const QString &keywords)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE events SET generated_summary = ?, keywords = ? WHERE id = ?"));
    query.addBindValue(summary);
    query.addBindValue(keywords);
    query.addBindValue(eventId);

    if (!query.exec()) {
        qWarning() << "updateSummary failed:" << query.lastError().text();
        return false;
    }
    return true;
}

bool DatabaseManager::insertSummary(int streamId, const QString &summary,
                                    const QString &keywords, const QString &eventIds)
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO summaries (stream_id, summary_text, keywords, event_ids)"
        " VALUES (?, ?, ?, ?)"));
    query.addBindValue(streamId);
    query.addBindValue(summary);
    query.addBindValue(keywords);
    query.addBindValue(eventIds);

    if (!query.exec()) {
        qWarning() << "insertSummary failed:" << query.lastError().text();
        return false;
    }
    return true;
}

QList<QHash<QString, QVariant>> DatabaseManager::searchSummariesByKeyword(const QString &keyword) const
{
    QList<QHash<QString, QVariant>> results;
    if (!m_db.isOpen() || keyword.trimmed().isEmpty())
        return results;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, stream_id, summary_text, keywords, event_ids, generated_at"
        " FROM summaries"
        " WHERE keywords LIKE ? OR summary_text LIKE ?"
        " ORDER BY id DESC"));
    const QString pattern = QStringLiteral("%%1%").arg(keyword.trimmed());
    query.addBindValue(pattern);
    query.addBindValue(pattern);

    if (!query.exec()) {
        qWarning() << "searchSummariesByKeyword failed:" << query.lastError().text();
        return results;
    }

    while (query.next()) {
        QHash<QString, QVariant> row;
        row[QStringLiteral("id")] = query.value(0);
        row[QStringLiteral("stream_id")] = query.value(1);
        row[QStringLiteral("summary_text")] = query.value(2);
        row[QStringLiteral("keywords")] = query.value(3);
        row[QStringLiteral("event_ids")] = query.value(4);
        row[QStringLiteral("generated_at")] = query.value(5);
        results.append(row);
    }
    return results;
}

QList<QHash<QString, QVariant>> DatabaseManager::recentSummaries(int streamId, int limit) const
{
    QList<QHash<QString, QVariant>> results;
    if (!m_db.isOpen())
        return results;

    QSqlQuery query(m_db);
    if (streamId < 0) {
        query.prepare(QStringLiteral(
            "SELECT id, stream_id, summary_text, keywords, event_ids, generated_at"
            " FROM summaries ORDER BY id DESC LIMIT ?"));
        query.addBindValue(limit);
    } else {
        query.prepare(QStringLiteral(
            "SELECT id, stream_id, summary_text, keywords, event_ids, generated_at"
            " FROM summaries WHERE stream_id = ? ORDER BY id DESC LIMIT ?"));
        query.addBindValue(streamId);
        query.addBindValue(limit);
    }

    if (!query.exec()) {
        qWarning() << "recentSummaries failed:" << query.lastError().text();
        return results;
    }

    while (query.next()) {
        QHash<QString, QVariant> row;
        row[QStringLiteral("id")] = query.value(0);
        row[QStringLiteral("stream_id")] = query.value(1);
        row[QStringLiteral("summary_text")] = query.value(2);
        row[QStringLiteral("keywords")] = query.value(3);
        row[QStringLiteral("event_ids")] = query.value(4);
        row[QStringLiteral("generated_at")] = query.value(5);
        results.append(row);
    }
    return results;
}

QList<QHash<QString, QVariant>> DatabaseManager::searchEvents(const QString &keyword) const
{
    QList<QHash<QString, QVariant>> results;
    if (!m_db.isOpen() || keyword.trimmed().isEmpty())
        return results;

    // ★ B4: 双路搜索
    //   路径 A：events 表 description / generated_summary / keywords（历史事件）
    //   路径 B：summaries 表 keywords / summary_text → 反查 event_ids 指向的事件
    //   合并去重（按 id）
    const QString pattern = QStringLiteral("%%1%").arg(keyword.trimmed());
    QSet<qint64> seenIds;

    // 路径 A：events 表
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, video_id, timestamp_ms, description, keyframe_path,"
        "       generated_summary, keywords, video_position_ms"
        " FROM events"
        " WHERE description LIKE ? OR generated_summary LIKE ? OR keywords LIKE ?"
        " ORDER BY timestamp_ms DESC"));
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    query.addBindValue(pattern);

    if (!query.exec()) {
        qWarning() << "searchEvents failed:" << query.lastError().text();
        return results;
    }

    while (query.next()) {
        QHash<QString, QVariant> row;
        row[QStringLiteral("id")] = query.value(0);
        row[QStringLiteral("video_id")] = query.value(1);
        row[QStringLiteral("timestamp_ms")] = query.value(2);
        row[QStringLiteral("description")] = query.value(3);
        row[QStringLiteral("keyframe_path")] = query.value(4);
        row[QStringLiteral("generated_summary")] = query.value(5);
        row[QStringLiteral("keywords")] = query.value(6);
        row[QStringLiteral("video_position_ms")] = query.value(7);
        row[QStringLiteral("_source")] = QStringLiteral("event");
        seenIds.insert(query.value(0).toLongLong());
        results.append(row);
    }

    // 路径 B：summaries 表 → event_ids 反查
    const auto summaries = searchSummariesByKeyword(keyword);
    for (const auto &srow : summaries) {
        const QString eventIds = srow.value(QStringLiteral("event_ids")).toString();
        if (eventIds.trimmed().isEmpty())
            continue;
        // event_ids 逗号分隔 → 逐 id 查 events 补充
        const QStringList ids = eventIds.split(QChar(','), Qt::SkipEmptyParts);
        for (const QString &idStr : ids) {
            bool okId = false;
            const qint64 id = idStr.trimmed().toLongLong(&okId);
            if (!okId || seenIds.contains(id))
                continue;
            QSqlQuery eq(m_db);
            eq.prepare(QStringLiteral(
                "SELECT id, video_id, timestamp_ms, description, keyframe_path,"
                "       generated_summary, keywords, video_position_ms"
                " FROM events WHERE id = ?"));
            eq.addBindValue(id);
            if (!eq.exec() || !eq.next())
                continue;
            QHash<QString, QVariant> row;
            row[QStringLiteral("id")] = eq.value(0);
            row[QStringLiteral("video_id")] = eq.value(1);
            row[QStringLiteral("timestamp_ms")] = eq.value(2);
            row[QStringLiteral("description")] = eq.value(3);
            row[QStringLiteral("keyframe_path")] = eq.value(4);
            row[QStringLiteral("generated_summary")] = eq.value(5);
            row[QStringLiteral("keywords")] = eq.value(6);
            row[QStringLiteral("video_position_ms")] = eq.value(7);
            row[QStringLiteral("_source")] = QStringLiteral("summary");
            seenIds.insert(id);
            results.append(row);
        }
    }

    return results;
}

void DatabaseManager::setPicsDir(const QString &dir)
{
    m_picsDir = dir;
}

QList<QHash<QString, QVariant>> DatabaseManager::recentEvents(int streamId, int limit) const
{
    QList<QHash<QString, QVariant>> results;
    if (!m_db.isOpen())
        return results;

    QSqlQuery query(m_db);
    if (streamId < 0) {
        query.prepare(QStringLiteral(
            "SELECT id, video_id, timestamp_ms, description, keyframe_path,"
            "       generated_summary, keywords, video_position_ms"
            " FROM events ORDER BY timestamp_ms DESC LIMIT ?"));
        query.addBindValue(limit);
    } else {
        query.prepare(QStringLiteral(
            "SELECT id, video_id, timestamp_ms, description, keyframe_path,"
            "       generated_summary, keywords, video_position_ms"
            " FROM events WHERE video_id = ? ORDER BY timestamp_ms DESC LIMIT ?"));
        query.addBindValue(streamId);
        query.addBindValue(limit);
    }

    if (!query.exec()) {
        qWarning() << "recentEvents failed:" << query.lastError().text();
        return results;
    }

    while (query.next()) {
        QHash<QString, QVariant> row;
        row[QStringLiteral("id")] = query.value(0);
        row[QStringLiteral("video_id")] = query.value(1);
        row[QStringLiteral("timestamp_ms")] = query.value(2);
        row[QStringLiteral("description")] = query.value(3);
        row[QStringLiteral("keyframe_path")] = query.value(4);
        row[QStringLiteral("generated_summary")] = query.value(5);
        row[QStringLiteral("keywords")] = query.value(6);
        row[QStringLiteral("video_position_ms")] = query.value(7);
        results.append(row);
    }
    return results;
}

QString DatabaseManager::picsDir() const
{
    return m_picsDir;
}
