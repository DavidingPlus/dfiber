// 升序链表实现定时器。

#pragma once

#include <cstdint>
#include <functional>

#include "config.h"


class Timer
{
    friend class TimerManager;

public:

    using Callback = std::function<void()>;

    explicit Timer(uint64_t expire, Callback cb)
        : m_expire(expire), m_callback(std::move(cb)) {}


private:

    // 超时时间（ms）。
    uint64_t m_expire;

    // 超时回调。
    Callback m_callback;

    Timer *prev = nullptr, *next = nullptr;
};


class TimerManager
{
public:

    TimerManager() = default;

    ~TimerManager();

    // 向定时器管理器添加一个新的定时器。
    void addTimer(Timer *timer);

    // 把一个 Timer 从链表中移除。注意：这里只是“摘链”，不是一定要释放内存。
    void delTimer(Timer *timer);

    // 修改一个已经存在的 Timer 的超时时间。需要和 delTimer() 配合使用，先删再插。
    void adjustTimer(Timer *timer, uint64_t newExpire);

    // 检查哪些 Timer 已经超时，并执行完所有超时的定时任务。这个函数的外部一般是有人在调用这个函数检查是否超时。
    void tick();


#if DFIBER_CONFIG_DEBUG

    // 打印定时器管理器中的定时器的信息。
    void printTimers();

#endif


private:

    Timer *m_head = nullptr, *m_tail = nullptr;
};
