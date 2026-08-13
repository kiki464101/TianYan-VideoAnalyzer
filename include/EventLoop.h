#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <atomic>
#include <functional>
#include <vector>

#include <QObject>

class Channel;

// 事件循环：单线程 epoll_wait 循环（Linux 实现），fd 抽象保持跨平台可编译
class EventLoop : public QObject
{
    Q_OBJECT

public:
    explicit EventLoop(QObject *parent = nullptr);
    ~EventLoop() override;

    // 生命周期
    bool init(int maxEvents = 1024);
    void run();
    void stop();

    // fd 注册（实际 epoll_ctl 在 Linux 实现中完成）
    bool addChannel(Channel *channel);
    bool removeChannel(Channel *channel);
    bool updateChannel(Channel *channel);

    int epollFd() const;

private:
    int m_epollFd = -1;                    // Linux epoll 句柄；Windows 下为 -1
    std::atomic<bool> m_running{false};
    std::vector<Channel *> m_channels;
};

#endif // EVENTLOOP_H
