#ifndef STREAMMANAGER_H
#define STREAMMANAGER_H

#include <QObject>
#include <QHash>
#include <QImage>
#include <opencv2/opencv.hpp>

class QThread;
class CaptureWorker;

// 视频流管理：每路视频源一个 QThread，线程内 cv::VideoCapture 循环抓帧
class StreamManager : public QObject
{
    Q_OBJECT

public:
    explicit StreamManager(QObject *parent = nullptr);
    ~StreamManager() override;

    // 启动一路采集线程（本地摄像头）
    bool startCapture(int streamId, int cameraIndex);
    // 启动一路采集线程（本地视频文件，播放结束自动停止）
    bool startCaptureFromFile(int streamId, const QString &filePath);
    // 停止并回收线程
    bool stopCapture(int streamId);
    // 跳转（视频文件模式）：通过 invokeMethod 在子线程调用 cap.set(POS_MSEC)
    bool seekPosition(int streamId, qint64 ms);
    // 暂停/恢复（文件模式）
    bool setPaused(int streamId, bool paused);
    // 设置播放速度（倍速播放：1.0x 正常, 2.0x 快进, 0.5x 慢放；暂停时忽略）
    bool setPlaybackSpeed(int streamId, double speed);
    // 当前播放位置（ms，原子读取线程安全）
    qint64 currentPositionMs(int streamId) const;
    int  activeCount() const;

signals:
    // 工作线程抓到的原始帧（Mat 引用计数共享，需在接收侧 clone）
    void frameCaptured(int streamId, const cv::Mat &frame);
    // 采集异常（摄像头打开失败 / 断开）
    void captureError(int streamId, const QString &message);
    // 视频元数据就绪（文件总时长 ms，用于初始化进度条范围）
    void videoMetadataReady(int streamId, qint64 totalMs);
    // 跳转后需要重置背景模型（避免跳转首帧误报）
    void backgroundResetRequested(int streamId);

private:
    struct StreamContext
    {
        QThread *thread = nullptr;
        CaptureWorker *worker = nullptr;
        bool running = false;
    };

    QHash<int, StreamContext> m_streams;
};

#endif // STREAMMANAGER_H
