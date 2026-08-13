#include "EventLoop.h"
#include "Channel.h"

EventLoop::EventLoop(QObject *parent)
    : QObject(parent)
{
}

EventLoop::~EventLoop()
{
}

bool EventLoop::init(int maxEvents)
{
    Q_UNUSED(maxEvents);
    return false;
}

void EventLoop::run()
{
}

void EventLoop::stop()
{
}

bool EventLoop::addChannel(Channel *channel)
{
    Q_UNUSED(channel);
    return false;
}

bool EventLoop::removeChannel(Channel *channel)
{
    Q_UNUSED(channel);
    return false;
}

bool EventLoop::updateChannel(Channel *channel)
{
    Q_UNUSED(channel);
    return false;
}

int EventLoop::epollFd() const
{
    return m_epollFd;
}
