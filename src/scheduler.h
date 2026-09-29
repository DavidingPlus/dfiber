#ifndef _DFIBER_SCHEDULER_H_
#define _DFIBER_SCHEDULER_H_

#include <mutex>
#include <vector>

#include "fiber.h"
#include "thread.h"
#include "globalmacros.h"


DFIBER_NAMESPACE_BEGIN(dfiber)


// 现在这套调度器的设计，除了 stop 里面看到了 m_schedulerFiber->resume()，其他地方全是工作线程自己在跑 run() 函数，那这个调度协程 m_schedulerFiber 是怎么起作用的呢？
// m_schedulerFiber 并不是整个线程池的“中央调度器”，它也没办法跨线程调度 Worker 线程上的协程。协程上下文切换（swapcontext）只能发生在同一个线程内部，因此每个 Worker 线程都必须自己执行 Scheduler::run() 来调度本线程上的协程。从这个角度来讲，工作线程自己创建的主协程和主线程额外创建的调度协程的作用是一致的。m_schedulerFiber 的真正作用只有一个：让 Caller（主线程）也能够以协程的方式进入 Scheduler::run() 调度循环，从而参与任务调度。也就是说，useCaller=true 时，主线程会通过：Main Fiber -> Scheduler Fiber -> Scheduler::run() 的方式成为一个调度线程，与其它 Worker 线程一起消费同一个任务队列。
// TODO 当前的设计中没有在调度器启动阶段主动 resume(m_schedulerFiber)，而在 stop() 中 resume 一次。在主线程中，我们 start 调度器以后，如果立马执行 stop 函数，主线程跑 stop 函数时，会自动进入 m_schedulerFiber 的 resume() 函数流程，主线程从主协程切换到调度协程，从而参与工作，最后完成工作后在 stop() 函数中优雅退出。但这样的问题是，我们无法预测主线程在 start 调度器以后会做什么工作，这样可能会有主线程参与不了工作以及其他风险。

// 调度器（Scheduler）的出现，把程序员从繁琐的上下文切换中解放了出来。它让调度协程充当了全职管家，没事时管家在后台待命，有活时管家就把执行权交给子协程，任务完成后子协程自动交还执行权，从而实现了任务的自动化批量消耗。
// 引入协程调度以前，协程的调度都是由用户进行 resume 或 yield 的，这就好比让用户充当了调度器的工作，显然是不够灵活的。引入了协程调度后，则可以先创建一个协程调度器，然后把这些要调度的协程传递个协程调度器，让其一个个消耗。
// 调度器任务的定义：对于协程调度器而言，协程是原生的调度单位。函数同样可以作为调度任务。原因在于：协程本质上是函数及其运行状态（上下文）的组合。将函数视为任务，可以简化用户的使用逻辑。在实际实现中，调度器内部会将传入的函数包装成协程后再进行调度，但对外部接口而言，调度器应同时支持协程和函数。
// 多线程调度：基于协程的基本特性，一个线程在同一时刻只能运行一个协程。为了提升调度器的吞吐量和并发能力，引入多线程调度机制。通过维护一个线程池，实现多个线程同时运行多个协程，这种 M：N 的调度模型（M 个协程运行在 N 个线程上）在处理高并发 IO 时效率远高于单线程。
// 调度器内部维护一个任务队列和一个调度线程池。启动调度后，线程池按序从队列中取出任务执行。调度线程池可灵活包含 Caller 线程。当所有任务执行完毕且无新任务时，线程池进入空闲状态。一旦新任务到达，通过通知机制唤醒线程池重新开始调度。执行停止逻辑时，各调度线程依次退出，最终完成调度器的资源释放。
// 注意区分几个概念：调度器，调度线程，调度协程！！！
/*
 * 调度器不仅可以管理新创建的工作线程，还可以将创建调度器的 Caller 线程纳入调度范围（useCaller=true）。
 * 如果不使用 Caller 线程参与调度，那么 Caller 在线程启动调度器后通常只能等待其他工作线程结束，无法继续执行任务，造成线程资源的浪费；而将 Caller 线程作为调度线程之一，则无需额外创建一个工作线程，能够提高线程利用率并减少线程创建开销。
 * 当 Caller 线程加入调度后，并不是 Main Fiber 与 Scheduler Fiber 同时运行，而是 Main Fiber 主动切换到 Scheduler Fiber，之后 Caller 线程便进入调度循环。Scheduler Fiber 负责从任务队列中取出协程并执行，当某个协程被调度运行时，它会独占当前线程的执行权，Scheduler Fiber 暂停执行；只有当协程主动 yield、阻塞（Hook 后）或执行结束时，控制权才会重新回到 Scheduler Fiber，由调度器继续选择下一个协程运行。
 * 因此，一个线程虽然可以拥有多个协程，但任意时刻只能运行一个协程，调度器与普通协程通过上下文切换轮流执行，而不是并行执行，这也是协程属于协作式调度（Cooperative Scheduling）的原因。
 */
class Scheduler
{

public:

    // threads 指定线程池的线程数量，useCaller 指定是否将主线程作为工作线程，name 调度器的名称。
    Scheduler(size_t threads = 1, bool useCaller = true, const std::string &name = "Scheduler");

    // 防止出现资源泄露，基类指针删除派生类对象时不完全销毁的问题。
    virtual ~Scheduler();

    // 获取调度器的名字。
    const std::string &getName() const { return m_name; }


public:

    /// 获取当前线程绑定的调度器对象。如果当前线程没有进入 Scheduler::run()，则返回 nullptr。
    static Scheduler *GetThis();


protected:

    // 设置正在运行的调度器。
    void SetThis();


public:

    // 添加任务到任务队列。FiberOrCb 调度任务类型，可以是协程对象或函数指针。
    // 调度器既支持直接调度协程，也支持调度普通函数。普通函数会在真正执行时包装成协程，这里只保存二者之一。
    template <typename FiberOrCb>
    void scheduleLock(FiberOrCb fc, int thread = -1)
    {
        bool needTickle = false; // 用于标记任务队列是否为空，从而判断是否需要唤醒线程。

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 如果任务队列为空，代表目前所有线程都没有执行任务，都在执行空闲 idle 协程，需要被唤醒。
            needTickle = m_tasks.empty();

            // 创建 Task 的任务对象。
            ScheduleTask task(fc, thread);
            if (task.fiber || task.cb) m_tasks.push_back(task); // 存在就加入。
        }

        // 如果检查出了队列为空，就唤醒线程。
        if (needTickle) tickle();
    }


    // 启动线程池，启动调度器。作用是创建额外的工作线程，并让它们进入 Scheduler::run() 调度循环。
    // 当 useCaller=true 时，Caller 线程本身就是一个工作线程，它会通过调度协程进入 run()，因此 start() 只需要创建剩余的工作线程（如果 threads==1，则无需创建任何新线程）。
    // 当 useCaller=false 时，Caller 线程不参与协程调度，因此所有调度工作都必须由 start() 创建的工作线程完成。如果不创建这些线程，就没有任何线程会进入 run() 执行调度循环。
    virtual void start();

    // 关闭线程池，停止调度器，等所有调度任务都执行完后再返回。
    virtual void stop();


protected:

    // 唤醒线程。
    virtual void tickle();

    // 线程函数。调度器的核心，负责从任务队列中取出任务并通过协程执行。
    virtual void run();

    // 空闲协程函数，无任务调度时执行 idle 协程。
    virtual void idle();

    // 是否可以关闭。
    virtual bool stopping();

    // 返回是否有空闲线程。当调度协程进入 idle 时空闲线程数 +1，从 idle 协程返回时空闲线程数减 1。
    bool hasIdleThreads() { return m_idleThreadCount > 0; }


private:

    // 任务类型。参数可以给一个函数或者一个协程对象指针，这两种参数都接受。
    struct ScheduleTask
    {
        ScheduleTask() { reset(); }

        ScheduleTask(std::shared_ptr<Fiber> f, int thr)
        {
            fiber = f;
            thread = thr;
        }

        ScheduleTask(std::shared_ptr<Fiber> *f, int thr)
        {
            fiber.swap(*f); // 将内容转移也就是指针内部的转移和上面的赋值不同，引用计数不会增加。
            thread = thr;
        }

        ScheduleTask(std::function<void()> f, int thr)
        {
            cb = f;
            thread = thr;
        }

        ScheduleTask(std::function<void()> *f, int thr)
        {
            cb.swap(*f); // 同理。
            thread = thr;
        }

        void reset() // 重置。
        {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }


        std::shared_ptr<Fiber> fiber;

        std::function<void()> cb;

        // 指定任务需要运行的线程 id。
        int thread;
    };

private:

    // 调度器的名称。
    std::string m_name;

    // 互斥锁 -> 保护任务队列。
    std::mutex m_mutex;

    // 线程池，存初始化好的线程。
    std::vector<std::shared_ptr<Thread>> m_threads;

    // 任务队列。
    std::vector<ScheduleTask> m_tasks;

    // 存储工作线程的线程 id。
    std::vector<int> m_threadIds;

    // 需要额外创建的线程数。
    size_t m_threadCount = 0;

    // 活跃线程数。
    std::atomic<size_t> m_activeThreadCount = {0};

    // 空闲线程数。
    std::atomic<size_t> m_idleThreadCount = {0};

    // 主线程是否也参与工作线程，分担任务。
    // useCaller = true：Caller 线程也会成为一个工作线程，它会运行 Scheduler::run()，与其它 Worker 一起调度和执行协程，从而减少创建额外线程的开销，并充分利用已有线程资源。
    // useCaller = false：Caller 线程仅负责创建、启动和停止调度器，不参与协程调度；所有协程都由新创建的 Worker 线程执行。
    bool m_useCaller;

    // 如果是 m_useCaller，需要额外创建的调度协程的指针。
    std::shared_ptr<Fiber> m_schedulerFiber;

    // 如果是 m_useCaller，记录主线程的线程 ID。
    int m_rootThread = -1;

    // 是否正在关闭。这个状态用于告诉所有线程，调度器准备退出了，以后不要一直等待新任务。
    bool m_stopping = false;
};


DFIBER_NAMESPACE_END


#endif
