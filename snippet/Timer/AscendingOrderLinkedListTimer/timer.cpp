#include "timer.h"

#include <iostream>
#include <chrono>


TimerManager::~TimerManager()
{
    while (m_head)
    {
        Timer *tmp = m_head;
        m_head = m_head->next;
        delete tmp;
    }
}

void TimerManager::addTimer(Timer *timer)
{
    if (!timer) return;

    // 空链表。
    if (!m_head)
    {
        m_head = m_tail = timer;


        return;
    }

    // 插到链表头。
    if (timer->m_expire < m_head->m_expire)
    {
        timer->next = m_head;
        m_head->prev = timer;
        m_head = timer;


        return;
    }

    // 从头开始寻找插入位置。
    Timer *cur = m_head;

    // 包含了找到指定位置和找到链表尾部的两种情况。
    while (cur->next && cur->next->m_expire <= timer->m_expire) cur = cur->next;

    timer->next = cur->next;
    timer->prev = cur;

    cur->next ? cur->next->prev = timer : m_tail = timer;
    cur->next = timer;
}

void TimerManager::delTimer(Timer *timer)
{
    if (!timer) return;

    if (timer == m_head) m_head = m_head->next;
    if (timer == m_tail) m_tail = m_tail->prev;

    if (timer->prev) timer->prev->next = timer->next;
    if (timer->next) timer->next->prev = timer->prev;

    timer->prev = nullptr;
    timer->next = nullptr;
}

void TimerManager::adjustTimer(Timer *timer, uint64_t newExpire)
{
    if (!timer) return;

    delTimer(timer);
    timer->m_expire = newExpire;
    addTimer(timer);
}

void TimerManager::tick()
{
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (DFIBER_CONFIG_DEBUG) std::cout << "TimerManager::tick() now: " << now << std::endl;

    // 这里的循环是为了让所有超时的定时任务都被执行掉。
    while (m_head)
    {
        // 找到未超时的任务，退出。
        if (now < m_head->m_expire) break;

        // 从链表头开始执行超时的定时任务，每次执行完就删除链表头的任务，直到所有超时任务被执行完。
        Timer *timer = m_head;

        if (timer->m_callback) timer->m_callback();

        delTimer(timer);

        delete timer;
    }
}

#if DFIBER_CONFIG_DEBUG

void TimerManager::printTimers()
{
    std::cout << "TimerManager::printTimers(): ";

    for (Timer *cur = m_head; cur; cur = cur->next) std::cout << cur->m_expire << " ";

    std::cout << std::endl;
}

#endif
