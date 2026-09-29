#include "scheduler.h"

#include "hook.h"
#include "config.h"

#include <cassert>
#include <chrono>


DFIBER_NAMESPACE_BEGIN(dfiber)


// 当前线程绑定的调度器对象。每个线程都有一个 thread_local 的 Scheduler 指针，用于记录当前线程正在运行的调度器，他们可能指向的是同一个调度器对象。
static thread_local Scheduler *t_scheduler = nullptr;


Scheduler *Scheduler::GetThis()
{
    return t_scheduler;
}

void Scheduler::SetThis()
{
    t_scheduler = this;
}

Scheduler::Scheduler(size_t threads, bool useCaller, const std::string &name)
    : m_useCaller(useCaller), m_name(name)
{
    assert(threads > 0 && Scheduler::GetThis() == nullptr); // 首先判断线程的数量是否大于 0，并且调度器的对象是否是空指针，是就调用 setThis() 进行设置。

    Thread::SetName(m_name); // 设置当前线程的名称为调度器的名称 m_name。

    // 设置调度协程对象。主线程创建出来的和工作线程创建出来的线程局部变量 t_scheduler 是两个值。
    // useCaller 为 true 的情况下，t_scheduler 为 this，为 false 的情况，为 nullptr。
    SetThis();

    // useCaller 决定创建调度器的线程（Caller 线程）是否作为工作线程参与协程调度。
    // useCaller = true：表示 Caller 线程也要作为一个工作线程使用。Caller 线程需要执行 Scheduler::run()，但由于它还要继续运行主流程（如 main 函数），因此不能直接进入 run()，而是额外创建一个调度协程（Scheduler Fiber），由主协程切换到调度协程，再由调度协程负责调度和执行各个任务协程。
    // useCaller = false：Caller 线程仅负责创建、启动和停止调度器，不参与协程调度。调度工作全部交给新创建的 Worker 线程完成。由于 Worker 线程的入口函数本身就是 Scheduler::run()，无需再额外创建调度协程，线程直接进入调度循环即可。
    if (useCaller)
    {
        --threads; // 因为主线程作为了工作线程所以需要额外创建的线程数量减 1。

        // 创建主协程。
        Fiber::GetThis();

        // 创建调度协程。调度协程显然是 false -> 协程退出后将返回主协程。
        // void Scheduler::run() 等价于 void Scheduler::run(Scheduler* this)，成员函数必须知道作用于哪个对象，因此绑定 this 进去，std::bind(&Scheduler::run, this)。
        m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        // 设置协程的调度器对象。
        Fiber::SetSchedulerFiber(m_schedulerFiber.get());

        // 获取主线程 ID。
        m_rootThread = Thread::GetThreadId();
        m_threadIds.emplace_back(m_rootThread);
    }

    m_threadCount = threads; // 将剩余的线程数量（即总线程数量减去是否使用调用者线程）赋值给 m_threadCount。

    if (DFIBER_CONFIG_DEBUG) std::cout << "Scheduler::Scheduler() success" << std::endl;
}

Scheduler::~Scheduler()
{
    assert(true == stopping()); // 判断调度器是否终止。

    // 获取调度器的对象。
    if (this == GetThis()) t_scheduler = nullptr; // 将其设置为 nullptr 防止悬空指针。

    if (DFIBER_CONFIG_DEBUG) std::cout << "Scheduler::~Scheduler() success" << std::endl;
}

void Scheduler::start()
{
    std::lock_guard<std::mutex> lock(m_mutex); // 互斥锁防止共享资源的竞争。

    if (m_stopping) // 如果调度器退出直接报错打印 cerr 后面的话。
    {
        std::cerr << "Scheduler is stopped" << std::endl;


        return;
    }

    assert(m_threads.empty()); // 首先线程池数量为空。
    m_threads.resize(m_threadCount);

    for (size_t i = 0; i < m_threadCount; ++i)
    {
        // 进程全局只有一个调度器，这个调度器管理一个线程池，每个线程都执行同一个调度器的调度循环（run()），共同消费同一个任务队列。
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
        m_threadIds.emplace_back(m_threads[i]->getId());
    }

    if (DFIBER_CONFIG_DEBUG) std::cout << "Scheduler::start() success" << std::endl;
}

void Scheduler::stop()
{
    if (DFIBER_CONFIG_DEBUG) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;

    // 1. 判断是否已经可以停止。
    if (stopping()) return;

    // 告诉所有线程，以后不要一直调度了，可以准备退出。
    m_stopping = true;

    // 2. 检查 stop() 是否再正确的线程调用。从目前的设计来看，这个检查没有意义。
    // 不管是 m_useCaller 为 true 还是 false，都必须在主线程里面 SetThis() 绑定好协程调度器，因为管理的操作一定是在主线程发生的，因此一定要保证 GetThis() 这类函数的返回值不是 nullptr。具体见 IOManager 的 addEvent() 的 eventCtx.scheduler = Scheduler::GetThis()。因此下面的检测其实没有意义。
    // 下面的 this 指针对应的 Scheduler 对象是主线程最开始创建调度器的那个 Scheduler 对象。调用 Scheduler::stop() 函数的是一定主线程，例如 main() 函数里面！
    if (DFIBER_CONFIG_DEBUG)
    {
        std::cout << "this: " << this << '\n'
                  << "GetThis(): " << GetThis() << std::endl;
    }

    // TODO 这个判断从当前设计上暂时无意义，从语义上有意义，后续优化。
    // m_useCaller ? assert(this == GetThis()) : assert(this != GetThis());

    // 3. 唤醒所有睡眠中的线程。
    // 调用 tickle() 的目的唤醒空闲线程或协程，防止 m_scheduler 或其他线程处于永久阻塞在等待任务的状态中。
    for (size_t i = 0; i < m_threadCount; ++i)
    {
        tickle();

        {
            // 注意：tickle 使用 pipe 作为唤醒机制时，多个写入事件可能被同一个 epoll_wait 线程一次性消费，导致其他本该被唤醒的线程依旧阻塞，因此 tickle 次数不严格对应唤醒线程数。stop 时需要确保所有等待 epoll 的线程最终都能重新检查 stopping 状态。
            // TODO 这里使用睡眠机制让主线程分批次写管道，保证所有子线程都被唤醒。后续改为 eventfd()。
            HookEnableGuard guard(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 发生在 useCaller 为 true 时，唤醒执行调度协程的线程。
    if (m_schedulerFiber) tickle();
    // 唤醒调度协程以后，让它继续任务调度，
    if (m_schedulerFiber)
    {
        m_schedulerFiber->resume(); // 开始任务调度。

        if (DFIBER_CONFIG_DEBUG) std::cout << "m_schedulerFiber ends in thread: " << Thread::GetThreadId() << std::endl;
    }

    // 获取此时的线程通过 swap 不会增加引用计数的方式加入到 thrs，方便下面的 join 保持线程正常退出。
    std::vector<std::shared_ptr<Thread>> thrs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        thrs.swap(m_threads);
    }

    for (auto &i : thrs) i->join();

    if (DFIBER_CONFIG_DEBUG) std::cout << "Schedule::stop() ends in thread: " << Thread::GetThreadId() << std::endl;
}

void Scheduler::tickle()
{
    // 留空。具体代码在子类 io+scheduler。
}

void Scheduler::run()
{
    // 获取当前线程的 ID。
    int threadId = Thread::GetThreadId();
    if (DFIBER_CONFIG_DEBUG) std::cout << "Schedule::run() starts in thread: " << threadId << std::endl;

    // set_hook_enable(true);

    // 设置调度器对象。
    SetThis();

    // 新创建的工作线程刚启动时还没有主协程，需要先创建线程的 Main Fiber。后续任务协程会以 Scheduler Fiber（Caller）或 Main Fiber（Worker）作为返回点进行切换。注意：这里只创建 Main Fiber，不会创建 Scheduler Fiber。Scheduler Fiber 仅在 useCaller=true 时，为 Caller 线程额外创建。
    if (threadId != m_rootThread) Fiber::GetThis();

    // 创建空闲协程，std::make_shared 是 C++11 引入的一个函数，用于创建 std::shared_ptr 对象。相比于直接使用 std::shared_ptr 构造函数，std::make_shared 更高效且更安全，因为它在单个内存分配中同时分配了控制块和对象，避免了额外的内存分配和指针操作。
    std::shared_ptr<Fiber> idleFiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));

    ScheduleTask task;
    while (true)
    {
        task.reset();
        // 是否需要唤醒其他线程进行任务调度。
        bool tickleMe = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 不能直接取第一个任务，因为 Scheduler 支持指定任务运行在指定线程上。
            auto it = m_tasks.begin();
            // 1. 遍历任务队列。
            while (m_tasks.end() != it)
            {
                // 设置了指定的线程 ID，就遍历到指定的线程 ID 再取任务。
                if (-1 != it->thread && threadId != it->thread)
                {
                    ++it;
                    tickleMe = true; // 这个任务虽然不是我的，但是可以被其他线程执行，因此设置为 true。

                    continue;
                }

                // 2. 取出任务。
                assert(it->fiber || it->cb);
                task = *it;
                m_tasks.erase(it);
                ++m_activeThreadCount;

                break; // 这里取到任务的线程就直接 break 所以并没有遍历到队尾。
            }
            // 代码执行到这里，说明已经取走任务了。如果队列中还有任务，那么仍然可以通知其他线程醒来工作。加第二个条件的判断是因为上面的遍历任务队列上来就命中了，直接退出循环，这时 tickleMe 是 false，因此需要两个条件一起判断。
            tickleMe = tickleMe || (it != m_tasks.end());
        }

        // 这里虽然写了唤醒但并没有具体的逻辑代码，具体的在 io+scheduler。
        if (tickleMe) tickle();

        // 3. 执行任务。
        if (task.fiber)
        {
            // resume 协程，resume 返回时此时任务要么执行完了，要么半路 yield，总之任务完成了，活跃线程 -1。
            {
                std::lock_guard<std::mutex> lock(task.fiber->m_mutex);

                if (Fiber::TERMINATE != task.fiber->getState()) task.fiber->resume();
            }

            --m_activeThreadCount; // 线程完成任务后就不再处于活跃状态，而是进入空闲状态，因此需要将活跃线程计数减一。
            task.reset();
        }
        else if (task.cb)
        {
            // 对于函数也应该被调度，具体做法就封装成协程加入调度。
            std::shared_ptr<Fiber> cbFiber = std::make_shared<Fiber>(task.cb);
            {
                std::lock_guard<std::mutex> lock(cbFiber->m_mutex);

                cbFiber->resume(); // 刚创建出来的工作协程状态是 READY，可以被直接 resume()。
            }

            --m_activeThreadCount;
            task.reset();
        }
        // 4. 无任务 -> 执行空闲协程。
        else
        {
            // 系统关闭 -> idle 协程将从死循环跳出并结束 -> 此时的 idle 协程状态为 TERMINATE -> 再次进入将跳出循环并退出 run()。
            if (Fiber::TERMINATE == idleFiber->getState())
            {
                // 如果调度器没有调度任务，那么 idle 协程回不断的 resume/yield，不会结束进入一个忙等待，如果 idle 协程结束了，一定是调度器停止了，直到有任务才执行上面的 if/else。idle 协程不断与当前线程的调度入口（Main Fiber 或 Scheduler Fiber）进行切换。当 stopping() 返回 true 时，idle 协程结束，run() 也随之退出。
                if (DFIBER_CONFIG_DEBUG) std::cout << "Schedule::run() ends in thread: " << threadId << std::endl;

                break;
            }

            ++m_idleThreadCount;
            idleFiber->resume();
            --m_idleThreadCount;
        }
    }
}

void Scheduler::idle()
{
    // 当 stop() 将 m_stopping 设置为 true 后，调度线程不会立即退出，而是继续执行 Scheduler::run()，将任务队列中剩余的任务全部调度完成。当没有可执行任务时，会进入空闲协程 idle()。此时 stopping() 返回 true，idle() 会退出循环并结束，随后 Scheduler::run() 跳出主循环，最终使调度线程（或调度协程）正常结束，实现调度器的优雅退出（Graceful Shutdown）。
    while (!stopping())
    {
        if (DFIBER_CONFIG_DEBUG) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;

        {
            // 降低空闲协程在无任务时对 cpu 占用率，避免空转浪费资源。
            HookEnableGuard guard(false);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        Fiber::GetThis()->yield();
    }
}

bool Scheduler::stopping()
{
    // 使用互斥锁的目的因为 m_tasks，mactiveThreadCount 会被多线程竞争所以需要互斥锁来保护资源的访问。
    // 此时这个函数的目的就是为了判断调度器是否退出。在 stop 函数中如果 stopping 为 true 代表调度器已经退出直接返回 return 不进行任何的操作。
    std::lock_guard<std::mutex> lock(m_mutex);


    return m_stopping && m_tasks.empty() && 0 == m_activeThreadCount;
}


DFIBER_NAMESPACE_END
