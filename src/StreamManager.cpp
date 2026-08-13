#include "StreamManager.h"

#include <QThread>
#include <QMetaObject>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QProcess>

#include <atomic>
#include <cmath>
#include <algorithm>

// ──────────────────────────────────────────────────────────────────────
// 采集工作线程：在独立 QThread 中循环读取摄像头/视频帧
// 通过信号将 cv::Mat 送回主线程（需 qRegisterMetaType<cv::Mat>）
// 支持：视频文件跳转（invokeMethod → 子线程 cap.set）、暂停/恢复、
//       播放位置原子读取、视频元数据上报
// ──────────────────────────────────────────────────────────────────────
class CaptureWorker : public QObject
{
    Q_OBJECT

public:
    // 摄像头模式
    explicit CaptureWorker(int cameraIndex)
        : m_cameraIndex(cameraIndex)
        , m_filePath(QString())
    {
    }

    // 文件模式
    explicit CaptureWorker(const QString &filePath)
        : m_cameraIndex(-1)
        , m_filePath(filePath)
    {
    }

    // 线程启动后由 QThread::started 触发，在子线程执行
public slots:
    void process()
    {
        // 打开视频源：摄像头模式 or 文件模式
        if (!m_filePath.isEmpty())
            m_cap.open(m_filePath.toStdString());          // 文件模式
        else
            m_cap.open(m_cameraIndex);                     // 摄像头模式

        // ★ 文件模式打开失败：尝试用系统 ffmpeg 转码（HEVC/H.264 mp4 等
        //   OpenCV MinGW 构建无 FFMPEG 后端，无法直接解码）
        if (!m_cap.isOpened() && !m_filePath.isEmpty()) {
            const QString convPath = convertWithFfmpeg(m_filePath);
            if (!convPath.isEmpty()) {
                m_cap.open(convPath.toStdString());
                m_isConvertedFile = true;
                m_convertedPath = convPath;
            }
        }

        if (!m_cap.isOpened()) {
            emit errorOccurred(m_filePath.isEmpty()
                                   ? QStringLiteral("无法打开摄像头 #%1").arg(m_cameraIndex)
                                   : QStringLiteral("无法打开视频文件: %1").arg(m_filePath));
            emit finished();      // ★ 打开失败也必须通知退出，否则线程永不结束
            return;
        }

        // 文件模式：计算总时长并上报（初始化进度条范围）
        if (!m_filePath.isEmpty()) {
            const double fps = m_cap.get(cv::CAP_PROP_FPS);
            const double frames = m_cap.get(cv::CAP_PROP_FRAME_COUNT);
            m_totalMs = (fps > 0.0 && frames > 0.0)
                            ? static_cast<qint64>(frames / fps * 1000.0)
                            : 0;
            emit videoMetadataReady(m_totalMs);
        }

        cv::Mat frame;
        while (!m_stop.load(std::memory_order_acquire)) {
            // ★ 处理投递到子线程的事件（seek 请求等），保证跳转及时响应
            QCoreApplication::processEvents();

            // 暂停：不读帧，等待恢复
            if (m_paused.load(std::memory_order_acquire)) {
                QThread::msleep(50);
                continue;
            }

            if (m_cap.read(frame)) {
                // 原子记录当前播放位置（供进度条轮询）
                // 防御：OpenCV 在 seek/暂停边界可能返回 NaN 或 garbage 极大值
                const double posMs = m_cap.get(cv::CAP_PROP_POS_MSEC);
                constexpr double kMaxPlausibleMs = 24.0 * 3600.0 * 1000.0;  // 24h
                if (posMs >= 0 && posMs < kMaxPlausibleMs)
                    m_positionMs.store(static_cast<qint64>(posMs),
                                       std::memory_order_release);
                // clone 深拷贝：帧缓冲复用，必须复制后再跨线程传递
                emit frameReady(frame.clone());
            } else {
                // ★ 文件模式：播放结束（read 返回 false）→ 正常退出，避免死循环
                if (!m_filePath.isEmpty()) {
                    cleanupConvertedFile();
                    emit finished();
                    return;
                }
                // 摄像头模式：读取失败短暂退避后重试
                QThread::msleep(10);
                continue;
            }
            // 帧率控制：基准 30fps(33ms)，按倍速调整延时
            //   speed=2.0 → ~17ms(60fps 快进)；speed=0.5 → ~66ms(15fps 慢放)
            {
                const double speed = m_speed.load(std::memory_order_acquire);
                const double baseMs = 33.0;
                const double delayMs = speed > 0.0 ? baseMs / speed : baseMs;
                QThread::msleep(static_cast<int>(delayMs));
            }
        }

        cleanupConvertedFile();
        emit finished();          // ★ 通知线程退出清理链
    }

    // ── 跨线程请求（原子操作，线程安全） ──────────────────────────

    // 停止
    void stop()
    {
        m_stop.store(true, std::memory_order_release);
    }

    // 暂停/恢复
    void setPaused(bool paused)
    {
        m_paused.store(paused, std::memory_order_release);
    }

    // 设置播放速度（子线程读取，原子安全；暂停状态由 process 忽略）
    void setPlaybackSpeed(double speed)
    {
        // 限制在 [0.1, 8.0] 合理范围，避免极端值
        const double clamped = std::clamp(speed, 0.1, 8.0);
        m_speed.store(clamped, std::memory_order_release);
    }

    // 跳转请求（主线程 → 子线程）：在子线程执行，操作 m_cap
    void handleSeek(qint64 ms)
    {
        if (!m_cap.isOpened())
            return;
        // ★ 优先按帧 seek（MJPG AVI 对 POS_MSEC 支持不稳，帧索引最可靠）
        const double fps = m_cap.get(cv::CAP_PROP_FPS);
        if (fps > 0.0) {
            const double frameIdx = static_cast<double>(ms) * fps / 1000.0;
            m_cap.set(cv::CAP_PROP_POS_FRAMES, frameIdx);
        } else {
            m_cap.set(cv::CAP_PROP_POS_MSEC, static_cast<double>(ms));
        }
        m_positionMs.store(ms, std::memory_order_release);
        // 跳转后重置背景模型，避免跳转首帧产生误报
        emit backgroundResetRequested();
    }

    // 当前播放位置（原子读取，双重防御非法值）
    qint64 positionMs() const
    {
        const qint64 v = m_positionMs.load(std::memory_order_acquire);
        constexpr qint64 kMaxPlausibleMs = 24LL * 3600 * 1000;  // 24h
        return (v >= 0 && v < kMaxPlausibleMs) ? v : 0;
    }

signals:
    void frameReady(const cv::Mat &frame);
    void errorOccurred(const QString &message);
    void finished();
    void videoMetadataReady(qint64 totalMs);
    void backgroundResetRequested();

private:
    // 用系统 ffmpeg 将无法解码的视频转码为 MJPG AVI（OpenCV 内置支持）
    // 返回转码后的临时文件路径；失败返回空串
    QString convertWithFfmpeg(const QString &srcPath);
    // 清理转码临时文件（播放结束/停止时调用）
    void cleanupConvertedFile();

    int       m_cameraIndex;
    QString   m_filePath;
    cv::VideoCapture m_cap;                    // 成员：供 handleSeek 访问
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    std::atomic<double> m_speed{1.0};          // 播放倍速（1.0x 正常）
    std::atomic<qint64> m_positionMs{0};       // 当前播放位置 ms
    qint64    m_totalMs = 0;                   // 文件总时长 ms
    bool      m_isConvertedFile = false;       // 是否经 ffmpeg 转码
    QString   m_convertedPath;                 // 转码临时文件（结束时清理）
};

// ★ moc 元信息必须放在 CaptureWorker 完整定义之后（moc 生成的代码
//   引用 CaptureWorker 类型，include 过早会导致 incomplete type 编译错误）
#include "StreamManager.moc"


// ──────────────────────────────────────────────────────────────────────
// CaptureWorker::convertWithFfmpeg
// 用系统 ffmpeg 将 OpenCV 无法解码的视频（HEVC/H.264 等）转码为
// MJPG AVI（OpenCV 内置支持的编码），转码产物为临时文件。
// ──────────────────────────────────────────────────────────────────────
QString CaptureWorker::convertWithFfmpeg(const QString &srcPath)
{
    // 定位系统 ffmpeg：WinGet 常见安装路径 + PATH 兜底
    QString ffmpeg = QStringLiteral(
        "C:/Users/HP/AppData/Local/Microsoft/WinGet/Packages/"
        "Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe/"
        "ffmpeg-8.1.2-full_build/bin/ffmpeg.exe");
    if (!QFileInfo::exists(ffmpeg))
        ffmpeg = QStringLiteral("ffmpeg");   // 依赖 PATH

    // 临时输出文件：同目录下 <原名>_conv_<pid>.avi
    const QFileInfo srcInfo(srcPath);
    const QString dstPath = QDir(srcInfo.absolutePath()).filePath(
        srcInfo.completeBaseName() + QStringLiteral("_conv_%1.avi").arg(
            QCoreApplication::applicationPid()));

    // ffmpeg -y -i src -c:v mjpeg -q:v 3 -an dst
    // 用 QProcess 启动（避免 system() 在 cmd.exe 下的路径/引号问题）
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ffmpeg, QStringList()
                   << QStringLiteral("-y")
                   << QStringLiteral("-i") << srcPath
                   << QStringLiteral("-c:v") << QStringLiteral("mjpeg")
                   << QStringLiteral("-q:v") << QStringLiteral("3")
                   << QStringLiteral("-an")
                   << dstPath);
    if (!proc.waitForFinished(120000)) {   // 2 分钟超时（大视频转码）
        proc.kill();
        qWarning() << "ffmpeg 转码超时, src=" << srcPath;
        QFile::remove(dstPath);
        return QString();
    }
    if (proc.exitCode() != 0 || !QFileInfo::exists(dstPath)) {
        qWarning() << "ffmpeg 转码失败, exit=" << proc.exitCode()
                   << ", src=" << srcPath;
        QFile::remove(dstPath);
        return QString();
    }
    return dstPath;
}

void CaptureWorker::cleanupConvertedFile()
{
    if (m_isConvertedFile && !m_convertedPath.isEmpty()) {
        // ★ 先释放 VideoCapture 文件句柄（Windows 下打开的文件无法删除）
        if (m_cap.isOpened())
            m_cap.release();
        QFile::remove(m_convertedPath);
        m_isConvertedFile = false;
        m_convertedPath.clear();
    }
}

// ──────────────────────────────────────────────────────────────────────

StreamManager::StreamManager(QObject *parent)
    : QObject(parent)
{
    // 允许 cv::Mat 通过 QueuedConnection 跨线程传递
    qRegisterMetaType<cv::Mat>("cv::Mat");
}

StreamManager::~StreamManager()
{
    // 通知所有仍在运行的采集线程停止
    for (auto &kv : m_streams) {
        kv.worker->stop();
        disconnect(kv.worker, nullptr, this, nullptr);
    }
    // 等待线程自然退出（process 返回 → finished → quit）
    for (auto &kv : m_streams) {
        kv.thread->quit();
        if (!kv.thread->wait(3000))
            qWarning() << "capture thread" << kv.thread << "did not stop in 3s";
        delete kv.worker;
        delete kv.thread;
    }
    m_streams.clear();
}

bool StreamManager::startCapture(int streamId, int cameraIndex)
{
    if (m_streams.contains(streamId))
        return false;

    auto *thread = new QThread(this);
    auto *worker = new CaptureWorker(cameraIndex);
    worker->moveToThread(thread);

    // 线程启动 → 在子线程执行 process()
    connect(thread, &QThread::started, worker, &CaptureWorker::process);

    // 子线程发帧 → 主线程转发为 StreamManager 信号（自动 QueuedConnection）
    connect(worker, &CaptureWorker::frameReady, this,
            [this, streamId](const cv::Mat &frame) {
                // ★ 停止后不再转发（防止已入队的帧继续触发 UI 刷新）
                if (m_streams.contains(streamId))
                    emit frameCaptured(streamId, frame);
            });
    connect(worker, &CaptureWorker::errorOccurred, this,
            [this, streamId](const QString &msg) {
                if (m_streams.contains(streamId))
                    emit captureError(streamId, msg);
            });
    // 元数据 / 背景重置请求转发
    connect(worker, &CaptureWorker::videoMetadataReady, this,
            [this, streamId](qint64 totalMs) {
                if (m_streams.contains(streamId))
                    emit videoMetadataReady(streamId, totalMs);
            });
    connect(worker, &CaptureWorker::backgroundResetRequested, this,
            [this, streamId]() {
                if (m_streams.contains(streamId))
                    emit backgroundResetRequested(streamId);
            });

    // ★ 线程自动清理链：process 退出 → finished → quit → deleteLater
    connect(worker, &CaptureWorker::finished, thread, &QThread::quit);
    // ★ 自动结束（播放完毕/异常）时从活动列表移除，防止条目悬挂
    connect(worker, &CaptureWorker::finished, this, [this, streamId]() {
        m_streams.remove(streamId);
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_streams.insert(streamId, StreamContext{thread, worker, true});
    thread->start();
    return true;
}

bool StreamManager::startCaptureFromFile(int streamId, const QString &filePath)
{
    if (m_streams.contains(streamId) || filePath.isEmpty())
        return false;

    auto *thread = new QThread(this);
    auto *worker = new CaptureWorker(filePath);   // 文件模式 Worker
    worker->moveToThread(thread);

    // 线程启动 → 在子线程执行 process()
    connect(thread, &QThread::started, worker, &CaptureWorker::process);

    // 子线程发帧 → 主线程转发为 StreamManager 信号（自动 QueuedConnection）
    connect(worker, &CaptureWorker::frameReady, this,
            [this, streamId](const cv::Mat &frame) {
                if (m_streams.contains(streamId))
                    emit frameCaptured(streamId, frame);
            });
    connect(worker, &CaptureWorker::errorOccurred, this,
            [this, streamId](const QString &msg) {
                if (m_streams.contains(streamId))
                    emit captureError(streamId, msg);
            });
    // 元数据 / 背景重置请求转发
    connect(worker, &CaptureWorker::videoMetadataReady, this,
            [this, streamId](qint64 totalMs) {
                if (m_streams.contains(streamId))
                    emit videoMetadataReady(streamId, totalMs);
            });
    connect(worker, &CaptureWorker::backgroundResetRequested, this,
            [this, streamId]() {
                if (m_streams.contains(streamId))
                    emit backgroundResetRequested(streamId);
            });

    // ★ 线程自动清理链：process 退出 → finished → quit → deleteLater
    connect(worker, &CaptureWorker::finished, thread, &QThread::quit);
    // ★ 自动结束（播放完毕/异常）时从活动列表移除，防止条目悬挂
    connect(worker, &CaptureWorker::finished, this, [this, streamId]() {
        m_streams.remove(streamId);
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    m_streams.insert(streamId, StreamContext{thread, worker, true});
    thread->start();
    return true;
}

bool StreamManager::stopCapture(int streamId)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return false;

    CaptureWorker *worker = it->worker;

    // 1) 置停止标志（原子，子线程下一轮循环退出）
    worker->stop();
    // 2) 断开 worker → 本对象的信号连接（阻止已入队帧继续转发）
    disconnect(worker, nullptr, this, nullptr);
    // 3) 从活动列表移除（UI 立即恢复按钮状态）
    m_streams.erase(it);

    // 线程通过 finished → quit → deleteLater 链自行清理，不阻塞主线程
    return true;
}

bool StreamManager::seekPosition(int streamId, qint64 ms)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return false;

    // 投递到子线程执行 handleSeek（process() 内 processEvents 会及时处理）
    return QMetaObject::invokeMethod(it->worker, "handleSeek",
                                     Qt::QueuedConnection,
                                     Q_ARG(qint64, ms));
}

bool StreamManager::setPaused(int streamId, bool paused)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end() || !it->worker)
        return false;

    it->worker->setPaused(paused);   // 原子标志，跨线程安全
    return true;
}

bool StreamManager::setPlaybackSpeed(int streamId, double speed)
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end() || !it->worker)
        return false;

    it->worker->setPlaybackSpeed(speed);   // 原子，跨线程安全
    return true;
}

qint64 StreamManager::currentPositionMs(int streamId) const
{
    auto it = m_streams.find(streamId);
    if (it == m_streams.end())
        return -1;

    return it->worker->positionMs();   // 原子读取
}

int StreamManager::activeCount() const
{
    return m_streams.size();
}
