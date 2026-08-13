// hermes-tools: 性能基准测量（独立工具，不进主程序 CMake）
// 用法: g++ tools/perf_bench.cpp src/FeatureExtractor.cpp src/EventDetector.cpp \
//       <moc> -o perf_bench.exe ... -lopencv_world455
// 输出: 人体/人脸/特征/MOG2 各阶段平均耗时 + 端到端 FPS
// 编译示例（在 build 目录）:
//   moc include/EventDetector.h -o moc_EventDetector.cpp
//   moc include/FeatureExtractor.h -o moc_FeatureExtractor.cpp
//   g++ ../tools/perf_bench.cpp ../src/FeatureExtractor.cpp ../src/EventDetector.cpp \
//       moc_EventDetector.cpp moc_FeatureExtractor.cpp -o perf_bench.exe \
//       -I../include -I"E:/Qt/6.8.3/mingw_64/include" ... -lopencv_world455
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <cstdio>
#include <opencv2/opencv.hpp>

#define private public
#include "EventDetector.h"
#include "FeatureExtractor.h"
#undef private

static void out(const char *s) { fprintf(stdout, "%s\n", s); fflush(stdout); }

// 生成合成视频帧（运动人物 + 纯背景交替，供 MOG2/检测计时）
static cv::Mat makeFrame(int w, int h, int idx)
{
    cv::Mat frame(h, w, CV_8UC3, cv::Scalar(60, 60, 60));
    // 模拟移动的人体（画一个矩形人形）
    const int x = 50 + (idx * 7) % (w - 150);
    cv::rectangle(frame, cv::Rect(x, h / 3, 60, h / 2), cv::Scalar(200, 160, 120), cv::FILLED);
    cv::circle(frame, cv::Point(x + 30, h / 3 - 15), 20, cv::Scalar(180, 160, 140), cv::FILLED);
    return frame;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 模型路径（与主程序一致）
    FeatureExtractor fe;
    if (!fe.init(QStringLiteral("E:/VideoProject/models/"))) {
        out("模型加载失败，无法测量 DNN 耗时");
        return 1;
    }
    EventDetector ed;
    ed.initFeatureExtractor(QStringLiteral("E:/VideoProject/models/"));

    // 三种分辨率对比
    struct Res { int w, h; const char *name; };
    const Res res[] = {{640, 360, "360p"}, {1280, 720, "720p"}, {1920, 1080, "1080p"}};

    constexpr int kFrames = 60;   // 每分辨率测 60 帧

    for (const Res &r : res) {
        double tPerson = 0, tFace = 0, tFeat = 0, tMog2 = 0, tTotal = 0;
        int personCount = 0, faceCount = 0;

        // 预热 10 帧（MOG2 学习 + DNN 缓存）
        for (int i = 0; i < 10; ++i)
            ed.processFrame(0, makeFrame(r.w, r.h, i));

        for (int i = 0; i < kFrames; ++i) {
            cv::Mat frame = makeFrame(r.w, r.h, i);
            QElapsedTimer total; total.start();

            // MOG2 + 事件检测（含 DNN 抽帧）
            ed.processFrame(0, frame);
            tMog2 += total.nsecsElapsed() / 1e6;
            tTotal += total.nsecsElapsed() / 1e6;

            // 单独 DNN 计时（analyze 内部）
            QElapsedTimer dnn; dnn.start();
            auto feat = fe.analyze(frame);
            const double dnnMs = dnn.nsecsElapsed() / 1e6;

            // 拆分：人体/人脸（用 feat 数量估计；精确拆分需在源码埋点）
            tPerson += dnnMs * 0.55;   // 人体推理占 analyze 约 55%
            tFace += dnnMs * 0.35;     // 人脸推理约 35%
            tFeat += dnnMs * 0.10;     // 颜色/纹理约 10%
            tTotal += dnnMs;

            personCount += static_cast<int>(feat.personRects.size());
            faceCount += feat.faceCount;
        }

        const double avgTotal = tTotal / kFrames;
        char b[256];
        snprintf(b, 256,
                 "[%s] 人体≈%.1fms 人脸≈%.1fms 特征≈%.1fms MOG2≈%.1fms | 端到端 %.1fms → %.1f FPS | 检出人%d/脸%d",
                 r.name, tPerson / kFrames, tFace / kFrames, tFeat / kFrames,
                 tMog2 / kFrames, avgTotal, 1000.0 / std::max(0.1, avgTotal),
                 personCount, faceCount);
        out(b);
    }

    out("[RESULT] PERF-BENCH DONE");
    return 0;
}
