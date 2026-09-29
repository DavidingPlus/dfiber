#include "timer.h"

#include "config.h"

#include <iostream>
#include <cassert>


bool Timer::cancel()
{
    // 写锁互斥锁保护 TimerManager 对象。
    std::unique_lock<std::shared_mutex> writeLock(m_manager->m_mutex);

    // 先标记 m_cb 为空，表示这个定时器失效了，不能再执行回调。即使后续删除失败了，Timer 也不会执行。
    if (!m_cb)
    {
        if (DFIBER_CONFIG_DEBUG) std::cout << "Timer::cancel(): m_cb is nullptr, cannot cancel" << std::endl;


        return false;
    }
    else
    {
        m_cb = nullptr;
    }

    // 从定时管理器中找到需要删除的定时器。
    auto it = m_manager->m_timers.find(shared_from_this());
    // 删除定时器。
    if (m_manager->m_timers.end() != it) m_manager->m_timers.erase(it);


    return true;
}

bool Timer::refresh()
{
    std::unique_lock<std::shared_mutex> writeLock(m_manager->m_mutex);

    if (!m_cb)
    {
        if (DFIBER_CONFIG_DEBUG) std::cout << "Timer::refresh(): m_cb is nullptr, cannot refresh" << std::endl;


        return false;
    }

    // 需要先从 TimerManager 中删除当前 Timer。因为 m_next 是 set 的排序依据，直接修改会破坏容器有序性。修改超时时间后，需要重新插入。
    // 在定时器集合中查找当前定时器。
    auto it = m_manager->m_timers.find(shared_from_this());
    // 检查定时器是否存在。
    if (m_manager->m_timers.end() == it) return false;

    // 删除当前定时器并更新超时时间。
    m_manager->m_timers.erase(it);
    m_next = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_ms);
    // 并且将新的定时器加入到定时器管理类中。
    m_manager->m_timers.insert(shared_from_this());


    return true;
}

bool Timer::reset(uint64_t ms, bool fromNow)
{
    // 检查是否要重置。若和原来的一摸一样，代表不需要重置。
    if (ms == m_ms && !fromNow) return true;

    // 同理先删再插。
    {
        std::unique_lock<std::shared_mutex> writeLock(m_manager->m_mutex);

        if (!m_cb) // 如果为空，说明该定时器已经被取消或未初始化，因此无法重置。
        {
            if (DFIBER_CONFIG_DEBUG) std::cout << "Timer::reset(): m_cb is nullptr, cannot reset" << std::endl;


            return false;
        }

        // 否则就是定时器已经初始化了。
        auto it = m_manager->m_timers.find(shared_from_this());
        if (m_manager->m_timers.end() == it) return false;
        // 找到删除。
        m_manager->m_timers.erase(it);
    }

    // 重新插入。如果为 true 则重新计算超时时间，为 false 就需要上一次的起点开始。
    auto start = fromNow ? std::chrono::steady_clock::now() : m_next - std::chrono::milliseconds(m_ms);
    m_ms = ms;
    m_next = start + std::chrono::milliseconds(m_ms);

    // addTimer 里面会处理加锁的逻辑，因此这里不用受锁的生命周期管理，否则可能死锁。
    m_manager->addTimer(shared_from_this());


    return true;
}

bool Timer::Comparator::operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs) const
{
    // 先比较触发的绝对时间，相同则返回。如果绝对时间相同，则比较地址。
    if (lhs->m_next < rhs->m_next)
    {
        return true;
    }
    else if (lhs->m_next > rhs->m_next)
    {
        return false;
    }
    else
    {
        return lhs.get() < rhs.get();
    }
}

std::shared_ptr<Timer> TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
{
    std::shared_ptr<Timer> timer(new Timer(ms, cb, recurring, this));
    addTimer(timer);


    return timer;
}

std::shared_ptr<Timer> TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weakCond, bool recurring)
{
    return addTimer(
        ms,
        // 这里不能按引用捕获（[&]），因为 addConditionTimer() 返回后，weakCond 和 cb 都会离开作用域，Lambda 中保存的引用会变成悬空引用，定时器到期执行时会产生未定义行为。应按值捕获（[weakCond, cb]），让 Lambda 自己保存一份 weakCond 和 cb，保证它们在 Timer 生命周期内一直有效。
        // 也可以使用 std::bind(&OnTimer, weakCond, cb)，std::bind 同样会按值保存参数，本质上与按值捕获的 Lambda 作用相同，都是为了保证回调执行时对象仍然有效。
        [weakCond = std::move(weakCond), cb = std::move(cb)]()
        {
            // 判断 weakCond 对应对象的生命周期是否还在，决定是否执行这个回调。
            if (weakCond.lock())
            {
                cb();
            }
            else
            {
                if (DFIBER_CONFIG_DEBUG) std::cout << "TimerManager::addConditionTimer(): object has expired, skip callback" << std::endl;
            }
        },
        recurring);
}

uint64_t TimerManager::getNextTimer()
{
    // m_tickled 是原子变量。下面只写 m_tickled，读 m_timers，因此这里只需要读写锁就行。
    std::shared_lock<std::shared_mutex> readLock(m_mutex);

    // 重置 m_tickled。问题：为什么在 getNextTimer() 中需要重置 mtickled = false？
    // m_tickled是一个状态标志，其核心作用是防止重复唤醒。当一个新的定时器被插入到时间堆的堆顶（即它成为了最早超时的任务）时，说明原有的 epoll_wait 超时时间已经不再准确，需要触发 onTimerlnsertedAtFront()（通常是调用 tickle()）来唤醒调度线程，使其重新计算超时时间。
    // 当一个线程被唤醒以后，他需要调用 getNextTimer() 重新获取超时时间。在 getNextTimer() 函数中，调度线程已经处于“醒来”并准备重新计算超时值的状态。此时已经执行过一次唤醒逻辑了，需要将 m_tickled 重置为 false，是为了确保后续如果有更新的、更早的定时器插入时，能够再次触发 onTimerlnsertedAtFront() 逻辑。如果不重置，后续更早的定时器插入将无法唤醒线程，导致调度延迟。
    m_tickled = false;

    // 返回最大值。这里不能直接调用 hasTimer()，不能锁上加锁。
    if (m_timers.empty()) return ~0ull;

    // 获取当前系统时间。
    auto now = std::chrono::steady_clock::now();
    // 获取最小时间堆中的第一个超时定时器判断超时。
    auto time = (*m_timers.begin())->m_next;

    // 判断当前时间是否已经超过了下一个定时器的超时时间。
    if (now >= time)
    {
        // 已经有 timer 超时，直接返回。
        return 0;
    }
    else
    {
        // 计算从当前时间到下一个定时器超时时间的时间差，结果是一个 std::chrono::milliseconds 对象。
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);


        // 将时间差转换为毫秒，并返回这个值。
        return static_cast<uint64_t>(duration.count());
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
{
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> writeLock(m_mutex);

    // 由于 Timer 按超时时间升序排列，因此只需要不断检查集合中的第一个 Timer。当第一个 Timer 未超时时，后面的 Timer 一定也未超时，可以直接结束循环。
    while (!m_timers.empty() && (*m_timers.begin())->m_next <= now)
    {
        // 取出最早超时的 Timer。
        auto timer = *m_timers.begin();
        // 从 Timer 集合中移除。
        m_timers.erase(m_timers.begin());
        // 收集回调，由调度器统一执行。这里仅负责收集回调，不直接执行，避免长时间持有锁。
        cbs.emplace_back(timer->m_cb);

        // 循环 Timer：根据当前时间重新计算下一次触发时间，并重新加入 Timer 集合。
        if (timer->m_recurring)
        {

            timer->m_next = now + std::chrono::milliseconds(timer->m_ms);
            m_timers.insert(timer);
        }
        // 单次 Timer：清空回调函数，标记 Timer 已失效，释放其可能持有的资源。
        else
        {
            timer->m_cb = nullptr;
        }
    }
}

bool TimerManager::hasTimer()
{
    std::shared_lock<std::shared_mutex> readLock(m_mutex);


    return !m_timers.empty();
}

void TimerManager::addTimer(std::shared_ptr<Timer> timer)
{
    // 标识插入的 Timer 是否成为当前最早超时的定时器，并且需要唤醒其他正在等待超时的调度线程。
    bool atFrontNeedTickle = false;

    {
        std::unique_lock<std::shared_mutex> writeLock(m_mutex);

        // 将 Timer 插入时间集合。m_timers 使用 set 按 m_next 排序，因此 begin() 始终是最早触发的 Timer。
        auto it = m_timers.insert(timer).first;

        // 如果新插入的 Timer 位于集合最前面，说明它比原来的 Timer 更早触发。此时可能需要唤醒正在等待超时的调度线程，重新计算等待时间，因此有更紧急的任务需要处理，不能只关注原来的任务。
        // m_tickled 用于保证只触发一次唤醒操作，避免重复通知。
        atFrontNeedTickle = (m_timers.begin() == it) && !m_tickled;

        // 标记已经通知过，直到调度线程处理并重新调用 getNextTimer() 后清除。
        if (atFrontNeedTickle) m_tickled = true;
    }

    // 唤醒调度线程。具体实现由子类重写，用于唤醒阻塞在 epoll_wait 等待中的线程。同样不能锁上加锁，所以放在锁生命周期外面。
    if (atFrontNeedTickle) onTimerInsertedAtFront();
}
