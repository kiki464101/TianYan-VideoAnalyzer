#ifndef FEATUREEXTRACTOR_H
#define FEATUREEXTRACTOR_H

#include <QObject>
#include <QString>
#include <vector>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>

// 特征提取：人体检测（OpenCV DNN MobileNet-SSD）+ 人脸检测（ResNet-10 SSD）
// + 人物全貌特征（上衣/下装颜色、纹理、服装类型、鞋子颜色）
// "多目标检测"：一帧可检出 N 个人体 + M 个人脸，每人独立特征，多事件触发
// 颜色空间规范：全程 BGR。DNN 输入要求 BGR，画框/标注在 BGR 图上进行。
class FeatureExtractor : public QObject
{
    Q_OBJECT

public:
    // 单人的独立特征集合（人体框优先，否则人脸框）
    struct PersonFeatures
    {
        cv::Rect rect;                // 关联的框（人体优先，否则人脸）
        bool     isFromPerson = true; // true=人体框，false=人脸框
        int      pairedFaceIndex = -1;// 配对的人脸索引（-1=没配到）
        QString  dominantColor;       // 上衣主色描述（如 "红色"、"蓝色"）
        QString  clothingType;        // 服装类型（"上衣"/"裤子"/"裙子"）
        QString  clothingColor;       // 上衣颜色/纹理（如 "蓝色"、"格子"）
        QString  lowerColor;          // 下装颜色（如 "黑色"）
        QString  shoeColor;           // 鞋子颜色（如 "白色"）
    };

    // 单帧分析结果（多目标）
    struct AnalysisResult
    {
        bool     hasPerson = false;   // 是否检测到人体
        bool     hasFace = false;     // 是否检测到人脸
        int      faceCount = 0;       // 检测到的人脸数量
        std::vector<cv::Rect> personRects;   // 所有人体框（原始帧坐标）
        std::vector<cv::Rect> faceRects;     // 所有人脸框（原始帧坐标）
        std::vector<PersonFeatures> persons; // 每人独立特征（人体优先，人脸兜底）
    };

    explicit FeatureExtractor(QObject *parent = nullptr);
    ~FeatureExtractor() override;

    // 加载模型文件（人体 MobileNet-SSD + 人脸 ResNet-10 SSD）
    // 返回是否成功；失败时输出 qWarning 提示下载模型
    bool init(const QString &modelDir);

    // 分析一帧 BGR 图像：人体 + 人脸多目标检测，每人独立特征提取
    // 不修改原图；内部如需颜色分析会将 ROI 转 HSV 计算
    AnalysisResult analyze(const cv::Mat &bgrFrame);

private:
    // 人体检测（MobileNet-SSD）：返回所有人体框（已 NMS）
    std::vector<cv::Rect> detectPerson(const cv::Mat &bgrFrame);
    // 人脸检测（ResNet-10 SSD）：返回所有人脸框（已 NMS）+ 人脸数量
    std::vector<cv::Rect> detectFace(const cv::Mat &bgrFrame, int &faceCount);
    // NMS 去重（OpenCV cv::dnn::NMSBoxes 封装）
    static std::vector<int> nmsBoxes(const std::vector<cv::Rect> &boxes,
                                     const std::vector<float> &scores,
                                     float scoreThreshold, float nmsThreshold);
    // 给人体框配对最近的人脸（人脸在框内/上方时），无则返回空 Rect
    cv::Rect findNearestFace(const cv::Rect &personRect,
                             const std::vector<cv::Rect> &faceRects,
                             int &faceIndex) const;
    // 分析人脸下方区域：上衣（颜色+纹理）、下装（类型+颜色）、鞋子
    // 结果写入 pf（引用参数）
    void analyzeClothing(const cv::Mat &bgrFrame, const cv::Rect &refRect,
                         PersonFeatures &pf);
    // 单一区域主色（HSV 直方图 → 红/蓝/黄/绿/黑/白）
    QString dominantColorOf(const cv::Mat &bgrRoi) const;
    // 纹理判断：对 ROI 做分块颜色差异统计 → 格子/条纹/纯色
    QString textureOf(const cv::Mat &bgrRoi) const;

    cv::dnn::Net m_personNet;         // 人体检测网络（MobileNet-SSD）
    cv::dnn::Net m_faceNet;           // 人脸检测网络（ResNet-10 SSD）
    bool m_personInit = false;        // 人体模型是否加载成功
    bool m_faceInit = false;          // 人脸模型是否加载成功
};

#endif // FEATUREEXTRACTOR_H
