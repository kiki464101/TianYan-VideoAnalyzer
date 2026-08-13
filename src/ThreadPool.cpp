#include "ThreadPool.h"

ThreadPool::ThreadPool(size_t threadCount)
    : m_threadCount(threadCount)
{
}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::start()
{
}

bool ThreadPool::enqueue(Task task)
{
    (void)task;
    return false;
}

void ThreadPool::stop()
{
}

void ThreadPool::workerLoop()
{
}

size_t ThreadPool::threadCount() const
{
    return m_threadCount;
}

size_t ThreadPool::pendingTasks() const
{
    return 0;
}
