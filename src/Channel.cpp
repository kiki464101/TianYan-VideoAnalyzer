#include "Channel.h"

Channel::Channel(int fd)
    : m_fd(fd)
{
}

int Channel::fd() const
{
    return m_fd;
}

void Channel::setFd(int fd)
{
    m_fd = fd;
}

void Channel::setReadCallback(EventCallback cb)
{
    m_readCallback = std::move(cb);
}

void Channel::setWriteCallback(EventCallback cb)
{
    m_writeCallback = std::move(cb);
}

void Channel::setErrorCallback(EventCallback cb)
{
    m_errorCallback = std::move(cb);
}

void Channel::setCloseCallback(EventCallback cb)
{
    m_closeCallback = std::move(cb);
}

int Channel::events() const
{
    return m_events;
}

void Channel::setEvents(int events)
{
    m_events = events;
}

void Channel::enableReading()
{
}

void Channel::disableReading()
{
}

void Channel::enableWriting()
{
}

void Channel::disableWriting()
{
}

void Channel::handleEvent(int revents) const
{
    (void)revents;
}
