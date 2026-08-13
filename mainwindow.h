#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QElapsedTimer>
#include <QSlider>
#include <QList>
#include <QHash>
#include <QVariant>
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTextEdit;
class QTabWidget;
class QSplitter;
class QVBoxLayout;
class VideoAnalyzerController;

// ──────────────────────────────────────────────────────────────────────
// 带事件标记的进度条：在普通 QSlider 上绘制红色小圆点标记事件位置
// 通过 setEventMarkers(ms 列表, 总时长) 提供标记数据
// ──────────────────────────────────────────────────────────────────────
class ProgressSlider : public QSlider
{
    Q_OBJECT

public:
    explicit ProgressSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
        // 启用鼠标跟踪：不按下也能收到 mouseMoveEvent（悬停显示时间）
        setMouseTracking(true);
    }

    // 设置事件标记（按时间排序的 ms 列表）与视频总时长
    void setEventMarkers(const QList<qint64> &timestampsMs, qint64 totalMs)
    {
        m_markers = timestampsMs;
        m_totalMs = totalMs;
        update();
    }

    void clearEventMarkers()
    {
        m_markers.clear();
        update();
    }

signals:
    // 鼠标悬停在某位置 → 对应时间戳（ms）；离开（-1）清除状态栏
    void hoveredAt(qint64 ms);

protected:
    void paintEvent(QPaintEvent *event) override
    {
        // 先画普通进度条
        QSlider::paintEvent(event);

        // 再叠画事件红点
        if (m_markers.isEmpty() || m_totalMs <= 0)
            return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 50, 50, 220));   // 半透明红

        const int grooveH = 6;                   // 槽高度
        const int markerD = 9;                   // 红点直径
        const int y = (height() - grooveH) / 2;  // 槽中心线（近似）

        for (qint64 ms : m_markers) {
            if (ms < 0 || ms > m_totalMs)
                continue;
            const double ratio = static_cast<double>(ms) / m_totalMs;
            // 留出两侧 padding，红点画在槽中心线
            const int x = static_cast<int>(ratio * (width() - 10)) + 5;
            p.drawEllipse(QPointF(x, y + grooveH / 2.0), markerD / 2.0, markerD / 2.0);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        QSlider::mouseMoveEvent(event);
        if (m_totalMs <= 0)
            return;

        // 鼠标 x 位置 → 比例 → 时间戳
        const double ratio = static_cast<double>(event->pos().x()) / std::max(1, width());
        const qint64 ms = static_cast<qint64>(ratio * m_totalMs);
        emit hoveredAt(ms);
    }

    void leaveEvent(QEvent *event) override
    {
        QSlider::leaveEvent(event);
        emit hoveredAt(-1);   // 离开 → 清除显示
    }

private:
    QList<qint64> m_markers;
    qint64 m_totalMs = 0;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

public slots:
    // 从工作线程收到新帧后更新视频显示
    void updateFrame(int streamId, const QImage &image);
    // 事件检测结果追加到事件列表
    void onEventDetected(int streamId, qint64 timestampMs, const QString &description,
                         const QString &keyframePath);
    // 摘要生成完成（含关键词）
    void onSummaryReady(int streamId, const QString &summary,
                        const QStringList &keywords);
    // 错误提示
    void onError(int streamId, const QString &message);
    // 检索结果
    void onSearchResults(const QList<QHash<QString, QVariant>> &results);
    // 性能监控（每秒刷新 FPS 与数据库待写入数）
    void updatePerfStats();
    // 视频元数据（总时长 ms）→ 初始化进度条范围
    void onVideoMetadataReady(int streamId, qint64 totalMs);
    // 事件源切换（摄像头/新视频）→ 清空当前 UI 事件与标记
    void onEventsRefreshed();

private slots:
    void onStartStream();
    void onSelectVideoFile();
    void onStopStream();
    void onGenerateSummary();
    void onSearch();
    void onEventItemDoubleClicked();
    // 检测参数调优（valueChanged 时实时生效）
    void onAreaThresholdChanged(double value);
    void onCooldownChanged(int value);
    void onDetectIntervalChanged(int value);
    void onWarmupChanged(int value);
    // ── 播放器控制 ──
    void onTogglePause();
    void onSeekBack5s();
    void onSeekForward5s();
    void onProgressSliderPressed();
    void onProgressSliderReleased();
    void onProgressSliderMoved(int value);
    void onProgressSliderHovered(qint64 ms);  // 悬停进度条显示时间
    void updateProgressPosition();     // 播放中每秒刷新进度条
    // ── 检索跳转 ──
    void onSearchPrev();
    void onSearchNext();
    void onSearchItemClicked(QListWidgetItem *item);  // 检索结果单击定位
    // ── Tab2 摘要事件跳转 ──
    void onSummaryEventClicked(QListWidgetItem *item);  // 摘要事件单击定位
    void onSummaryPrev();                              // 摘要事件上一处
    void onSummaryNext();                              // 摘要事件下一处
    // ── 倍速播放 ──
    void onSpeedChanged(int index);
    // ── 输入源切换 ──
    void onSourceChanged(int index);

private:
    void buildUi();
    void connectSignals();
    void buildControlBar(QVBoxLayout *mainLayout);
    void buildVideoArea(QVBoxLayout *mainLayout);
    void buildPanelTabs(QSplitter *splitter);

    // 格式化毫秒 → "HH:MM:SS"
    static QString formatTime(qint64 ms);

    Ui::MainWindow *ui;
    VideoAnalyzerController *m_controller = nullptr;

    // 控制栏
    QComboBox   *m_sourceCombo = nullptr;   // 输入源: USB摄像头 / 本地视频文件
    QPushButton *m_startButton = nullptr;
    QPushButton *m_fileButton = nullptr;    // 选择视频文件（文件模式显示）
    QPushButton *m_stopButton = nullptr;
    QSpinBox    *m_cameraSpin = nullptr;
    QLineEdit   *m_apiKeyEdit = nullptr;
    QPushButton *m_summaryButton = nullptr;

    // 视频显示区（播放器）
    QWidget       *m_playerArea = nullptr;   // 播放器整体容器
    QLabel       *m_videoLabel = nullptr;
    ProgressSlider *m_progressSlider = nullptr;  // 带事件标记的进度条
    QPushButton  *m_pauseButton = nullptr;       // 暂停/播放
    QPushButton  *m_back5Button = nullptr;       // 快退 5s
    QPushButton  *m_forward5Button = nullptr;    // 快进 5s
    QComboBox    *m_speedCombo = nullptr;        // 倍速: 0.3x~2.0x
    bool m_seeking = false;                      // 拖动进度条中（不刷新位置）
    bool m_isFileMode = false;                   // 当前是否为文件模式
    bool m_isPaused = false;
    qint64 m_totalMs = 0;                        // 当前视频总时长

    // 右侧面板
    QTabWidget *m_panelTabs = nullptr;
    QListWidget *m_eventList = nullptr;       // Tab1: 事件记录
    QTextEdit   *m_summaryText = nullptr;     // Tab2: AI 摘要
    QLabel      *m_keywordsLabel = nullptr;   // Tab2: 关键词展示
    QListWidget *m_summaryEventList = nullptr;   // ★ Tab2: 摘要相关事件列表（可点击跳转）
    QPushButton *m_summaryPrevButton = nullptr;  // ★ Tab2: 上一处
    QPushButton *m_summaryNextButton = nullptr;  // ★ Tab2: 下一处
    QList<qint64> m_summaryEventPositions;       // ★ Tab2: 摘要事件视频位置列表（跳转用）
    QLineEdit   *m_searchEdit = nullptr;      // Tab3: 检索输入
    QPushButton *m_searchButton = nullptr;
    QPushButton *m_searchPrevButton = nullptr; // Tab3: 上一处
    QPushButton *m_searchNextButton = nullptr; // Tab3: 下一处
    QListWidget *m_searchResultList = nullptr; // Tab3: 检索结果
    // 检索跳转：匹配事件的 timestamp_ms 列表（按时间排序）
    QList<qint64> m_searchResultVideoPositions;   // 视频内位置 ms，按时间排序，跳转用
    // LLM 自动标注：上次自动请求时间（30s 间隔抑制）
    qint64 m_lastAutoSummaryMs = 0;

    // Tab4: 参数调优
    QSlider *m_sliderThreshold = nullptr;    // 灵敏度 1-20 → 0.01-0.20
    QLabel  *m_areaValueLabel = nullptr;     // 当前阈值数值显示
    QSlider *m_sliderCooldown = nullptr;     // 冷却 10-100 → 1000-10000ms
    QLabel  *m_cooldownValueLabel = nullptr; // 当前冷却数值显示
    QSpinBox       *m_intervalSpin = nullptr;  // 检测间隔帧
    QSpinBox       *m_warmupSpin = nullptr;    // 预热帧数

    // 状态栏
    QLabel *m_frameCountLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_perfLabel = nullptr;      // 实时性能监控: FPS + 数据库待写入

    // 性能监控
    int m_frameCount = 0;
    int m_lastFrameCount = 0;
    QElapsedTimer m_fpsTimer;           // 每秒计算 FPS
    int m_eventCount = 0;
};
#endif // MAINWINDOW_H
