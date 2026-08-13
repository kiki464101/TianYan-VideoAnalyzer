#include "LLMClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QRegularExpression>
#include <QDebug>

// ──────────────────────────────────────────────────────────────────────
// LLM 客户端实现：
//   通过 QNetworkAccessManager 异步 POST 到硅基流动 /chat/completions
//   请求体使用 nlohmann::json 构建，响应用 QJsonDocument 解析
// ──────────────────────────────────────────────────────────────────────

LLMClient::LLMClient(QObject *parent)
    : QObject(parent)
{
}

LLMClient::~LLMClient()
{
}

void LLMClient::setApiKey(const QString &key)
{
    m_apiKey = key.trimmed();
}

void LLMClient::setEndpoint(const QUrl &url)
{
    m_endpoint = url;
}

void LLMClient::setModel(const QString &model)
{
    m_model = model;
}

void LLMClient::requestSummary(int streamId, const QString &eventsText)
{
    if (m_apiKey.isEmpty()) {
        emit requestFailed(streamId, QStringLiteral("未配置 API Key"));
        return;
    }
    if (eventsText.trimmed().isEmpty()) {
        emit requestFailed(streamId, QStringLiteral("无事件可摘要"));
        return;
    }

    QNetworkRequest request(m_endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_apiKey).toUtf8());

    QByteArray body = QByteArray::fromStdString(
        buildRequestBody(eventsText).dump());

    QNetworkReply *reply = m_networkManager.post(request, body);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, streamId]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    emit requestFailed(streamId, reply->errorString());
                    return;
                }

                // 解析响应 JSON
                QJsonParseError parseErr;
                QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
                if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
                    emit requestFailed(streamId, QStringLiteral("响应解析失败: %1")
                                                       .arg(parseErr.errorString()));
                    return;
                }

                // choices[0].message.content
                const QJsonArray choices = doc.object().value("choices").toArray();
                if (choices.isEmpty()) {
                    emit requestFailed(streamId, QStringLiteral("响应无 choices"));
                    return;
                }
                const QString summary =
                    choices.at(0).toObject().value("message").toObject()
                        .value("content").toString();

                // 关键词提取：模型按 "关键词：a, b, c" 最后一行输出
                QStringList keywords;
                const QStringList lines = summary.split('\n');
                for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                    const QString line = it->trimmed();
                    if (line.startsWith(QStringLiteral("关键词"))) {
                        const QString kwPart = line.section(QStringLiteral("："), 1, -1)
                                                   .section(QStringLiteral(":"), 1, -1)
                                                   .trimmed();
                        // 按常见分隔符切分：中英文逗号/分号/顿号/空白
                        const QStringList parts = kwPart.split(
                            QRegularExpression(QStringLiteral("[,，;；、\\s]+")));
                        for (const QString &kw : parts) {
                            const QString k = kw.trimmed();
                            if (!k.isEmpty())
                                keywords << k;
                        }
                        break;
                    }
                }
                // 兜底：未按格式输出时，从摘要中取前 3 个词
                if (keywords.isEmpty()) {
                    const QStringList words = summary.split(
                        QRegularExpression(QStringLiteral("\\s+")));
                    for (const QString &w : words) {
                        if (w.size() >= 2 && keywords.size() < 3)
                            keywords << w.trimmed();
                    }
                }

                emit summaryReady(streamId, summary, keywords);
            });
}

nlohmann::json LLMClient::buildRequestBody(const QString &eventsText) const
{
    // 精心设计的提示词：结构化日志 → 人物全貌特征描述 → 关键词
    // 目的：引导 AI 生成包含"上衣、裤子、鞋子"等具体描述的摘要，提升检索精准度
    const QString prompt = QStringLiteral(
        "你是一个智能视频侦查助手。以下是摄像头捕捉到的事件日志：\n"
        "%1\n\n"
        "请分析日志内容。请严格按以下格式输出：\n"
        "摘要：用自然语言描述画面中人物的特征（如：一名穿蓝色上衣、白色裤子的男子走过，脚穿黑色鞋子）。\n"
        "关键词：提取 3-5 个关键特征词（如：人体、蓝色上衣、白色裤子、黑色鞋子、停留、闯入），用逗号分隔。\n"
        "（只输出以上两行，不要其他内容）").arg(eventsText);

    nlohmann::json body;
    body["model"] = m_model.toStdString();
    body["messages"] = nlohmann::json::array({
        nlohmann::json{{"role", "user"}, {"content", prompt.toStdString()}}
    });
    body["temperature"] = 0.3;
    body["max_tokens"] = 512;
    return body;
}
