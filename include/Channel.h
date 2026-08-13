#ifndef CHANNEL_H
#define CHANNEL_H

#include <functional>

// fd 事件封装（muduo 风格）：一个 fd 绑定一组回调，事件到达时触发
class Channel
{
public:
    using EventCallback = std::function<void()>;

    explicit Channel(int fd = -1);
    ~Channel() = default;

    // fd 归属：Channel 不拥有 fd 的 close 权，仅持有编号
    int  fd() const;
    void setFd(int fd);

    // 回调注册
    void setReadCallback(EventCallback cb);
    void setWriteCallback(EventCallback cb);
    void setErrorCallback(EventCallback cb);
    void setCloseCallback(EventCallback cb);

    // 事件标记（位或：Readable/Writable/Error）
    int  events() const;
    void setEvents(int events);
    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();

    // 分发：根据 revents 调用对应回调
    void handleEvent(int revents) const;

private:
    int  m_fd = -1;
    int  m_events = 0;
    int  m_revents = 0;
    EventCallback m_readCallback;
    EventCallback m_writeCallback;
    EventCallback m_errorCallback;
    EventCallback m_closeCallback;
};

#endif // CHANNEL_H
