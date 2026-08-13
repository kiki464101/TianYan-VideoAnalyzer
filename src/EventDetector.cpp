#include "EventDetector.h"

#include <QDateTime>
#include <QDebug>
#include <algorithm>

// ──────────────────────────────────────────────────────────────────────
// 事件检测实现：
//   MOG2 背景减除 → 形态学去噪 → 运动面积占比 → 阈值过滤 → 冷却抑制
// ──────────────────────────────────────────────────────────────────────

EventDetector::EventDetector(QObject *parent)
    : QObject(parent)
{
    // MOG2：history=500, varThreshold=16, detectShadows=false（省算力、避免阴影误报）
    m_bgSubtractor = cv::createBackgroundSubtractorMOG2(500, 16, false);
}

EventDetector::~EventDetector()
{
}

void EventDetector::reset()
{
    // 重建背景模型（等价于重置 MOG2 的学习状态）
    m_bgSubtractor = cv::createBackgroundSubtractorMOG2(500, 16, false);
    m_frameCount = 0;
    m_lastTriggerByStream.clear();
    // 清空检测框缓存（新视频/跳转后不再显示旧框）
    m_lastPersonRects.clear();
    m_lastFaceRects.clear();
    m_lastHasPerson = false;
    m_lastHasFace = false;
    m_lastDetectFrame = 0;
}

cv::Mat EventDetector::annotateFrame(const cv::Mat &frame)
{
    cv::Mat output = frame.clone();   // 深拷贝（BGR，不修改原图）
    if (output.empty())
        return output;

    // 所有人体蓝色框（BGR: 255,0,0）
    if (m_lastHasPerson) {
        for (const cv::Rect &r : m_lastPersonRects) {
            if (r.width <= 0 || r.height <= 0)
                continue;
            cv::rectangle(output, r, cv::Scalar(255, 0, 0), 2);
            cv::putText(output, "Person",
                        cv::Point(r.x, std::max(20, r.y - 6)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 0, 0), 2);
        }
    }
    // 所有人脸绿色框（BGR: 0,255,0）
    if (m_lastHasFace) {
        for (const cv::Rect &r : m_lastFaceRects) {
            if (r.width <= 0 || r.height <= 0)
                continue;
            cv::rectangle(output, r, cv::Scalar(0, 255, 0), 2);
            cv::putText(output, "Face",
                        cv::Point(r.x, std::max(20, r.y - 6)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
        }
    }
    return output;
}

void EventDetector::setAreaThreshold(double ratio)
{
    m_areaThreshold = ratio;
}

void EventDetector::setCooldownMs(int ms)
{
    m_cooldownMs = ms;
}

void EventDetector::setDetectInterval(int frames)
{
    m_detectInterval = frames;
}

void EventDetector::setWarmupFrames(int frames)
{
    m_warmupFrames = frames;
}

bool EventDetector::initFeatureExtractor(const QString &modelPath)
{
    m_featureExtractor = std::make_unique<FeatureExtractor>(this);
    const bool ok = m_featureExtractor->init(modelPath);
    if (!ok)
        qWarning() << "[EventDetector] 特征提取不可用（模型缺失），"
                   << "人脸检测/颜色分析已降级跳过，运动检测不受影响";
    return ok;
}

bool EventDetector::processFrame(int streamId, const cv::Mat &frame)
{
    if (frame.empty() || !m_bgSubtractor)
        return false;

    m_frameCount++;

    // 1) 抽帧：每 N 帧检测一次，降低 CPU 负载
    if (m_frameCount % m_detectInterval != 0)
        return false;

    // 2) 背景建模（预热期：只建背景，不判定）
    cv::Mat fgMask;
    m_bgSubtractor->apply(frame, fgMask, m_frameCount <= m_warmupFrames ? 0.01 : 0.001);
    if (m_frameCount <= m_warmupFrames)
        return false;

    // 3) 形态学去噪：开运算去掉孤立噪点，闭运算填充空洞
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::morphologyEx(fgMask, fgMask, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(fgMask, fgMask, cv::MORPH_CLOSE, kernel);

    // ── 4.5) DNN 检测与运动解耦（★ 核心修复）────────────────────
    //   无论画面有无运动，每 detectInterval 帧都跑人体+人脸检测，
    //   更新缓存框 → annotateFrame 每帧用最新缓存画框。
    //   这样"人站着不动也有框"，且框实时跟随，无延迟。
    FeatureExtractor::AnalysisResult feat;
    if (m_featureExtractor)
        feat = m_featureExtractor->analyze(frame);

    // 更新检测框缓存（供 annotateFrame 在播放画面上实时画框）
    m_lastHasPerson = feat.hasPerson;
    m_lastPersonRects = feat.personRects;
    m_lastHasFace = feat.hasFace;
    m_lastFaceRects = feat.faceRects;
    m_lastDetectFrame = m_frameCount;

    // 4) 轮廓分析：寻找外部轮廓，过滤小面积噪点
    //    不用 countNonZero 全图统计——远处树叶晃动等小目标会被
    //    外接矩形面积阈值（MIN_CONTOUR_AREA）过滤掉，降低误报
    constexpr int MIN_CONTOUR_AREA = 2000;   // 最小有效轮廓面积（平方像素），过滤更多噪点
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(fgMask, contours, hierarchy,
                     cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    int motionPixels = 0;
    cv::Rect mergedBox;
    bool hasMotion = false;
    for (const auto &contour : contours) {
        // 外接矩形面积 = 宽 × 高（近似目标大小）
        const cv::Rect box = cv::boundingRect(contour);
        const int area = box.width * box.height;
        if (area < MIN_CONTOUR_AREA)
            continue;   // 过滤小噪点（树叶晃动、光线闪烁）

        motionPixels += area;   // 累加有效轮廓面积
        mergedBox = hasMotion ? (mergedBox | box) : box;   // 合并包围盒
        hasMotion = true;
    }

    // 无有效轮廓 → 非事件
    if (!hasMotion)
        return false;

    // 5) 面积占比（用有效轮廓总面积 / 画面总面积）
    int totalPixels = fgMask.cols * fgMask.rows;
    double ratio = (totalPixels > 0)
                       ? static_cast<double>(motionPixels) / totalPixels
                       : 0.0;

    // 6) 阈值过滤：运动面积占比不足，忽略
    if (ratio < m_areaThreshold)
        return false;

    // 7) 冷却时间：同一路视频源在冷却窗口内不重复触发
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = m_lastTriggerByStream.find(streamId);
    if (it != m_lastTriggerByStream.end() && (now - it.value()) < m_cooldownMs)
        return false;
    m_lastTriggerByStream[streamId] = now;

    // 8) 生成事件记录（特征已在 4.5 步检测，此处直接使用）
    EventRecord rec;
    rec.streamId = streamId;
    rec.timestampMs = now;
    rec.motionRatio = ratio;
    rec.keyFrame = frame.clone();   // 深拷贝关键帧（BGR，后续直接绘制）
    rec.boundingBox = mergedBox;

    // 8b) ★ 多目标事件触发：无人 → 物体移动事件；有人 → N 条独立事件
    if (!feat.hasPerson && !feat.hasFace) {
        // 物体移动事件：仅更新 UI 信息流，不截图不入库
        rec.description = QStringLiteral("[流%1] 检测到物体移动，未识别人物")
                              .arg(streamId);
        rec.saveKeyFrame = false;
        rec.personIndex = 0;
        emit eventTriggered(rec);
        return true;
    }

    // 8c) ★ 多人画面：准备共享关键帧（画上所有人的框 + 编号 P1/P2/...）
    //     keyFrame 仅第一条携带，后续事件通过 keyframePath 共享同一张图
    const qint64 sharedTs = now;
    cv::Mat sharedFrame = frame.clone();   // BGR 深拷贝
    for (size_t i = 0; i < feat.persons.size(); ++i) {
        const auto &pf = feat.persons[i];
        const QString label = QStringLiteral("P%1").arg(i + 1);
        cv::rectangle(sharedFrame, pf.rect, cv::Scalar(255, 0, 0), 2);
        cv::putText(sharedFrame, label.toStdString(),
                    cv::Point(pf.rect.x, std::max(20, pf.rect.y - 6)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 0, 0), 2);
    }
    for (const cv::Rect &faceRect : feat.faceRects) {
        cv::rectangle(sharedFrame, faceRect, cv::Scalar(0, 255, 0), 2);
        cv::putText(sharedFrame, "Face",
                    cv::Point(faceRect.x, std::max(20, faceRect.y - 6)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
    }

    // 8d) 每人触发一条独立事件（N 条入库，共享同一张关键帧）
    for (size_t i = 0; i < feat.persons.size(); ++i) {
        const auto &pf = feat.persons[i];
        EventRecord one = rec;   // 复用 streamId/ts/ratio/boundingBox
        one.personIndex = static_cast<int>(i + 1);
        one.keyFrame = (i == 0) ? sharedFrame.clone() : cv::Mat();  // 仅第一条带图
        one.saveKeyFrame = true;

        const int cx = pf.rect.x + pf.rect.width / 2;
        const int cy = pf.rect.y + pf.rect.height / 2;
        const QString tag = pf.isFromPerson ? QStringLiteral("人体") : QStringLiteral("人脸");

        // 组合特征：身穿<上衣颜色>，下装<类型><颜色>，脚穿<鞋子颜色>
        QStringList features;
        if (!pf.clothingColor.isEmpty())
            features << QStringLiteral("身穿%1上衣").arg(pf.clothingColor);
        if (!pf.clothingType.isEmpty() && !pf.lowerColor.isEmpty())
            features << QStringLiteral("%1为%2").arg(pf.clothingType, pf.lowerColor);
        if (!pf.shoeColor.isEmpty())
            features << QStringLiteral("脚穿%1鞋子").arg(pf.shoeColor);

        one.description = QStringLiteral("[流%1] 检测到人物(%2)，P%3 位置(%4,%5)")
                              .arg(streamId).arg(tag).arg(i + 1).arg(cx).arg(cy);
        if (!features.isEmpty())
            one.description += QStringLiteral("，特征：%1").arg(features.join(QStringLiteral("，")));
        // ★ 特征为空时不补"未能识别着装"

        emit eventTriggered(one);
    }

    return true;
}
