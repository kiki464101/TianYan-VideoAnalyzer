#ifndef LLMCLIENT_H
#define LLMCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>

#include <nlohmann/json.hpp>

// LLM 客户端：调用 DeepSeek API 生成视频摘要（异步）
class LLMClient : public QObject
{
    Q_OBJECT

public:
    explicit LLMClient(QObject *parent = nullptr);
    ~LLMClient() override;

    // 配置
    void setApiKey(const QString &key);
    void setEndpoint(const QUrl &url);   // 默认 https://api.deepseek.com/chat/completions
    void setModel(const QString &model); // 默认 deepseek-chat

    // 异步生成摘要：入参为结构化事件描述（EventDetector::EventRecord 序列化文本）
    void requestSummary(int streamId, const QString &eventsText);

signals:
    void summaryReady(int streamId, const QString &summary, const QStringList &keywords);
    void requestFailed(int streamId, const QString &error);

private:
    nlohmann::json buildRequestBody(const QString &eventsText) const;

    QNetworkAccessManager m_networkManager;
    QString m_apiKey;
    QUrl    m_endpoint = QUrl("https://api.deepseek.com/chat/completions");
    QString m_model = "deepseek-chat";
};

#endif // LLMCLIENT_H
