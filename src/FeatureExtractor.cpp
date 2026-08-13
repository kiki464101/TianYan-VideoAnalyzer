#include "FeatureExtractor.h"

#include <QDebug>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

// ──────────────────────────────────────────────────────────────────────
// 特征提取实现：
//   人体检测：OpenCV DNN (MobileNet-SSD, 300x300, 置信度 0.5, 类别 15=人)
//   人脸检测：OpenCV DNN (ResNet-10 SSD, 300x300, 置信度 0.5) —— 兜底
//   全貌分析：人体/人脸框下方区域 → 上衣(颜色+纹理) → 下装(类型+颜色) → 鞋子
// 双重检测策略：人体优先（正面/背面都能检出），人脸兜底（人体漏检时）
// 颜色空间规范：全程 BGR；颜色分析仅将 ROI 转 HSV 计算，不改原图。
// ──────────────────────────────────────────────────────────────────────

FeatureExtractor::FeatureExtractor(QObject *parent)
    : QObject(parent)
{
}

FeatureExtractor::~FeatureExtractor()
{
}

bool FeatureExtractor::init(const QString &modelDir)
{
    bool ok = true;

    // ── 人体检测模型（MobileNet-SSD） ────────────────────────────
    const QString personProto = modelDir + QStringLiteral("MobileNetSSD_deploy.prototxt");
    const QString personModel = modelDir + QStringLiteral("mobilenet_iter_73000.caffemodel");
    if (QFileInfo::exists(personProto) && QFileInfo::exists(personModel)) {
        try {
            m_personNet = cv::dnn::readNetFromCaffe(personProto.toStdString(),
                                                    personModel.toStdString());
            m_personInit = !m_personNet.empty();
        } catch (const cv::Exception &e) {
            qWarning() << "[FeatureExtractor] 人体检测模型加载异常:" << e.what();
            m_personInit = false;
        }
        if (m_personInit)
            qInfo() << "[FeatureExtractor] 人体检测模型加载成功:" << personModel;
        else {
            qWarning() << "[FeatureExtractor] 人体检测模型损坏";
            ok = false;
        }
    } else {
        qWarning() << "[FeatureExtractor] 未找到人体检测模型（MobileNetSSD_deploy.prototxt +"
                   << "mobilenet_iter_73000.caffemodel），人体检测已禁用。"
                   << "请下载到 E:/VideoProject/models/";
        ok = false;
    }

    // ── 人脸检测模型（ResNet-10 SSD，兜底） ──────────────────────
    const QString faceProto = modelDir + QStringLiteral("deploy.prototxt");
    const QString faceModel = modelDir + QStringLiteral("res10_300x300_ssd_iter_140000.caffemodel");
    if (QFileInfo::exists(faceProto) && QFileInfo::exists(faceModel)) {
        try {
            m_faceNet = cv::dnn::readNetFromCaffe(faceProto.toStdString(),
                                                  faceModel.toStdString());
            m_faceInit = !m_faceNet.empty();
        } catch (const cv::Exception &e) {
            qWarning() << "[FeatureExtractor] 人脸检测模型加载异常:" << e.what();
            m_faceInit = false;
        }
        if (m_faceInit)
            qInfo() << "[FeatureExtractor] 人脸检测模型加载成功:" << faceModel;
        else {
            qWarning() << "[FeatureExtractor] 人脸检测模型损坏";
            ok = false;
        }
    } else {
        qWarning() << "[FeatureExtractor] 未找到人脸检测模型（deploy.prototxt +"
                   << "res10_300x300_ssd_iter_140000.caffemodel），人脸检测已禁用。"
                   << "请下载到 E:/VideoProject/models/";
        ok = false;
    }

    return ok;
}

FeatureExtractor::AnalysisResult FeatureExtractor::analyze(const cv::Mat &bgrFrame)
{
    AnalysisResult result;

    if (bgrFrame.empty())
        return result;

    // ── 1) 人体检测（多目标）：正面/背面都能检出，NMS 去重 ──────
    if (m_personInit) {
        result.personRects = detectPerson(bgrFrame);
        result.hasPerson = !result.personRects.empty();
    }

    // ── 2) 人脸检测（多目标，与人体同时检测） ───────────────────
    if (m_faceInit) {
        result.faceRects = detectFace(bgrFrame, result.faceCount);
        result.hasFace = !result.faceRects.empty();
    }

    // ── 3) 每人独立特征：人体框优先，人脸兜底 ───────────────────
    // 给每个人体框配对最近人脸；无人体但有人脸时每人脸一条
    if (!result.personRects.empty()) {
        for (const cv::Rect &personRect : result.personRects) {
            PersonFeatures pf;
            pf.rect = personRect;
            pf.isFromPerson = true;
            int faceIndex = -1;
            cv::Rect pairedFace = findNearestFace(personRect, result.faceRects, faceIndex);
            if (faceIndex >= 0)
                pf.pairedFaceIndex = faceIndex;
            Q_UNUSED(pairedFace);
            analyzeClothing(bgrFrame, personRect, pf);
            result.persons.push_back(pf);
        }
    } else if (!result.faceRects.empty()) {
        for (const cv::Rect &faceRect : result.faceRects) {
            PersonFeatures pf;
            pf.rect = faceRect;
            pf.isFromPerson = false;
            analyzeClothing(bgrFrame, faceRect, pf);
            result.persons.push_back(pf);
        }
    }

    return result;
}

std::vector<cv::Rect> FeatureExtractor::detectPerson(const cv::Mat &bgrFrame)
{
    std::vector<cv::Rect> result;
    try {
        // MobileNet-SSD：300x300 blob
        cv::Mat blob = cv::dnn::blobFromImage(
            bgrFrame, 0.007843, cv::Size(300, 300),
            cv::Scalar(127.5, 127.5, 127.5),   // MobileNet-SSD 均值/缩放
            false, false);                     // 保持 BGR

        m_personNet.setInput(blob);
        cv::Mat detections = m_personNet.forward();

        const int rows = detections.size[2];
        const int cols = detections.size[3];
        const float *data = reinterpret_cast<const float *>(detections.data);

        // MobileNet-SSD COCO 类别：15 = person
        constexpr int CLASS_PERSON = 15;
        constexpr float CONF_THRESHOLD = 0.3f;   // ★ 0.5→0.3：浅色衣物/部分遮挡召回

        std::vector<cv::Rect> boxes;
        std::vector<float> scores;
        for (int i = 0; i < rows; ++i) {
            const int classId = static_cast<int>(data[i * cols + 1]);
            const float confidence = data[i * cols + 2];
            if (classId != CLASS_PERSON || confidence < CONF_THRESHOLD)
                continue;

            const int x1 = static_cast<int>(data[i * cols + 3] * bgrFrame.cols);
            const int y1 = static_cast<int>(data[i * cols + 4] * bgrFrame.rows);
            const int x2 = static_cast<int>(data[i * cols + 5] * bgrFrame.cols);
            const int y2 = static_cast<int>(data[i * cols + 6] * bgrFrame.rows);

            const int bx1 = std::max(0, std::min(x1, bgrFrame.cols - 1));
            const int by1 = std::max(0, std::min(y1, bgrFrame.rows - 1));
            const int bx2 = std::max(0, std::min(x2, bgrFrame.cols - 1));
            const int by2 = std::max(0, std::min(y2, bgrFrame.rows - 1));
            if (bx2 <= bx1 || by2 <= by1)
                continue;

            boxes.push_back(cv::Rect(bx1, by1, bx2 - bx1, by2 - by1));
            scores.push_back(confidence);
        }

        // NMS 去重（同一个人被检成多个框时只保留最高分）
        std::vector<int> kept = nmsBoxes(boxes, scores, CONF_THRESHOLD, 0.4f);
        for (int idx : kept)
            result.push_back(boxes[idx]);

        return result;
    } catch (const cv::Exception &e) {
        qWarning() << "[FeatureExtractor] 人体检测异常:" << e.what();
        return result;
    }
}

std::vector<cv::Rect> FeatureExtractor::detectFace(const cv::Mat &bgrFrame, int &faceCount)
{
    std::vector<cv::Rect> result;
    faceCount = 0;
    try {
        // ResNet-10 SSD：300x300 blob
        cv::Mat blob = cv::dnn::blobFromImage(
            bgrFrame, 1.0, cv::Size(300, 300),
            cv::Scalar(104.0, 177.0, 123.0),   // ResNet-10 SSD 均值
            false, false);                     // 保持 BGR

        m_faceNet.setInput(blob);
        cv::Mat detections = m_faceNet.forward();

        const int rows = detections.size[2];
        const int cols = detections.size[3];
        const float *data = reinterpret_cast<const float *>(detections.data);

        constexpr float CONF_THRESHOLD = 0.4f;   // ★ 0.5→0.4：人脸召回（人体已优先）

        std::vector<cv::Rect> boxes;
        std::vector<float> scores;
        for (int i = 0; i < rows; ++i) {
            const float confidence = data[i * cols + 2];
            if (confidence < CONF_THRESHOLD)
                continue;

            const int x1 = static_cast<int>(data[i * cols + 3] * bgrFrame.cols);
            const int y1 = static_cast<int>(data[i * cols + 4] * bgrFrame.rows);
            const int x2 = static_cast<int>(data[i * cols + 5] * bgrFrame.cols);
            const int y2 = static_cast<int>(data[i * cols + 6] * bgrFrame.rows);

            const int bx1 = std::max(0, std::min(x1, bgrFrame.cols - 1));
            const int by1 = std::max(0, std::min(y1, bgrFrame.rows - 1));
            const int bx2 = std::max(0, std::min(x2, bgrFrame.cols - 1));
            const int by2 = std::max(0, std::min(y2, bgrFrame.rows - 1));
            if (bx2 <= bx1 || by2 <= by1)
                continue;

            boxes.push_back(cv::Rect(bx1, by1, bx2 - bx1, by2 - by1));
            scores.push_back(confidence);
        }

        // NMS 去重
        std::vector<int> kept = nmsBoxes(boxes, scores, CONF_THRESHOLD, 0.4f);
        for (int idx : kept)
            result.push_back(boxes[idx]);

        faceCount = static_cast<int>(result.size());
        return result;
    } catch (const cv::Exception &e) {
        qWarning() << "[FeatureExtractor] 人脸检测异常:" << e.what();
        return result;
    }
}

std::vector<int> FeatureExtractor::nmsBoxes(const std::vector<cv::Rect> &boxes,
                                            const std::vector<float> &scores,
                                            float scoreThreshold, float nmsThreshold)
{
    std::vector<int> kept;
    if (boxes.empty() || boxes.size() != scores.size())
        return kept;

    try {
        cv::dnn::NMSBoxes(boxes, scores, scoreThreshold, nmsThreshold, kept, 100);
    } catch (const cv::Exception &e) {
        qWarning() << "[FeatureExtractor] NMS 异常:" << e.what();
    }
    return kept;
}

cv::Rect FeatureExtractor::findNearestFace(const cv::Rect &personRect,
                                           const std::vector<cv::Rect> &faceRects,
                                           int &faceIndex) const
{
    faceIndex = -1;
    if (faceRects.empty())
        return cv::Rect();

    // 人脸必须与人体框有重叠，或位于人体框正上方（人头位置）
    const cv::Point personCenter(personRect.x + personRect.width / 2,
                                 personRect.y + personRect.height / 2);
    int bestDist = INT_MAX;
    cv::Rect bestRect;
    for (size_t i = 0; i < faceRects.size(); ++i) {
        const cv::Rect &face = faceRects[i];
        cv::Rect inter = face & personRect;
        const bool overlaps = inter.width > 0 && inter.height > 0;
        const bool above = (face.y + face.height > personRect.y)
                           && (face.y < personRect.y + personRect.height);
        if (!overlaps && !above)
            continue;

        const cv::Point faceCenter(face.x + face.width / 2,
                                   face.y + face.height / 2);
        const int dx = personCenter.x - faceCenter.x;
        const int dy = personCenter.y - faceCenter.y;
        const int dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            bestRect = face;
            faceIndex = static_cast<int>(i);
        }
    }
    return bestRect;
}

void FeatureExtractor::analyzeClothing(const cv::Mat &bgrFrame, const cv::Rect &refRect,
                                       PersonFeatures &pf)
{
    // 基准：人体框时用框上部 30%（头部区域）；人脸框时直接用
    // 颜色分析锚点：身体区域从锚点下方开始
    //   人体框: 头约在上 15-30%，上衣在下 30-75%，下装在下 75-92%
    //   人脸框: 与之前逻辑一致（人脸下方 1.5 倍高）
    const bool isPerson = pf.isFromPerson;
    const int refY = refRect.y;
    const int refH = refRect.height;
    const int refW = refRect.width;
    const int refX = refRect.x;

    // 身体区域水平范围（比参考框略宽，容纳肩膀）
    const int bodyW = isPerson ? refW : refW * 2;
    const int bodyX = isPerson ? refX : refX + refW / 2 - bodyW / 2;
    const int bx1 = std::max(0, bodyX);
    const int bx2 = std::min(bgrFrame.cols, bodyX + bodyW);

    // 区域划分（相对参考框顶部）
    int coatY1, coatY2, lowerY1, lowerY2, shoeY1, shoeY2;
    if (isPerson) {
        // 人体框：头 ~15-30%，上衣 30-75%，下装 75-92%，鞋子 92-100%
        coatY1 = refY + static_cast<int>(refH * 0.30);
        coatY2 = refY + static_cast<int>(refH * 0.75);
        lowerY1 = refY + static_cast<int>(refH * 0.75);
        lowerY2 = refY + static_cast<int>(refH * 0.92);
        shoeY1 = refY + static_cast<int>(refH * 0.92);
        shoeY2 = refY + refH;
    } else {
        // 人脸框：与之前一致（人脸下方 1.5 倍高 → 下装 → 鞋子）
        const int faceBottom = refY + refH;
        const int coatH = static_cast<int>(refH * 1.5);
        coatY1 = faceBottom;
        coatY2 = std::min(bgrFrame.rows, faceBottom + coatH);
        lowerY1 = coatY2;
        lowerY2 = std::min(bgrFrame.rows, lowerY1 + static_cast<int>(refH * 1.5));
        shoeY1 = lowerY2;
        shoeY2 = std::min(bgrFrame.rows, shoeY1 + static_cast<int>(refH * 0.5));
    }

    // ── 上衣：主色 + 纹理 ─────────────────────────────────────
    if (coatY2 > coatY1 && bx2 > bx1 && coatY1 < bgrFrame.rows) {
        const int c1 = std::max(0, coatY1);
        const int c2 = std::min(bgrFrame.rows, coatY2);
        if (c2 > c1) {
            cv::Mat coatRoi = bgrFrame(cv::Rect(bx1, c1, bx2 - bx1, c2 - c1));
            pf.dominantColor = dominantColorOf(coatRoi);
            const QString texture = textureOf(coatRoi);
            if (texture.isEmpty())
                pf.clothingColor = pf.dominantColor;
            else
                pf.clothingColor = pf.dominantColor.isEmpty()
                                       ? texture
                                       : pf.dominantColor + texture;
        }
    }

    // ── 下装：类型（裤子/裙子）+ 颜色 ─────────────────────────
    //   颜色区 0.75-0.92×refH（防鞋色污染）；
    //   判别区扩到 0.75-1.0×refH（含脚踝，腿缝最宽处）
    if (lowerY2 > lowerY1 && bx2 > bx1 && lowerY1 < bgrFrame.rows) {
        const int l1 = std::max(0, lowerY1);
        const int l2 = std::min(bgrFrame.rows, lowerY2);
        if (l2 > l1) {
            cv::Mat lowerRoi = bgrFrame(cv::Rect(bx1, l1, bx2 - bx1, l2 - l1));
            pf.lowerColor = dominantColorOf(lowerRoi);

            // ★ 裤裙判别（腿缝列投影法，替代宽高比——宽高比恒>1.6 永远判裙）：
            //   判别区 = 颜色区底部延伸至人体框底部（含脚踝，腿缝最明显）
            //   画面截断（判别 ROI 高度 < refH*0.05）→ 跳过下装，不写裤/裙
            const int g1 = l1;
            const int g2 = std::min(bgrFrame.rows, refY + refH);
            if (g2 - g1 >= static_cast<int>(refH * 0.05) && bx2 > bx1) {
                cv::Mat gray;
                cv::Mat lowerDisc = bgrFrame(cv::Rect(bx1, g1, bx2 - bx1, g2 - g1));
                cv::cvtColor(lowerDisc, gray, cv::COLOR_BGR2GRAY);

                const double gap = legGapRatio(gray);    // 主判：腿缝比
                const double flare = flareRatio(gray);   // 辅判：下摆外扩
                if (gap < 0.6)
                    pf.clothingType = QStringLiteral("裤子");
                else if (gap > 0.8)
                    pf.clothingType = QStringLiteral("裙子");
                else
                    pf.clothingType = (flare > 1.25) ? QStringLiteral("裙子")
                                                     : QStringLiteral("裤子");
            }
            // 截断跳过：clothingType 保持空 → 描述自动不出现下装段
        }
    }

    // ── 鞋子：颜色 ────────────────────────────────────────────
    if (shoeY2 > shoeY1 && bx2 > bx1 && shoeY1 < bgrFrame.rows) {
        const int s1 = std::max(0, shoeY1);
        const int s2 = std::min(bgrFrame.rows, shoeY2);
        if (s2 > s1) {
            cv::Mat shoeRoi = bgrFrame(cv::Rect(bx1, s1, bx2 - bx1, s2 - s1));
            pf.shoeColor = dominantColorOf(shoeRoi);
        }
    }
}

QString FeatureExtractor::dominantColorOf(const cv::Mat &bgrRoi) const
{
    if (bgrRoi.empty())
        return QString();

    cv::Mat hsv;
    cv::cvtColor(bgrRoi, hsv, cv::COLOR_BGR2HSV);

    int countRed = 0, countBlue = 0, countYellow = 0, countGreen = 0;
    int countBlack = 0, countWhite = 0;
    constexpr int kSaturationMin = 60;
    constexpr int kValueMin = 60;

    for (int y = 0; y < hsv.rows; ++y) {
        const cv::Vec3b *row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x) {
            const int h = row[x][0];
            const int s = row[x][1];
            const int v = row[x][2];

            if (s < kSaturationMin || v < kValueMin) {
                if (v < 100)
                    countBlack++;
                else if (v > 200)
                    countWhite++;
                continue;
            }

            if (h <= 10 || h >= 170)
                countRed++;
            else if (h >= 20 && h <= 35)
                countYellow++;
            else if (h > 35 && h <= 85)
                countGreen++;
            else if (h > 90 && h <= 130)
                countBlue++;
        }
    }

    struct ColorCount { QString name; int count; };
    const ColorCount counts[] = {
        { QStringLiteral("红色"), countRed },
        { QStringLiteral("蓝色"), countBlue },
        { QStringLiteral("黄色"), countYellow },
        { QStringLiteral("绿色"), countGreen },
        { QStringLiteral("黑色"), countBlack },
        { QStringLiteral("白色"), countWhite },
    };

    const ColorCount *best = &counts[0];
    for (const auto &c : counts) {
        if (c.count > best->count)
            best = &c;
    }
    return best->count > 0 ? best->name : QString();
}

QString FeatureExtractor::textureOf(const cv::Mat &bgrRoi) const
{
    // ★ G1+G2: Gabor 滤波器组纹理判断（方案 3）
    //   4 方向(0°/45°/90°/135°) × 3 尺度(细/中/粗) = 12 次滤波
    //   响应强度高 → 纹理；方向分布判定 条纹/格子
    //   小 ROI(<50×50) 降级为纯颜色（省 CPU，小区域纹理意义不大）
    if (bgrRoi.cols < 50 || bgrRoi.rows < 50)
        return QString();

    cv::Mat gray;
    cv::cvtColor(bgrRoi, gray, cv::COLOR_BGR2GRAY);
    // 形态学去噪：先开运算，抑制玻璃反光/字幕边缘等噪声干扰
    cv::Mat denoised;
    cv::morphologyEx(gray, denoised, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    // Gabor 滤波器组：4 方向 × 3 尺度
    constexpr double kSigmas[] = {2.0, 4.0, 8.0};        // 细/中/粗条纹尺度
    constexpr double kThetas[] = {0.0, CV_PI / 4, CV_PI / 2, 3 * CV_PI / 4};
    constexpr double kResponseThreshold = 0.3;   // 响应强度阈值（抑制噪声误判）

    // 各方向最大响应（跨尺度取最大值）
    double maxResp[4] = {0, 0, 0, 0};
    for (int t = 0; t < 4; ++t) {
        for (int s = 0; s < 3; ++s) {
            const cv::Size ksize(31, 31);
            cv::Mat kernel = cv::getGaborKernel(
                ksize, kSigmas[s], kThetas[t], kSigmas[s] * 2.5, 0.5, 0, CV_32F);
            cv::Mat filtered;
            cv::filter2D(denoised, filtered, CV_32F, kernel);
            // ★ 用响应图标准差做强度（纯色图滤波后均匀 → std 小，
            //   避免 minMaxLoc 对边界伪影的高敏感误判）
            cv::Scalar m, sdev;
            cv::meanStdDev(filtered, m, sdev);
            const double resp = sdev[0];
            if (resp > maxResp[t])
                maxResp[t] = resp;
        }
    }

    // 归一化：响应均值（纯色图 std 全低 → 均值低 → 无纹理）
    double meanResp = 0.0;
    for (int t = 0; t < 4; ++t)
        meanResp += maxResp[t];
    meanResp /= 4.0;
    if (meanResp < kResponseThreshold)
        return QString();

    // 正交对对比：横竖对(0°+90°) vs 对角对(45°+135°)
    const double ortho1 = (maxResp[0] + maxResp[2]) / 2.0;   // 横竖
    const double ortho2 = (maxResp[1] + maxResp[3]) / 2.0;   // 对角
    const double pairMax = std::max(ortho1, ortho2);
    const double pairMin = std::min(ortho1, ortho2);
    const double pairRatio = pairMax > 0 ? pairMin / pairMax : 0.0;

    // ★ 两正交对的最小/最大比判纹理：
    //   格子：两对都强（ratio ≥ 0.35）——横竖+对角交叉
    //   条纹：单对主导（ratio < 0.35）
    if (pairRatio >= 0.35)
        return QStringLiteral("格子");
    return QStringLiteral("条纹");
}

double FeatureExtractor::rectIou(const cv::Rect &a, const cv::Rect &b)
{
    const cv::Rect inter = a & b;
    const double interArea = static_cast<double>(inter.width) * inter.height;
    const double areaA = static_cast<double>(a.width) * a.height;
    const double areaB = static_cast<double>(b.width) * b.height;
    const double unionArea = areaA + areaB - interArea;
    if (unionArea <= 0.0)
        return 0.0;
    return interArea / unionArea;
}

// ─────────────────────────────────────────────────────────────
// ★ 裤裙判别（腿缝列投影法，替代宽高比——宽高比恒>1.6 永远判裙）
//   核心思想：裤子两条腿之间能看到背景缝（中间布料量少），
//   裙子布料连续覆盖（中间布料量 ≈ 两侧）。
// ─────────────────────────────────────────────────────────────
double FeatureExtractor::legGapRatio(const cv::Mat &grayLower)
{
    if (grayLower.cols < 40 || grayLower.rows < 20)
        return 1.0;   // ROI 太小，无法判别 → 返回"无腿缝"（保守判裙由调用方处理）

    // 布料像素 = 与 ROI 主灰度（直方图峰值）接近的像素（自适应背景）
    cv::Mat roi;
    cv::medianBlur(grayLower, roi, 5);   // 中值滤波去噪（输出到局部变量）
    // 直方图峰值 = 布料主灰度（面积最大的灰度值）
    const int bins = 256;
    float range[] = {0.0f, 256.0f};
    const float *ranges[] = {range};
    cv::Mat hist;
    cv::calcHist(&roi, 1, 0, cv::Mat(), hist, 1, &bins, ranges);
    double maxVal = 0;
    double minVal = 0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(hist, &minVal, &maxVal, &minLoc, &maxLoc);
    // ★ hist 是 256×1 竖向量：bin 索引在 maxLoc.y（列=0）
    const double clothGray = static_cast<double>(maxLoc.y);
    cv::Mat cloth;
    cv::Mat diff;
    cv::absdiff(roi, cv::Scalar(clothGray), diff);
    cv::threshold(diff, cloth, 40, 255, cv::THRESH_BINARY_INV);   // 差<40 = 布料

    // 只看底部 1/3（小腿/脚踝段，腿缝最宽最明显）
    const int h = cloth.rows;
    cv::Mat bot = cloth(cv::Rect(0, h * 2 / 3, cloth.cols, h - h * 2 / 3));

    // 列投影：每列布料像素数
    std::vector<int> colCount(bot.cols, 0);
    for (int y = 0; y < bot.rows; ++y) {
        const uchar *row = bot.ptr<uchar>(y);
        for (int x = 0; x < bot.cols; ++x)
            if (row[x] > 0)
                colCount[x]++;
    }

    // 中间 10% 竖条 vs 两侧腿内竖条（各 ~15%，避开最外侧背景）
    const int w = bot.cols;
    const int midL = static_cast<int>(w * 0.45);
    const int midR = static_cast<int>(w * 0.55);
    const int sideL = static_cast<int>(w * 0.15);
    const int sideR = static_cast<int>(w * 0.30);
    const int sideL2 = static_cast<int>(w * 0.70);
    const int sideR2 = static_cast<int>(w * 0.85);

    double midSum = 0, sideSum = 0;
    int midN = 0, sideN = 0;
    for (int x = midL; x < midR; ++x) { midSum += colCount[x]; midN++; }
    for (int x = sideL; x < sideR; ++x) { sideSum += colCount[x]; sideN++; }
    for (int x = sideL2; x < sideR2; ++x) { sideSum += colCount[x]; sideN++; }

    if (midN <= 0 || sideN <= 0)
        return 1.0;
    const double midAvg = midSum / midN;
    const double sideAvg = sideSum / sideN;
    if (sideAvg <= 1e-6)
        return 1.0;
    return midAvg / sideAvg;
}

double FeatureExtractor::flareRatio(const cv::Mat &grayLower)
{
    if (grayLower.cols < 40 || grayLower.rows < 20)
        return 1.0;

    cv::Mat roi = grayLower.clone();
    // 直方图峰值 = 布料主灰度
    const int bins = 256;
    float range[] = {0.0f, 256.0f};
    const float *ranges[] = {range};
    cv::Mat hist;
    cv::calcHist(&roi, 1, 0, cv::Mat(), hist, 1, &bins, ranges);
    double maxVal = 0;
    double minVal = 0;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(hist, &minVal, &maxVal, &minLoc, &maxLoc);
    cv::Mat diff;
    cv::absdiff(roi, cv::Scalar(maxLoc.y), diff);
    cv::Mat cloth;
    cv::threshold(diff, cloth, 40, 255, cv::THRESH_BINARY_INV);

    // 底部1/4 与 顶部1/4 的布料水平跨度（每行最左~最右布料距离，取平均）
    auto rowSpan = [&cloth](int y1, int y2) {
        double sum = 0; int n = 0;
        for (int y = y1; y < y2; ++y) {
            const uchar *row = cloth.ptr<uchar>(y);
            int first = -1, last = -1;
            for (int x = 0; x < cloth.cols; ++x)
                if (row[x] > 0) { if (first < 0) first = x; last = x; }
            if (first >= 0 && last >= first) { sum += (last - first); n++; }
        }
        return n > 0 ? sum / n : 0.0;
    };

    const int h = cloth.rows;
    const double topSpan = rowSpan(0, h / 4);
    const double botSpan = rowSpan(h * 3 / 4, h);
    if (topSpan <= 1e-6)
        return 1.0;
    return botSpan / topSpan;
}
