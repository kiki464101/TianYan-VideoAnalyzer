#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "VideoAnalyzerController.h"

#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QTextEdit>
#include <QTabWidget>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QStatusBar>
#include <QDateTime>
#include <QMessageBox>
#include <QFileDialog>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_controller(new VideoAnalyzerController(this))
{
    ui->setupUi(this);

    buildUi();
    connectSignals();

    // 初始化控制器（数据库 / 图片目录）
    m_controller->init(QStringLiteral("E:/VideoProject/video.db"),
                       QStringLiteral("E:/pics"));

    // 默认检测参数（与 EventDetector 默认值一致）
    m_controller->setAreaThreshold(0.01);
    m_controller->setCooldownMs(5000);
    m_controller->setDetectInterval(5);
    m_controller->setWarmupFrames(50);

    setWindowTitle(tr("VideoAnalyzer - 实时视频流分析与AI摘要"));
    resize(1280, 800);
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ────────────────────────────── UI 构建 ──────────────────────────────

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    buildControlBar(mainLayout);
    buildVideoArea(mainLayout);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_playerArea);
    buildPanelTabs(splitter);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({860, 380});
    mainLayout->addWidget(splitter, 1);

    setCentralWidget(central);

    // 状态栏
    m_frameCountLabel = new QLabel(tr("帧数: 0"), this);
    m_statusLabel = new QLabel(tr("就绪"), this);
    m_perfLabel = new QLabel(tr("FPS: -- | 数据库待写入: 0"), this);
    statusBar()->addPermanentWidget(m_frameCountLabel);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_perfLabel);

    // 性能监控：每秒刷新 FPS 与数据库待写入数
    m_fpsTimer.start();
    auto *perfTimer = new QTimer(this);
    perfTimer->setInterval(1000);
    connect(perfTimer, &QTimer::timeout, this, &MainWindow::updatePerfStats);
    perfTimer->start();
}

void MainWindow::buildControlBar(QVBoxLayout *mainLayout)
{
    auto *ctrlBar = new QHBoxLayout;

    // 输入源切换：USB 摄像头 / 本地视频文件
    m_sourceCombo = new QComboBox(this);
    m_sourceCombo->addItem(tr("USB 摄像头"));
    m_sourceCombo->addItem(tr("本地视频文件"));
    m_sourceCombo->setFixedWidth(140);

    m_cameraSpin = new QSpinBox(this);
    m_cameraSpin->setRange(0, 8);
    m_cameraSpin->setValue(0);
    m_cameraSpin->setPrefix(tr("摄像头 #"));
    m_cameraSpin->setFixedWidth(110);

    m_startButton = new QPushButton(tr("▶ 启动采集"), this);
    m_startButton->setStyleSheet("QPushButton { background:#2d8f3e; color:white;"
                                 " font-weight:bold; padding:6px 18px; border-radius:4px; }"
                                 "QPushButton:hover { background:#36a64f; }");
    m_fileButton = new QPushButton(tr("📁 选择视频文件"), this);
    m_fileButton->setStyleSheet("QPushButton { background:#2d6a9f; color:white;"
                                " font-weight:bold; padding:6px 18px; border-radius:4px; }"
                                "QPushButton:hover { background:#3582bd; }");
    m_stopButton = new QPushButton(tr("■ 停止"), this);
    m_stopButton->setStyleSheet("QPushButton { background:#b3402a; color:white;"
                                " font-weight:bold; padding:6px 18px; border-radius:4px; }"
                                "QPushButton:hover { background:#c94f36; }");
    m_stopButton->setEnabled(false);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText(tr("DeepSeek API Key（用于AI摘要）"));
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setMinimumWidth(240);
    m_apiKeyEdit->setText(QStringLiteral("«redacted:sk-…»"));

    m_summaryButton = new QPushButton(tr("✨ 生成摘要"), this);
    m_summaryButton->setEnabled(false);

    ctrlBar->addWidget(m_sourceCombo);
    ctrlBar->addWidget(m_cameraSpin);
    ctrlBar->addWidget(m_startButton);
    ctrlBar->addWidget(m_fileButton);
    ctrlBar->addWidget(m_stopButton);
    ctrlBar->addSpacing(20);
    ctrlBar->addWidget(m_apiKeyEdit, 1);
    ctrlBar->addWidget(m_summaryButton);

    mainLayout->addLayout(ctrlBar);

    // 默认模式：USB 摄像头（显示摄像头选择，隐藏文件按钮）
    m_fileButton->setVisible(false);
    m_isFileMode = false;
}

void MainWindow::buildVideoArea(QVBoxLayout *mainLayout)
{
    // 视频显示区改为播放器布局：视频标签 + 进度条 + 控制栏
    m_playerArea = new QWidget(this);
    auto *playerLayout = new QVBoxLayout(m_playerArea);
    playerLayout->setContentsMargins(0, 0, 0, 0);
    playerLayout->setSpacing(6);

    m_videoLabel = new QLabel(m_playerArea);
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setText(tr("等待视频信号...\n选择输入源后点击「启动采集」"));
    m_videoLabel->setMinimumSize(640, 320);
    m_videoLabel->setStyleSheet(
        "QLabel { background-color:#111111; color:#777777; font-size:16px;"
        " border:2px solid #333333; border-radius:6px; }");
    playerLayout->addWidget(m_videoLabel, 1);

    // ── 进度条（带事件标记） ──────────────────────────────────────
    m_progressSlider = new ProgressSlider(Qt::Horizontal, m_playerArea);
    m_progressSlider->setRange(0, 1000);
    m_progressSlider->setValue(0);
    m_progressSlider->setStyleSheet(
        "QSlider::groove:horizontal { height:6px; background:#333333; border-radius:3px; }"
        "QSlider::handle:horizontal { width:14px; margin:-5px 0; border-radius:7px;"
        " background:#4ec9b0; }"
        "QSlider::sub-page:horizontal { background:#4ec9b0; border-radius:3px; }");
    playerLayout->addWidget(m_progressSlider);

    // ── 播放控制栏 ────────────────────────────────────────────────
    auto *ctrlRow = new QHBoxLayout;
    m_back5Button = new QPushButton(tr("⏪ 快退5s"), m_playerArea);
    m_pauseButton = new QPushButton(tr("⏸ 暂停"), m_playerArea);
    m_forward5Button = new QPushButton(tr("快进5s ⏩"), m_playerArea);
    // 倍速调节下拉框（0.3x 慢放 ~ 2.0x 快进）
    m_speedCombo = new QComboBox(m_playerArea);
    m_speedCombo->addItem(tr("0.3x"), 0.3);
    m_speedCombo->addItem(tr("0.5x"), 0.5);
    m_speedCombo->addItem(tr("0.8x"), 0.8);
    m_speedCombo->addItem(tr("1.0x"), 1.0);
    m_speedCombo->addItem(tr("1.5x"), 1.5);
    m_speedCombo->addItem(tr("2.0x"), 2.0);
    m_speedCombo->setCurrentIndex(3);   // 默认 1.0x
    for (QPushButton *b : {m_back5Button, m_pauseButton, m_forward5Button}) {
        b->setStyleSheet("QPushButton { padding:5px 14px; border-radius:4px;"
                         " background:#2a2a2a; color:#e0e0e0; border:1px solid #444; }"
                         "QPushButton:hover { background:#3a3a3a; }"
                         "QPushButton:disabled { color:#666666; }");
    }
    ctrlRow->addStretch(1);
    ctrlRow->addWidget(m_back5Button);
    ctrlRow->addWidget(m_pauseButton);
    ctrlRow->addWidget(m_forward5Button);
    ctrlRow->addWidget(m_speedCombo);
    ctrlRow->addStretch(1);
    playerLayout->addLayout(ctrlRow);

    mainLayout->addWidget(m_playerArea, 3);
}

void MainWindow::buildPanelTabs(QSplitter *splitter)
{
    m_panelTabs = new QTabWidget(this);
    m_panelTabs->setMinimumWidth(360);

    // ── Tab1: 事件记录 ────────────────────────────────────────────
    auto *eventTab = new QWidget(this);
    auto *eventLayout = new QVBoxLayout(eventTab);
    eventLayout->setContentsMargins(6, 6, 6, 6);
    m_eventList = new QListWidget(eventTab);
    m_eventList->setStyleSheet("QListWidget { background:#1b1b1b; color:#e0e0e0;"
                               " border:1px solid #333; border-radius:4px; font-size:12px; }");
    eventLayout->addWidget(m_eventList);
    m_panelTabs->addTab(eventTab, tr("事件记录"));

    // ── Tab2: AI 摘要 + 关键词 ────────────────────────────────────
    auto *summaryTab = new QWidget(this);
    auto *summaryLayout = new QVBoxLayout(summaryTab);
    summaryLayout->setContentsMargins(6, 6, 6, 6);
    m_summaryText = new QTextEdit(summaryTab);
    m_summaryText->setReadOnly(true);
    m_summaryText->setPlaceholderText(tr("检测到事件后，点击「生成摘要」调用 DeepSeek 生成文字摘要..."));
    m_summaryText->setStyleSheet("QTextEdit { background:#1b1b1b; color:#e0e0e0;"
                                 " border:1px solid #333; border-radius:4px; font-size:13px; }");
    m_keywordsLabel = new QLabel(tr("关键词：--"), summaryTab);
    m_keywordsLabel->setWordWrap(true);
    m_keywordsLabel->setStyleSheet("QLabel { color:#4ec9b0; font-weight:bold;"
                                   " padding:4px; }");
    summaryLayout->addWidget(m_summaryText, 1);
    summaryLayout->addWidget(m_keywordsLabel);

    // ★ 摘要相关事件列表（可点击跳转 + 上一处/下一处）
    m_summaryEventList = new QListWidget(summaryTab);
    m_summaryEventList->setStyleSheet(
        "QListWidget { background:#1b1b1b; color:#e0e0e0;"
        " border:1px solid #333; border-radius:4px; font-size:12px; }");
    summaryLayout->addWidget(m_summaryEventList, 1);

    auto *summaryNavRow = new QHBoxLayout;
    m_summaryPrevButton = new QPushButton(tr("⏪ 上一处"), summaryTab);
    m_summaryNextButton = new QPushButton(tr("下一处 ⏩"), summaryTab);
    for (QPushButton *b : {m_summaryPrevButton, m_summaryNextButton}) {
        b->setEnabled(false);
        b->setStyleSheet("QPushButton { padding:4px 10px; }");
        summaryNavRow->addWidget(b);
    }
    summaryNavRow->addStretch(1);
    summaryLayout->addLayout(summaryNavRow);

    // ★ 摘要事件跳转绑定
    connect(m_summaryEventList, &QListWidget::itemClicked,
            this, &MainWindow::onSummaryEventClicked);
    connect(m_summaryPrevButton, &QPushButton::clicked,
            this, &MainWindow::onSummaryPrev);
    connect(m_summaryNextButton, &QPushButton::clicked,
            this, &MainWindow::onSummaryNext);

    m_panelTabs->addTab(summaryTab, tr("AI 摘要"));

    // ── Tab3: 关键词检索 ──────────────────────────────────────────
    auto *searchTab = new QWidget(this);
    auto *searchLayout = new QVBoxLayout(searchTab);
    searchLayout->setContentsMargins(6, 6, 6, 6);
    auto *searchRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit(searchTab);
    m_searchEdit->setPlaceholderText(tr("输入关键词，如：红色汽车、有人跑过..."));
    m_searchButton = new QPushButton(tr("搜索"), searchTab);
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchButton);
    // 检索定位：上一处 / 下一处
    auto *jumpRow = new QHBoxLayout;
    m_searchPrevButton = new QPushButton(tr("⬆ 上一处"), searchTab);
    m_searchNextButton = new QPushButton(tr("下一处 ⬇"), searchTab);
    for (QPushButton *b : {m_searchPrevButton, m_searchNextButton}) {
        b->setEnabled(false);   // 有检索结果才启用
        b->setStyleSheet("QPushButton { padding:5px 12px; border-radius:4px;"
                         " background:#2a2a2a; color:#e0e0e0; border:1px solid #444; }"
                         "QPushButton:hover { background:#3a3a3a; }"
                         "QPushButton:disabled { color:#666666; }");
    }
    jumpRow->addWidget(m_searchPrevButton);
    jumpRow->addWidget(m_searchNextButton);
    jumpRow->addStretch(1);
    m_searchResultList = new QListWidget(searchTab);
    m_searchResultList->setStyleSheet("QListWidget { background:#1b1b1b; color:#e0e0e0;"
                                      " border:1px solid #333; border-radius:4px; font-size:12px; }");
    searchLayout->addLayout(searchRow);
    searchLayout->addLayout(jumpRow);
    searchLayout->addWidget(m_searchResultList, 1);
    m_panelTabs->addTab(searchTab, tr("智能检索"));

    // ── Tab4: 检测参数调优 ────────────────────────────────────────
    auto *paramTab = new QWidget(this);
    auto *paramLayout = new QVBoxLayout(paramTab);
    paramLayout->setContentsMargins(10, 10, 10, 10);
    paramLayout->setSpacing(12);

    auto *detectGroup = new QGroupBox(tr("运动检测参数"), paramTab);
    auto *form = new QFormLayout(detectGroup);

    // 运动面积阈值：Slider 1-20 → 0.01-0.20（int → double 除以 100.0）
    auto *thresholdRow = new QWidget(detectGroup);
    auto *thresholdLayout = new QHBoxLayout(thresholdRow);
    thresholdLayout->setContentsMargins(0, 0, 0, 0);
    m_sliderThreshold = new QSlider(Qt::Horizontal, thresholdRow);
    m_sliderThreshold->setRange(1, 20);
    m_sliderThreshold->setValue(1);          // 默认 0.01
    m_areaValueLabel = new QLabel(tr("0.01"), thresholdRow);
    m_areaValueLabel->setMinimumWidth(48);
    m_areaValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    thresholdLayout->addWidget(m_sliderThreshold, 1);
    thresholdLayout->addWidget(m_areaValueLabel);
    form->addRow(tr("灵敏度(面积阈值):"), thresholdRow);

    // 冷却时间：Slider 10-100 → 1000-10000ms（int × 100）
    auto *cooldownRow = new QWidget(detectGroup);
    auto *cooldownLayout = new QHBoxLayout(cooldownRow);
    cooldownLayout->setContentsMargins(0, 0, 0, 0);
    m_sliderCooldown = new QSlider(Qt::Horizontal, cooldownRow);
    m_sliderCooldown->setRange(10, 100);
    m_sliderCooldown->setValue(50);          // 默认 5000ms
    m_cooldownValueLabel = new QLabel(tr("5000 ms"), cooldownRow);
    m_cooldownValueLabel->setMinimumWidth(48);
    m_cooldownValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cooldownLayout->addWidget(m_sliderCooldown, 1);
    cooldownLayout->addWidget(m_cooldownValueLabel);
    form->addRow(tr("冷却时间:"), cooldownRow);

    m_intervalSpin = new QSpinBox(detectGroup);
    m_intervalSpin->setRange(1, 30);
    m_intervalSpin->setValue(5);
    m_intervalSpin->setSuffix(tr(" 帧"));
    form->addRow(tr("检测间隔:"), m_intervalSpin);

    m_warmupSpin = new QSpinBox(detectGroup);
    m_warmupSpin->setRange(10, 300);
    m_warmupSpin->setSingleStep(10);
    m_warmupSpin->setValue(50);
    m_warmupSpin->setSuffix(tr(" 帧"));
    form->addRow(tr("预热帧数:"), m_warmupSpin);

    paramLayout->addWidget(detectGroup);

    auto *hint = new QLabel(
        tr("参数修改后实时生效。\n"
           "• 面积阈值：运动像素占比超过该值才触发事件，越大越不敏感。\n"
           "• 冷却时间：同路视频源两次事件的最小间隔，避免刷屏。\n"
           "• 检测间隔：每 N 帧检测一次，越大越省 CPU。\n"
           "• 预热帧数：背景建模初期只学习不判定，避免误报。"),
        paramTab);
    hint->setWordWrap(true);
    hint->setStyleSheet("QLabel { color:#888888; font-size:12px; }");
    paramLayout->addWidget(hint);
    paramLayout->addStretch(1);

    m_panelTabs->addTab(paramTab, tr("参数调优"));

    splitter->addWidget(m_panelTabs);
}

void MainWindow::connectSignals()
{
    // 控制器 → 界面
    connect(m_controller, &VideoAnalyzerController::frameReady,
            this, &MainWindow::updateFrame);
    connect(m_controller, &VideoAnalyzerController::eventDetected,
            this, &MainWindow::onEventDetected);
    // ★ 事件源切换（摄像头/新视频）→ 清空当前 UI 事件
    connect(m_controller, &VideoAnalyzerController::eventsRefreshed,
            this, &MainWindow::onEventsRefreshed);
    connect(m_controller, &VideoAnalyzerController::summaryReady,
            this, &MainWindow::onSummaryReady);
    connect(m_controller, &VideoAnalyzerController::searchResults,
            this, &MainWindow::onSearchResults);
    connect(m_controller, &VideoAnalyzerController::errorOccurred,
            this, &MainWindow::onError);

    // 按钮 → 操作
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::onStartStream);
    connect(m_fileButton,  &QPushButton::clicked, this, &MainWindow::onSelectVideoFile);
    connect(m_stopButton,  &QPushButton::clicked, this, &MainWindow::onStopStream);
    connect(m_summaryButton, &QPushButton::clicked, this, &MainWindow::onGenerateSummary);
    connect(m_searchButton, &QPushButton::clicked, this, &MainWindow::onSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearch);

    // API Key 实时同步：输入即推送（不用等点生成摘要）
    connect(m_apiKeyEdit, &QLineEdit::textChanged, this,
            [this](const QString &key) {
                m_controller->setApiKey(key);
            });

    // 事件列表双击 → 查看关键帧大图
    connect(m_eventList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onEventItemDoubleClicked);
    connect(m_searchResultList, &QListWidget::itemDoubleClicked,
            this, &MainWindow::onEventItemDoubleClicked);

    // 检测参数调优 → 实时转发给控制器
    // Slider(1-20) → threshold(0.01-0.20)：int 除以 100.0
    connect(m_sliderThreshold, &QSlider::valueChanged, this,
            [this](int v) {
                const double ratio = v / 100.0;
                m_areaValueLabel->setText(QString::number(ratio, 'f', 2));
                onAreaThresholdChanged(ratio);
            });
    // Slider(10-100) → cooldown(1000-10000ms)：int × 100
    connect(m_sliderCooldown, &QSlider::valueChanged, this,
            [this](int v) {
                const int ms = v * 100;
                m_cooldownValueLabel->setText(tr("%1 ms").arg(ms));
                onCooldownChanged(ms);
            });
    connect(m_intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onDetectIntervalChanged);
    connect(m_warmupSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onWarmupChanged);

    // ── 输入源切换 ──────────────────────────────────────────────────
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSourceChanged);

    // ── 播放器控制 ──────────────────────────────────────────────────
    connect(m_controller, &VideoAnalyzerController::videoMetadataReady,
            this, &MainWindow::onVideoMetadataReady);
    connect(m_pauseButton, &QPushButton::clicked, this, &MainWindow::onTogglePause);
    connect(m_back5Button, &QPushButton::clicked, this, &MainWindow::onSeekBack5s);
    connect(m_forward5Button, &QPushButton::clicked, this, &MainWindow::onSeekForward5s);
    // 进度条拖动 → 跳转（按下置 seeking 标志，松开执行跳转）
    connect(m_progressSlider, &QSlider::sliderPressed,
            this, &MainWindow::onProgressSliderPressed);
    connect(m_progressSlider, &QSlider::sliderReleased,
            this, &MainWindow::onProgressSliderReleased);
    connect(m_progressSlider, &QSlider::sliderMoved,
            this, &MainWindow::onProgressSliderMoved);
    // 鼠标悬停进度条 → 状态栏显示对应时间点
    connect(m_progressSlider, &ProgressSlider::hoveredAt,
            this, &MainWindow::onProgressSliderHovered);
    // 播放中每秒刷新进度条位置
    auto *progressTimer = new QTimer(this);
    progressTimer->setInterval(1000);
    connect(progressTimer, &QTimer::timeout, this, &MainWindow::updateProgressPosition);
    progressTimer->start();

    // ── 检索定位 ────────────────────────────────────────────────────
    connect(m_searchPrevButton, &QPushButton::clicked, this, &MainWindow::onSearchPrev);
    connect(m_searchNextButton, &QPushButton::clicked, this, &MainWindow::onSearchNext);
    // 检索结果单击 → 定位到该事件时间戳（选中即跳转）
    connect(m_searchResultList, &QListWidget::itemClicked,
            this, &MainWindow::onSearchItemClicked);
    // 倍速调节 → 转发给控制器
    connect(m_speedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSpeedChanged);
}

// ────────────────────────────── 槽函数 ──────────────────────────────

void MainWindow::onStartStream()
{
    // 输入源为 USB 摄像头模式
    m_isFileMode = false;
    int cameraIndex = m_cameraSpin->value();
    // ★ 先停止旧流（同一 streamId 只允许一路流）
    m_controller->stopStream(0);
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    if (m_controller->startStream(0, cameraIndex)) {
        m_startButton->setEnabled(false);
        m_stopButton->setEnabled(true);
        m_statusLabel->setText(tr("正在采集摄像头 #%1 ...").arg(cameraIndex));
        m_videoLabel->setText(tr("连接摄像头 #%1 ..."));
        // 摄像头模式：进度条与控制栏保持可见，但位置刷新/跳转逻辑被文件模式限制
        m_progressSlider->setValue(0);
        m_progressSlider->clearEventMarkers();
        m_isPaused = false;
        m_pauseButton->setText(tr("⏸ 暂停"));
    } else {
        m_statusLabel->setText(tr("启动失败：无法打开摄像头 #%1").arg(cameraIndex));
    }
}

void MainWindow::onSelectVideoFile()
{
    // 输入源为本地视频文件模式
    m_isFileMode = true;
    // 选择本地视频文件（mp4/avi/mkv 等 OpenCV 支持格式）
    const QString path = QFileDialog::getOpenFileName(
        this, tr("选择视频文件"), QString(),
        tr("视频文件 (*.mp4 *.avi *.mkv *.mov *.wmv *.flv *.m4v);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        m_isFileMode = false;
        return;   // 用户取消
    }

    // ★ 修复：先停止旧流（StreamManager 同一 streamId 只允许一路流，
    //   不停止会导致 startCaptureFromFile 因 m_streams.contains 拒绝，
    //   表现为"打开新视频后事件流不更新"）
    m_controller->stopStream(0);
    // ★ 立即清空旧视频的 UI 事件（事件记录/摘要/标记/检索）
    onEventsRefreshed();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);

    if (m_controller->startStreamFromFile(0, path)) {
        m_startButton->setEnabled(false);
        m_stopButton->setEnabled(true);
        m_statusLabel->setText(tr("正在播放视频: %1").arg(path));
        m_videoLabel->setText(tr("加载视频文件..."));
        m_isPaused = false;
        m_pauseButton->setText(tr("⏸ 暂停"));
    } else {
        m_statusLabel->setText(tr("无法打开视频文件: %1").arg(path));
        m_isFileMode = false;
    }
}

void MainWindow::onStopStream()
{
    if (m_controller->stopStream(0)) {
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
        m_statusLabel->setText(tr("已停止采集"));
        m_videoLabel->setText(tr("已停止。点击「启动采集」重新打开"));
        // 重置播放器状态
        m_progressSlider->setValue(0);
        m_progressSlider->clearEventMarkers();
        m_isPaused = false;
        m_isFileMode = false;
        m_totalMs = 0;
        m_pauseButton->setText(tr("⏸ 暂停"));
        m_searchResultVideoPositions.clear();
        m_searchPrevButton->setEnabled(false);
        m_searchNextButton->setEnabled(false);
    }
}

void MainWindow::onGenerateSummary()
{
    if (m_eventCount == 0) {
        m_statusLabel->setText(tr("暂无可摘要的事件"));
        return;
    }
    // API Key 已通过 textChanged 实时同步；这里兜底一次
    m_controller->setApiKey(m_apiKeyEdit->text());
    m_controller->requestSummary(0);
    m_statusLabel->setText(tr("正在请求 AI 生成摘要..."));
}

void MainWindow::onSearch()
{
    QString kw = m_searchEdit->text().trimmed();
    m_searchResultList->clear();
    if (kw.isEmpty()) {
        m_statusLabel->setText(tr("请输入检索关键词"));
        return;
    }
    m_statusLabel->setText(tr("搜索「%1」...").arg(kw));
    m_controller->searchEvents(kw);
}

void MainWindow::onSearchResults(const QList<QHash<QString, QVariant>> &results)
{
    m_searchResultList->clear();
    m_searchResultVideoPositions.clear();
    if (results.isEmpty()) {
        m_searchResultList->addItem(tr("（无匹配结果）"));
        m_statusLabel->setText(tr("检索完成：无结果"));
        m_searchPrevButton->setEnabled(false);
        m_searchNextButton->setEnabled(false);
        return;
    }
    for (const auto &row : results) {
        const QString desc = row.value(QStringLiteral("description")).toString();
        const QString time = QDateTime::fromMSecsSinceEpoch(
            row.value(QStringLiteral("timestamp_ms")).toLongLong())
                                 .toString("MM-dd HH:mm:ss");
        const QString keyframe = row.value(QStringLiteral("keyframe_path")).toString();
        const qint64 pos = row.value(QStringLiteral("video_position_ms")).toLongLong();
        // ★ B5: 来源标注（summaries 表反查命中 → "[摘要]" 前缀）
        const QString sourceTag =
            row.value(QStringLiteral("_source")).toString() == QStringLiteral("summary")
                ? QStringLiteral("[摘要] ")
                : QString();
        auto *item = new QListWidgetItem(tr("[%1] %2%3").arg(time, sourceTag, desc));
        item->setData(Qt::UserRole, keyframe);
        item->setData(Qt::UserRole + 1, pos);   // ★ 视频内位置 ms（单击定位用）
        m_searchResultList->addItem(item);
        // ★ 跳转列表只收 pos > 0 的项（过滤老数据默认 0，但仍显示供查看）
        if (pos > 0)
            m_searchResultVideoPositions << pos;
    }
    // 按时间排序
    std::sort(m_searchResultVideoPositions.begin(), m_searchResultVideoPositions.end());
    // 文件模式且有结果 → 启用定位按钮
    const bool canJump = m_isFileMode && !m_searchResultVideoPositions.isEmpty();
    m_searchPrevButton->setEnabled(canJump);
    m_searchNextButton->setEnabled(canJump);

    m_statusLabel->setText(tr("检索完成：%1 条结果，可跳转定位")
                               .arg(results.size()));
}

void MainWindow::updateFrame(int streamId, const QImage &image)
{
    Q_UNUSED(streamId);

    if (image.isNull())
        return;

    // 缩放到 QLabel 大小，保持宽高比
    QPixmap pix = QPixmap::fromImage(image);
    m_videoLabel->setPixmap(pix.scaled(m_videoLabel->size(),
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));

    m_frameCount++;
    m_frameCountLabel->setText(tr("帧数: %1").arg(m_frameCount));
}

void MainWindow::updatePerfStats()
{
    // 每秒计算一次 FPS
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    const int framesThisSecond = m_frameCount - m_lastFrameCount;
    m_lastFrameCount = m_frameCount;
    m_fpsTimer.restart();

    // 数据库待写入数（缓冲队列积压）
    const int pending = m_controller->pendingEventCount();

    m_perfLabel->setText(tr("FPS: %1 | 数据库待写入: %2")
                             .arg(framesThisSecond)
                             .arg(pending));

    // 帧率过低提示（可选：明显掉帧时提示）
    if (elapsedMs > 0 && framesThisSecond < 10 && m_frameCount > 30) {
        m_perfLabel->setStyleSheet("QLabel { color:#e8a33d; font-weight:bold; }");
    } else {
        m_perfLabel->setStyleSheet("QLabel { color:#4ec9b0; font-weight:bold; }");
    }
}

void MainWindow::onEventDetected(int streamId, qint64 timestampMs,
                                 const QString &description, const QString &keyframePath)
{
    m_eventCount++;
    QString time = QDateTime::fromMSecsSinceEpoch(timestampMs)
                       .toString("HH:mm:ss.zzz");
    auto *item = new QListWidgetItem(
        tr("[流%1 %2] %3").arg(streamId).arg(time, description));
    item->setData(Qt::UserRole, keyframePath);      // 存关键帧路径，双击可查看
    item->setData(Qt::UserRole + 1, timestampMs);   // 存事件时间戳（标记/定位用）
    m_eventList->insertItem(0, item);
    m_statusLabel->setText(tr("检测到事件 #%1").arg(m_eventCount));
    m_summaryButton->setEnabled(true);

    // ★ 当前视频运行期间动态添加事件标记（不依赖数据库历史）
    if (m_isFileMode && m_totalMs > 0) {
        QList<qint64> markers;
        for (int i = 0; i < m_eventList->count(); ++i) {
            QListWidgetItem *eventItem = m_eventList->item(i);
            if (!eventItem)
                continue;
            const qint64 ts = eventItem->data(Qt::UserRole + 1).toLongLong();
            if (ts > 0)
                markers << ts;
        }
        std::sort(markers.begin(), markers.end());
        m_progressSlider->setEventMarkers(markers, m_totalMs);
    }

    // ── LLM 自动标注：配置了 API Key 且距上次请求 >30s 时自动摘要 ──
    const QString key = m_apiKeyEdit->text().trimmed();
    const bool keyConfigured = !key.isEmpty()
                               && !key.startsWith(QStringLiteral("«redacted"));
    if (keyConfigured) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastAutoSummaryMs > 30000) {
            m_lastAutoSummaryMs = now;
            m_controller->setApiKey(key);
            m_controller->requestSummary(streamId);
            m_statusLabel->setText(tr("检测到事件 #%1，自动生成 AI 摘要中...")
                                       .arg(m_eventCount));
        }
    }
}

void MainWindow::onEventItemDoubleClicked()
{
    QListWidget *list = qobject_cast<QListWidget *>(sender());
    if (!list || !list->currentItem())
        return;

    // 从列表项读取关键帧路径（UserRole）
    QString keyframe = list->currentItem()->data(Qt::UserRole).toString();
    if (keyframe.isEmpty()) {
        m_statusLabel->setText(tr("该事件无关键帧图片"));
        return;
    }

    QPixmap pix(keyframe);
    if (pix.isNull()) {
        m_statusLabel->setText(tr("关键帧图片无法加载: %1").arg(keyframe));
        return;
    }
    m_videoLabel->setPixmap(pix.scaled(m_videoLabel->size(),
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    m_statusLabel->setText(tr("查看关键帧: %1").arg(keyframe));
}

void MainWindow::onSummaryReady(int streamId, const QString &summary,
                                const QStringList &keywords)
{
    Q_UNUSED(streamId);

    m_summaryText->setPlainText(summary);
    m_keywordsLabel->setText(
        keywords.isEmpty() ? tr("关键词：--")
                           : tr("关键词：%1").arg(keywords.join("  ")));
    m_statusLabel->setText(tr("摘要生成完成"));

    // ★ 填充摘要相关事件列表（可点击定位视频帧）
    m_summaryEventList->clear();
    m_summaryEventPositions.clear();
    const auto rows = m_controller->recentEventsByStream(0, 50);
    for (const auto &row : rows) {
        const QString desc = row.value(QStringLiteral("description")).toString();
        const QString time = QDateTime::fromMSecsSinceEpoch(
            row.value(QStringLiteral("timestamp_ms")).toLongLong())
                                 .toString("HH:mm:ss");
        const QString keyframe = row.value(QStringLiteral("keyframe_path")).toString();
        const qint64 pos = row.value(QStringLiteral("video_position_ms")).toLongLong();
        auto *item = new QListWidgetItem(tr("[%1] %2").arg(time, desc));
        item->setData(Qt::UserRole, keyframe);
        item->setData(Qt::UserRole + 1, pos);   // 视频内位置（跳转用）
        m_summaryEventList->addItem(item);
        if (pos > 0)
            m_summaryEventPositions << pos;
    }
    std::sort(m_summaryEventPositions.begin(), m_summaryEventPositions.end());
    const bool canJump = m_isFileMode && !m_summaryEventPositions.isEmpty();
    m_summaryPrevButton->setEnabled(canJump);
    m_summaryNextButton->setEnabled(canJump);
}

void MainWindow::onError(int streamId, const QString &message)
{
    Q_UNUSED(streamId);
    m_statusLabel->setText(tr("错误: %1").arg(message));
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);

    // 弹窗提示（LLM 请求失败 / 采集失败 / 无法打开文件等）
    QMessageBox::warning(this, tr("VideoAnalyzer - 错误"),
                         message);
}

// ────────────── 检测参数调优（实时生效） ─────────────────────────────

void MainWindow::onAreaThresholdChanged(double value)
{
    m_controller->setAreaThreshold(value);
    m_statusLabel->setText(tr("运动面积阈值 → %1").arg(value));
}

void MainWindow::onCooldownChanged(int value)
{
    m_controller->setCooldownMs(value);
    m_statusLabel->setText(tr("冷却时间 → %1 ms").arg(value));
}

void MainWindow::onDetectIntervalChanged(int value)
{
    m_controller->setDetectInterval(value);
    m_statusLabel->setText(tr("检测间隔 → 每 %1 帧").arg(value));
}

void MainWindow::onWarmupChanged(int value)
{
    m_controller->setWarmupFrames(value);
    m_statusLabel->setText(tr("预热帧数 → %1").arg(value));
}

// ────────────── 输入源切换 ──────────────────────────────────────────

void MainWindow::onSourceChanged(int index)
{
    // 切换输入源时先停止当前流
    m_controller->stopStream(0);
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_progressSlider->setValue(0);
    m_progressSlider->clearEventMarkers();
    m_isPaused = false;
    m_isFileMode = false;
    m_totalMs = 0;
    m_pauseButton->setText(tr("⏸ 暂停"));

    if (index == 0) {
        // USB 摄像头：显示摄像头选择，隐藏文件按钮
        m_cameraSpin->setVisible(true);
        m_fileButton->setVisible(false);
        m_statusLabel->setText(tr("输入源: USB 摄像头"));
        m_videoLabel->setText(tr("等待视频信号...\n点击「启动采集」打开摄像头"));
    } else {
        // 本地视频文件：显示文件按钮，隐藏摄像头选择
        m_cameraSpin->setVisible(false);
        m_fileButton->setVisible(true);
        m_statusLabel->setText(tr("输入源: 本地视频文件（点击「选择视频文件」）"));
        m_videoLabel->setText(tr("等待视频信号...\n点击「选择视频文件」加载本地视频"));
    }
}

// ────────────── 播放器控制 ──────────────────────────────────────────

void MainWindow::onEventsRefreshed()
{
    // 事件源切换（摄像头/新视频）→ 清空旧源的所有 UI 状态
    m_eventList->clear();
    m_eventCount = 0;

    m_summaryButton->setEnabled(false);
    m_summaryText->clear();
    m_keywordsLabel->setText(tr("关键词：--"));

    // 清空进度条事件标记（当前视频新产生的事件会动态添加）
    m_progressSlider->clearEventMarkers();

    // 清空检索结果与定位时间戳
    m_searchResultList->clear();
    m_searchResultVideoPositions.clear();
    m_searchPrevButton->setEnabled(false);
    m_searchNextButton->setEnabled(false);

    // 清空摘要事件列表与定位
    if (m_summaryEventList)
        m_summaryEventList->clear();
    m_summaryEventPositions.clear();
    if (m_summaryPrevButton)
        m_summaryPrevButton->setEnabled(false);
    if (m_summaryNextButton)
        m_summaryNextButton->setEnabled(false);

    m_statusLabel->setText(tr("已切换到新视频源，事件记录已清空"));
}

void MainWindow::onVideoMetadataReady(int streamId, qint64 totalMs)
{
    Q_UNUSED(streamId);
    m_totalMs = totalMs;
    m_progressSlider->setEnabled(true);
    m_progressSlider->setValue(0);

    // ★ 新视频开始时不加载数据库历史事件（避免旧视频事件标记重新出现）
    //   事件标记只添加当前视频运行期间新产生的事件
    m_progressSlider->clearEventMarkers();

    m_statusLabel->setText(tr("视频加载完成，总时长 %1")
                               .arg(formatTime(totalMs)));
}

void MainWindow::onTogglePause()
{
    if (!m_isFileMode) {
        m_statusLabel->setText(tr("仅视频文件模式支持暂停/播放"));
        return;
    }
    m_isPaused = !m_isPaused;
    if (m_controller->setPaused(0, m_isPaused)) {
        m_pauseButton->setText(m_isPaused ? tr("▶ 播放") : tr("⏸ 暂停"));
        m_statusLabel->setText(m_isPaused ? tr("已暂停") : tr("继续播放"));
    }
}

void MainWindow::onSeekBack5s()
{
    if (!m_isFileMode) {
        m_statusLabel->setText(tr("仅视频文件模式支持快进/快退"));
        return;
    }
    const qint64 target = qMax<qint64>(0, m_controller->currentPositionMs(0) - 5000);
    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1").arg(formatTime(target)));
    }
}

void MainWindow::onSeekForward5s()
{
    if (!m_isFileMode) {
        m_statusLabel->setText(tr("仅视频文件模式支持快进/快退"));
        return;
    }
    const qint64 cur = m_controller->currentPositionMs(0);
    const qint64 target = (m_totalMs > 0) ? qMin<qint64>(m_totalMs, cur + 5000)
                                          : cur + 5000;
    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1").arg(formatTime(target)));
    }
}

void MainWindow::onProgressSliderPressed()
{
    m_seeking = true;   // 拖动中暂停自动刷新
}

void MainWindow::onProgressSliderReleased()
{
    // 松开 → 按进度条位置跳转
    const qint64 target = (m_totalMs > 0)
                              ? static_cast<qint64>(m_progressSlider->value())
                                    * m_totalMs / 1000
                              : 0;
    m_seeking = false;
    if (m_controller->seekPosition(0, target)) {
        m_statusLabel->setText(tr("已跳转至 %1").arg(formatTime(target)));
    }
}

void MainWindow::onProgressSliderMoved(int value)
{
    // 拖动时在状态栏实时预览目标时间
    if (m_totalMs > 0) {
        const qint64 target = static_cast<qint64>(value) * m_totalMs / 1000;
        m_statusLabel->setText(tr("定位: %1").arg(formatTime(target)));
    }
}

void MainWindow::updateProgressPosition()
{
    // 播放中每秒从工作线程读取当前位置刷新进度条
    if (m_seeking || !m_isFileMode || !m_progressSlider->isEnabled())
        return;
    const qint64 pos = m_controller->currentPositionMs(0);
    if (pos < 0 || m_totalMs <= 0)
        return;
    m_progressSlider->setValue(static_cast<int>(pos * 1000 / m_totalMs));
}

// ────────────── 检索定位 ────────────────────────────────────────────

void MainWindow::onSearchPrev()
{
    if (m_searchResultVideoPositions.isEmpty() || !m_isFileMode)
        return;
    const qint64 cur = m_controller->currentPositionMs(0);
    // 找最后一个 < 当前进度的视频位置
    qint64 target = -1;
    for (qint64 p : m_searchResultVideoPositions) {
        if (p < cur - 500)   // 小容差避免跳回当前点
            target = p;
        else
            break;
    }
    if (target < 0 && !m_searchResultVideoPositions.isEmpty())
        target = m_searchResultVideoPositions.last();   // 回绕到最后一个

    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1（上一处）").arg(formatTime(target)));
    }
}

void MainWindow::onSearchNext()
{
    if (m_searchResultVideoPositions.isEmpty() || !m_isFileMode)
        return;
    const qint64 cur = m_controller->currentPositionMs(0);
    // 找第一个 > 当前进度的视频位置
    qint64 target = -1;
    for (qint64 p : m_searchResultVideoPositions) {
        if (p > cur + 500) {
            target = p;
            break;
        }
    }
    if (target < 0 && !m_searchResultVideoPositions.isEmpty())
        target = m_searchResultVideoPositions.first();   // 回绕到第一个

    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1（下一处）").arg(formatTime(target)));
    }
}

// ────────────── 检索结果单击 → 选中定位 ────────────────────────────

void MainWindow::onSearchItemClicked(QListWidgetItem *item)
{
    if (!item || !m_isFileMode)
        return;
    // 位置存在 Qt::UserRole+1（onSearchResults 写入）；UserRole 为关键帧路径
    const QVariant posVar = item->data(Qt::UserRole + 1);
    if (!posVar.isValid())
        return;
    const qint64 pos = posVar.toLongLong();
    if (pos <= 0) {
        // 老数据无位置信息：提示但不跳转
        m_statusLabel->setText(tr("该事件无视频位置信息（可能是旧数据）"));
        return;
    }
    if (m_controller->seekPosition(0, pos)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? pos * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已定位至 %1").arg(formatTime(pos)));
    }
}

// ────────────── 摘要事件跳转（Tab2） ───────────────────────────────

void MainWindow::onSummaryEventClicked(QListWidgetItem *item)
{
    if (!item || !m_isFileMode)
        return;
    const QVariant posVar = item->data(Qt::UserRole + 1);
    if (!posVar.isValid())
        return;
    const qint64 pos = posVar.toLongLong();
    if (pos <= 0) {
        m_statusLabel->setText(tr("该事件无视频位置信息（可能是旧数据）"));
        return;
    }
    if (m_controller->seekPosition(0, pos)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? pos * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已定位至 %1").arg(formatTime(pos)));
    }
}

void MainWindow::onSummaryPrev()
{
    if (m_summaryEventPositions.isEmpty() || !m_isFileMode)
        return;
    const qint64 cur = m_controller->currentPositionMs(0);
    qint64 target = -1;
    for (qint64 p : m_summaryEventPositions) {
        if (p < cur - 500)
            target = p;
        else
            break;
    }
    if (target < 0 && !m_summaryEventPositions.isEmpty())
        target = m_summaryEventPositions.last();   // 回绕到最后一个

    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1（上一处）").arg(formatTime(target)));
    }
}

void MainWindow::onSummaryNext()
{
    if (m_summaryEventPositions.isEmpty() || !m_isFileMode)
        return;
    const qint64 cur = m_controller->currentPositionMs(0);
    qint64 target = -1;
    for (qint64 p : m_summaryEventPositions) {
        if (p > cur + 500) {
            target = p;
            break;
        }
    }
    if (target < 0 && !m_summaryEventPositions.isEmpty())
        target = m_summaryEventPositions.first();   // 回绕到第一个

    if (m_controller->seekPosition(0, target)) {
        m_progressSlider->setValue(static_cast<int>(
            m_totalMs > 0 ? target * 1000 / m_totalMs : 0));
        m_statusLabel->setText(tr("已跳转至 %1（下一处）").arg(formatTime(target)));
    }
}

// ────────────── 倍速播放 ─────────────────────────────────────────────

void MainWindow::onSpeedChanged(int index)
{
    const double speed = m_speedCombo->itemData(index).toDouble();
    if (speed <= 0.0)
        return;
    if (m_controller->setPlaybackSpeed(0, speed)) {
        m_statusLabel->setText(tr("播放速度 → %1x").arg(
            QString::number(speed, 'f', 1)));
    }
}

// ────────────── 进度条悬停显示时间 ─────────────────────────────────

void MainWindow::onProgressSliderHovered(qint64 ms)
{
    if (ms < 0) {
        // 鼠标离开 → 恢复默认状态栏（若正在播放）
        if (m_isFileMode)
            m_statusLabel->setText(tr("正在播放: 当前位置 %1")
                                       .arg(formatTime(m_controller->currentPositionMs(0))));
        else
            m_statusLabel->setText(tr("就绪"));
        return;
    }
    m_statusLabel->setText(tr("时间点: %1").arg(formatTime(ms)));
}

QString MainWindow::formatTime(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 totalSec = ms / 1000;
    const int h = static_cast<int>(totalSec / 3600);
    const int m = static_cast<int>((totalSec % 3600) / 60);
    const int s = static_cast<int>(totalSec % 60);
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}
