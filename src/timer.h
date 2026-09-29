#ifndef _DFIBER_TIMER_H_
#define _DFIBER_TIMER_H_

#include <memory>
#include <functional>
#include <chrono>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <atomic>


class TimerManager;


// Timer 使用 shared_ptr 管理生命周期。Timer 在 refresh/reset 时会从 TimerManager 的容器中删除并重新插入，但不会创建新的 Timer 对象。shared_ptr 可以保证 Timer 在被 TimerManager 移除后，如果仍有外部引用，对象不会立即析构，避免出现悬空指针问题。

// 关于 Tick（滴答）信号的设计：
// 1. 固定周期触发：传统的升序链表或时间轮方案通常依赖一个固定周期的信号（如每 10 ms 触发一次），通过不断轮询检查是否有任务超时。
// 2. 动态超时触发（Sylar 方案）：Sylar 采用动态计算超时时间的思路。每次计算出距离堆顶（最早超时）定时器的剩余时间，并将其作为下一次阻塞等待的超时值。一旦被唤醒，至少有一个定时器必然到期。这种方式避免了无意义的周期性轮询，使定时更加精确且节能。

// Sylar 定时器的具体设计：
// 1. 数据结构：采用最小堆逻辑（源码中通常使用 std::set 存储定时器指针，不是最小堆但可以模拟最小堆的功能），可以快速定位并获取当前最小的超时时间。
// 2. 时间转换：用户注册时提供的是相对时间（如 3 秒后执行），Sylar 会将其转换为绝对稳定时间点（如当前的 UNIX 时间戳 + 3000ms）进行存储。
// 3. 精度限制：受限于底层的 epol_wait 系统调用，其超时精度通常维持在毫秒级。

// 定时器与 IO 协程调度器的整合：
// IO 协程调度器在空闲（ldle）状态下会阻塞在 epoll_wait 上等待网络 IO 事件。
// 1. 超时动态调整：在引入定时器前，epoll_wait 通常设置固定的超时时间（如 5 秒）。整合后，epoll_wait 的超时参数改用当前定时器管理器中最小的超时剩余时间。
// 2. 唤醒逻辑：epoll_wait 返回后，调度器会获取当前的绝对稳定时间，将所有超时时间点早于当前时间的定时器收集起来，并将其回调函数推入任务队列执行。
// 问题：epoll 唤醒一定代表定时器超时吗？不一定。
// 1. IO 事件到达（如套接字可读写）；
// 2. 达到了设定的超时时间（定时器到期）。
// 因此，当 epoll_wait 返回时，不能理所当然地认为定时器已超时。此时，调度器需要再次比对当前绝对时间与定时器的目标时间，从而精准判断哪些定时任务需要被处理。
class Timer : public std::enable_shared_from_this<Timer>
{

    friend class TimerManager;

public:

    // 从时间堆中删除 timer。
    bool cancel();

    // 刷新 timer。保持原来的超时时间间隔不变，仅将下一次触发时间重新设置为当前时间 + m_ms。常用于延长定时器生命周期，例如连接保活。
    bool refresh();

    // 重设 timer 的超时时间。ms 表示新的超时间隔，fromNow 表示是否从当前时间开始重新计算：true：从当前时间开始计算新的超时时间；false：保持原来的起始时间，仅修改时间间隔。可用于动态调整定时器周期。
    bool reset(uint64_t ms, bool fromNow);


private:

    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager)
        : m_ms(ms), m_cb(std::move(cb)), m_recurring(recurring), m_manager(manager) { m_next = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_ms); }


private:

    // 是否循环。
    bool m_recurring = false;

    // 定时器执行间隔（ms），用于循环定时器中重新计算下一次触发时间。对于单次定时器就是超时时间。
    uint64_t m_ms = 0;

    // 下一次触发的绝对时间。
    // 使用 steady_clock 而不是 system_clock。steady_clock 是单调递增时钟，不受系统时间修改（如手动调时、NTP 校时）影响，适合用于定时器、超时检测等场景；system_clock 表示真实系统时间，更适合日志、日期时间等用途。
    std::chrono::time_point<std::chrono::steady_clock> m_next;

    // 超时时触发的回调函数。
    std::function<void()> m_cb;

    // 管理此 timer 的管理器。
    TimerManager *m_manager = nullptr;


private:

    // 实现最小堆的比较函数，⽤于⽐较两个 Timer 对象，⽐较的依据是绝对超时时间。
    struct Comparator
    {
        bool operator()(const std::shared_ptr<Timer> &lhs, const std::shared_ptr<Timer> &rhs) const;
    };
};


class TimerManager
{
    friend class Timer;

public:

    TimerManager() = default;

    virtual ~TimerManager() = default;

    // 添加 timer。ms 定时器执行间隔时间。cb 定时器回调函数。recurring 是否循环定时器。
    // 将其加入 TimerManager 管理。返回 shared_ptr，方便用户后续调用 cancel、refresh、reset 等接口操作该 Timer。
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件 timer。
    // 条件定时器用于解决回调依赖对象生命周期的问题。即使定时器到期，如果关联对象已经提前销毁（例如连接断开、Session 被释放），则不会执行回调，避免访问无效对象或执行无意义的任务。
    // weak_ptr 是 shared_ptr 的弱引用，不会增加对象的引用计数，也不会影响对象生命周期。使用 lock() 可以尝试获得一个 shared_ptr。若对象仍然存在，lock() 返回有效的 shared_ptr；对象已销毁，lock() 返回 nullptr。常用于判断对象是否仍然存活，避免访问已经释放的对象。
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weakCond, bool recurring = false);

    // 拿到堆中最近的超时时间。
    uint64_t getNextTimer();

    // 提取当前已经超时的 Timer，并将对应的回调函数收集到 cbs 中。同时负责处理循环 Timer 的重新调度逻辑。它本身不执行任务，只是负责“把到期任务取出来交给调度器”。
    void listExpiredCb(std::vector<std::function<void()>> &cbs);

    // 堆中是否有定时器 timer。
    bool hasTimer();


protected:

    // 当一个最早的 timer 加入到堆中 -> 调用该函数。
    virtual void onTimerInsertedAtFront() {}

    // 添加 timer。这个私有函数是方便内部和 Timer 使用的，外部使用共有函数的那个重载版本。
    void addTimer(std::shared_ptr<Timer> timer);


private:

    std::shared_mutex m_mutex;

    // 时间堆。存储所有的 Timer 对象，并使用 Timer::Comparator 进行排序，确保最早超时的 Timer 在最前面。
    // 使用 std::set（底层是红黑树，这里是拿来模拟最小堆）而不是 std::priority_queue（底层是堆）。两者都能 O(logN) 插入、O(1) 获取最早超时的 Timer（begin()/top()），但 std::set 支持查找、删除和重新插入任意 Timer，便于实现 cancel()、refresh()、reset() 等操作，而 priority_queue 只能操作堆顶元素。
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;

    // 标识当前最早超时 Timer 是否已经触发过唤醒通知。当新的 Timer 成为最早超时定时器时，说明它比原来的 Timer 更早触发。此时可能需要唤醒正在等待超时的调度线程，重新计算等待时间，因此有更紧急的任务需要处理，不能只关注原来的任务。通过 onTimerInsertedAtFront() 唤醒调度线程，唤醒的线程会重新调用 getNextTimer() 函数。在调度线程重新调用 getNextTimer() 之前，只允许触发一次唤醒，避免重复通知。
    std::atomic_bool m_tickled = false;
};


#endif
