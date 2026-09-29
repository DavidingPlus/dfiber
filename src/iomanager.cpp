#include "iomanager.h"

#include "hook.h"
#include "config.h"

#include <cassert>
#include <cstring>
#include <exception>

#include <sys/epoll.h>
#include <fcntl.h>


IOManager::FdContext::EventContext &IOManager::FdContext::getEventContext(Event event)
{
    // 判断事件要么是读事件，或者写事件。
    assert(Event::READ == event || Event::WRITE == event);

    switch (event)
    {
        case Event::READ:
            return readEc;
        case Event::WRITE:
            return writeEc;
    }

    throw std::invalid_argument("Unsupported event type");
}

void IOManager::FdContext::resetEventContext(EventContext &ctx)
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(Event event)
{
    // 确保 event 是中有指定的事件，否则程序中断。
    assert(static_cast<int>(events) & static_cast<int>(event));

    // 清理该事件，表示不再关注，也就是说，注册 IO 事件是一次性的，如果想持续关注某个 Socket fd 的读写事件，那么每次触发事件后都要重新添加。
    // Event 使用位标志表示多个事件。0000 NONE，0001 READ，0100 WRITE。因此可以通过按位与 ~static_cast<int>(event) 清除指定 bit。
    events = static_cast<Event>(static_cast<int>(events) & ~static_cast<int>(event));

    // 获取触发事件对应的上下文信息。
    EventContext &ctx = getEventContext(event);
    // 这个过程就相当于 Scheduler 中把真正要执行的函数放入到任务队列中等线程取出后任务后，协程执行，执行完成后返回主协程继续，执行 run() 方法取任务执行任务(不过可能是不同的线程的协程执行了)。
    ctx.cb ? ctx.scheduler->scheduleLock(&ctx.cb) : ctx.scheduler->scheduleLock(&ctx.fiber);

    // 重置事件上下文。
    resetEventContext(ctx);
}

IOManager::IOManager(size_t threads, bool useCaller, const std::string &name)
    : Scheduler(threads, useCaller, name), TimerManager()
{
    // 创建 epoll fd。成功返回大于 0 的文件描述符，错误返回 -1。
    // 5000，epoll_create 的参数实际上在现代 Linux 内核中已经被忽略，最早版本的 Linux 中，这个参数用于指定 epoll 内部使用的事件表的大小。
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // 管道 pipe 通常用于进程间通信，但由于线程共享进程的文件描述符表，因此同一进程内的多个线程也可以通过 pipe 通信。IOManager 使用 pipe 并不是为了传递业务数据，而是创建一个可以被 epoll 监听的文件描述符。
    // IOManager 的工作线程通常阻塞在 epoll_wait() 中等待 IO 事件。当其他线程向调度器添加任务时，如果没有唤醒机制，工作线程会一直停留在 epoll_wait()，无法及时执行新加入的任务。因此需要一种能够让 epoll_wait() 主动返回的通知机制。
    // 之所以选择 pipe，是因为 Linux 的 epoll 只能监听文件描述符，而 pipe 本身就是内核提供的 fd 对象。当其他线程向 pipe 写端写入数据时，pipe 读端会产生可读事件，epoll_wait() 检测到该事件后返回，从而唤醒阻塞中的 IOManager 工作线程。
    // pipe 中传递的数据没有业务含义，仅表示“有新的任务或者状态变化，需要重新检查调度队列”。本质上是将线程间通知转换成 epoll 可以感知的 IO 事件。
    //
    // 为什么不使用 mutex、条件变量等传统线程同步机制？
    // 1. mutex（互斥锁）：
    //    - mutex 只能保证多个线程访问共享数据时的互斥性，不能主动唤醒阻塞在 epoll_wait() 中的线程。
    //    - 如果线程正在执行 epoll_wait()，另一个线程释放锁并不会让 epoll_wait() 返回。
    //    - 因此 mutex 适合保护任务队列，而不适合作为 IO 事件循环的唤醒机制。
    // 2. condition_variable（条件变量）：
    //    - 条件变量可以实现线程间等待和唤醒，例如 notify_one() 唤醒 wait()。
    //    - 但是条件变量只能唤醒阻塞在 condition_variable::wait() 上的线程，无法唤醒阻塞在 epoll_wait() 上的线程。
    //    - IOManager 的线程睡眠点是 epoll_wait()，因此条件变量无法直接使用。
    // 3. sleep / 轮询检查：
    //    - 可以通过定时检查任务队列的方式发现新任务，但会造成延迟或者 CPU 空转。
    //    - 不符合高性能事件驱动模型的设计。
    // 4. pipe、eventfd、socketpair：
    //    - 这些机制本身都是文件描述符，可以直接注册到 epoll 中。
    //    - 当事件发生时，epoll_wait() 能够感知并返回，因此适合作为事件循环的唤醒源。
    // 其中 eventfd 是 Linux 专门为事件通知设计的 fd，性能通常优于 pipe；socketpair 支持双向通信；pipe 实现简单、兼容性好，因此很多早期事件框架会采用 pipe。

    // 创建管道 pipe。创建管道的函数规定了 m_tickleFds[0] 是读端，1 是写段。成功返回 0，错误返回 -1。
    int res = pipe(m_tickleFds);
    assert(!res);

    // 将管道的监听注册到 epoll 上。
    epoll_event event;

    // epoll 的触发模式分为水平触发（LT）和边缘触发（ET）：
    // 1. 水平触发（Level Trigger，LT）：
    //    - 关注 fd 当前是否满足条件。
    //    - 只要缓冲区中仍然存在可读数据，epoll 就会持续通知。
    //    - 例如：缓冲区有 100 字节数据，第一次读取 50 字节后仍剩余 50 字节，epoll 仍会继续触发读事件。
    //    - 优点：编程简单，不容易遗漏事件。
    //    - 缺点：可能产生大量重复通知，降低高并发场景下的效率。
    // 2. 边缘触发（Edge Trigger，ET）：
    //    - 关注 fd 状态是否发生变化，只在状态变化时通知一次。
    //    - 例如：缓冲区从 0 字节变为 100 字节时触发一次，读取 50 字节后剩余 50 字节不会再次通知。
    //    - 优点：减少 epoll 通知次数，提高 IO 处理效率。
    //    - 缺点：要求应用程序一次事件内必须尽可能处理完所有数据，否则剩余数据可能不会再次触发事件。
    // 3. ET 模式通常需要配合非阻塞 IO：
    //    - 因为 ET 模式要求循环读取数据，一次把数据读取完，直到 read/write 返回 EAGAIN，表示当前数据已经处理完成。
    //    - 如果 fd 是阻塞模式，当数据读取完后继续调用 read/write，会因为没有数据而阻塞线程。
    //    - 设置非阻塞后，数据处理完成时可以立即返回 EAGAIN，让程序退出当前事件处理，重新进入 epoll_wait 等待下一次事件。
    // 高性能网络模型通常采用：非阻塞 fd + epoll ET + 事件循环。epoll_wait 负责等待事件发生，非阻塞 IO 负责保证事件处理过程不会因为数据不足而阻塞。

    // 设置标志位，并且采用读事件 EPOLLIN 和边缘触发 EPOLLET。
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = m_tickleFds[0];

    // fcntl 是 Linux 提供的文件描述符控制函数，用于修改或获取 fd 的属性。
    // 常用操作包括：
    // 1. F_GETFD / F_SETFD：file descriptor flags
    //    - 文件描述符本身的标志。
    //    - 例如 FD_CLOEXEC，表示进程执行 exec 系列函数时是否自动关闭该 fd。
    // 2. F_GETFL / F_SETFL：file status flags
    //    - 对应的文件状态标志。
    //    - 常见标志包括：
    //      O_NONBLOCK：设置非阻塞模式。
    //      O_APPEND：写文件时追加到文件末尾。
    //      O_ASYNC：启用异步 IO 信号通知。

    // 修改管道文件描述符以非阻塞的方式，配合边缘触发。
    {
        HookEnableGuard guard(false);
        res = fcntl(m_tickleFds[0], F_SETFL, fcntl(m_tickleFds[0], F_GETFL) | O_NONBLOCK);
        assert(!res);
    }

    // 将 m_tickleFds[0] 作为读事件放入到 event 监听集合中。
    res = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!res);

    // 初始化了一个包含 32 个文件描述符上下文的数组。
    contextResize(32);

    // 启动 Scheduler，开启线程池，准备处理任务。
    start();
}

IOManager::~IOManager()
{
    // 关闭 Scheduler 类中的线程池，让任务全部执行完后线程安全退出。
    stop();

    {
        HookEnableGuard guard(false);

        // 关闭 epoll 的句柄。
        close(m_epfd);

        // 关闭管道读端写端。
        close(m_tickleFds[0]);
        close(m_tickleFds[1]);
    }

    // 将 fdcontext 文件描述符一个个关闭。
    for (size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if (m_fdContexts[i]) delete m_fdContexts[i];
    }
}

// addEvent、delEvent、cancelEvent、cancelAll 这几个函数的代码架子是一样的，仅有细节不同。可对比理解。
int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
{
    // 查找 FdContext 对象。
    FdContext *fdCtx = nullptr;

    // 1. 如果说传入的 fd 在数组里面，查找然后初始化 FdContext 的对象。因为没有修改类的内部成员，因此使用读锁。
    std::shared_lock<std::shared_mutex> readLock(m_mutex);
    if (fd < static_cast<int>(m_fdContexts.size()))
    {
        fdCtx = m_fdContexts[fd];

        readLock.unlock();
    }
    // 2. 不存在则重新分配数组的 size 来初始化 FdContext 的对象。因为修改了 m_fdContexts，因此使用写锁。
    else
    {
        readLock.unlock();
        std::unique_lock<std::shared_mutex> writeLock(m_mutex);

        contextResize(fd * 3 / 2 + 1);
        fdCtx = m_fdContexts[fd];
    }

    // 找到或者创建 FdContext 的对象后，为 FdContext 加上互斥锁，确保 FdContext 的状态不会被其他线程修改。
    // 注意：这里的锁是 FdContext 上下文对象自身的，和前面 IOManager 的读写锁不同，注意区分。
    std::lock_guard<std::mutex> lock(fdCtx->mutex);

    // 判断要添加的事件是否已经存在了？是就返回 -1，因为相同的事件不能重复添加。
    if (static_cast<int>(fdCtx->events) & static_cast<int>(event)) return -1;

    // epfd 第一次注册事件，需要 EPOLL_CTL_ADD；如果已经注册了其他事件（例如 READ），再添加 WRITE 时使用 EPOLL_CTL_MOD 修改监听事件。
    int op = static_cast<bool>(fdCtx->events) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    // 我们认为设计 Event 的取值与 EPOLLIN/EPOLLOUT 保持一致，因此可以直接作为 epoll_event.events 的位标志，可以使用按位或运算。
    epevent.events = EPOLLET | static_cast<int>(fdCtx->events) | static_cast<int>(event);
    // epoll_event 的 data 成员是一个用户自定义数据区，内核不会解析其中的内容，而是在事件触发时原样返回。这里将当前 fd 对应的 FdContext 指针保存到 data.ptr 中。当 epoll_wait() 返回事件时，可以直接通过 event.data.ptr 取回对应的 FdContext，从而快速获得该 fd 的读写事件、回调函数、协程以及调度器等信息，无需再根据 fd 去查找。相比只保存 fd（event.data.fd），保存 FdContext* 可以避免一次额外的查表操作，也是 libevent、libuv、muduo 等事件驱动框架的常见做法。
    epevent.data.ptr = fdCtx;

    // 将事件添加到 epoll 中。如果添加失败，打印错误信息并返回 -1。epoll_ctl() 成功返回 0，失败返回 -1。
    if (epoll_ctl(m_epfd, op, fd, &epevent))
    {
        std::cerr << "addEvent::epoll_ctl() failed: " << strerror(errno) << '\n';


        return -1;
    }

    // 原子计数器，待处理的事件++；
    ++m_pendingEventCount;

    // 更新 FdContext 的 events 成员，记录当前的所有事件。注意 events 可以监听读和写的组合，如果 fdCtx->events 为 none,就相当于直接是 fdCtx->events = event
    fdCtx->events = static_cast<Event>(static_cast<int>(fdCtx->events) | static_cast<int>(event));

    // 设置 FdContext 中的事件上下文。
    FdContext::EventContext &eventCtx = fdCtx->getEventContext(event);
    // 确保 EventContext 中没有其他正在执行的调度器、协程或回调函数，也就是都为空。
    assert(!eventCtx.scheduler && !eventCtx.fiber && !eventCtx.cb);
    // 设置调度器为当前的调度器实例（Scheduler::GetThis()）。FdContext 本身只负责记录 fd 对应的读写事件及其回调信息，并不知道事件触发后应该交给哪个调度器执行。因此在注册事件时，需要记录当前线程正在运行的 Scheduler（通过 Scheduler::GetThis() 获取）。当 epoll_wait() 检测到该事件发生后，triggerEvent() 会通过保存的 scheduler 将对应的协程或回调重新加入调度队列，从而由正确的调度器负责恢复协程或执行回调。
    // 为什么不直接使用 this？因为这里需要保存的是"当前线程实际运行的调度器"这一概念，而不是单纯保存当前 IOManager 对象。对于 IOManager 而言，this 和 Scheduler::GetThis() 在正常情况下通常指向同一个对象（IOManager 继承自 Scheduler），直接使用 this 也能够正常工作。但整个框架统一通过 Scheduler::GetThis() 获取当前线程绑定的调度器，更符合调度器的设计思想，也避免代码依赖具体的对象指针，保持接口风格一致。
    // eventCtx.scheduler = dynamic_cast<Scheduler *>(this);
    eventCtx.scheduler = Scheduler::GetThis();

    // 如果提供了回调函数 cb，则将其保存到 EventContext 中；否则，将当前正在运行的协程保存到 EventContext 中，并确保协程的状态是正在运行。
    if (cb)
    {
        eventCtx.cb.swap(cb);
    }
    else
    {
        // 需要确保存在主协程。
        eventCtx.fiber = Fiber::GetThis();
        assert(Fiber::RUNNING == eventCtx.fiber->getState());
    }


    return 0;
}

bool IOManager::delEvent(int fd, Event event)
{
    FdContext *fdCtx = nullptr;

    std::shared_lock<std::shared_mutex> readLock(m_mutex);
    if (fd < static_cast<int>(m_fdContexts.size()))
    {
        fdCtx = m_fdContexts[fd];

        readLock.unlock();
    }
    else
    {
        readLock.unlock();

        // 找不到指定的 fd 也就无法删除，直接返回。
        return false;
    }

    std::lock_guard<std::mutex> lock(fdCtx->mutex);

    // 如果要删除的事件不相同，返回 false，否则就继续。
    if (!(static_cast<int>(fdCtx->events) & static_cast<int>(event))) return false;

    // 因为这里要删除事件，对原有的事件状态取反就是删除原有的状态。
    Event newEvents = static_cast<Event>(static_cast<int>(fdCtx->events) & ~static_cast<int>(event));
    // 删除了这一个事件可能还有其他事件，因此需要判断 newEvents 的状态来决定是修改 EPOLL_CTL_MOD 还是删除 EPOLL_CTL_DEL。
    int op = static_cast<bool>(newEvents) ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;

    epoll_event epevent;
    epevent.events = EPOLLET | static_cast<int>(newEvents);
    epevent.data.ptr = fdCtx;

    if (epoll_ctl(m_epfd, op, fd, &epevent))
    {
        std::cerr << "delEvent::epoll_ctl() failed: " << strerror(errno) << '\n';


        return false;
    }

    // 减少了待处理的事件。
    --m_pendingEventCount;

    // 更新 fdCtx->events。
    fdCtx->events = newEvents;

    // 重置上下文。
    FdContext::EventContext &eventCtx = fdCtx->getEventContext(event);
    fdCtx->resetEventContext(eventCtx);


    return true;
}

bool IOManager::cancelEvent(int fd, Event event)
{
    FdContext *fdCtx = nullptr;

    std::shared_lock<std::shared_mutex> readLock(m_mutex);
    if (fd < static_cast<int>(m_fdContexts.size()))
    {
        fdCtx = m_fdContexts[fd];

        readLock.unlock();
    }
    else
    {
        readLock.unlock();


        return false;
    }

    std::lock_guard<std::mutex> lock(fdCtx->mutex);

    if (!(static_cast<int>(fdCtx->events) & static_cast<int>(event))) return false;

    Event newEvents = static_cast<Event>(static_cast<int>(fdCtx->events) & ~static_cast<int>(event));
    int op = static_cast<bool>(newEvents) ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;

    epoll_event epevent;
    epevent.events = EPOLLET | static_cast<int>(newEvents);
    epevent.data.ptr = fdCtx;

    if (epoll_ctl(m_epfd, op, fd, &epevent))
    {
        std::cerr << "cancelEvent::epoll_ctl() failed: " << strerror(errno) << '\n';


        return false;
    }

    --m_pendingEventCount;

    // 这里不需要对 fdCtx->events 赋值，因为 triggerEvent() 函数会自动清理要触发的事件，提前赋值反而会导致 triggerEvent() 中的断言失败。
    // fdCtx->events = newEvents;

    // 触发事件对应的回调任务。
    fdCtx->triggerEvent(event);


    return true;
}

bool IOManager::cancelAll(int fd)
{
    FdContext *fdCtx = nullptr;

    std::shared_lock<std::shared_mutex> readLock(m_mutex);
    if (fd < static_cast<int>(m_fdContexts.size()))
    {
        fdCtx = m_fdContexts[fd];

        readLock.unlock();
    }
    else
    {
        readLock.unlock();


        return false;
    }

    std::lock_guard<std::mutex> lock(fdCtx->mutex);

    if (!static_cast<int>(fdCtx->events)) return false;

    // 删除所有的事件。
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fdCtx;

    if (epoll_ctl(m_epfd, op, fd, &epevent))
    {
        std::cerr << "cancelAll::epoll_ctl() failed: " << strerror(errno) << '\n';


        return false;
    }

    // 根据 fd 当前注册的事件，依次触发对应事件。triggerEvent() 会负责清除 FdContext 中对应的事件状态，并将等待该事件的协程或回调函数重新加入调度队列。
    Event oldEvents = fdCtx->events;
    if (static_cast<int>(oldEvents) & static_cast<int>(Event::READ))
    {
        fdCtx->triggerEvent(Event::READ);

        --m_pendingEventCount;
    }
    if (static_cast<int>(oldEvents) & static_cast<int>(Event::WRITE))
    {
        fdCtx->triggerEvent(Event::WRITE);

        --m_pendingEventCount;
    }

    assert(Event::NONE == fdCtx->events);


    return true;
}

void IOManager::tickle()
{
    // 检查当前是否有线程处于空闲状态。如果没有空闲线程，函数直接返回，不执行后续操作。
    if (!hasIdleThreads()) return;

    // 如果有空闲线程，函数会向管道 m_tickleFds[1] 写入一个字符 "T"。这个写操作的目的是向等待在 m_tickleFds[0]（管道的另一端）的线程发送一个信号，通知它有新任务可以处理了。
    {
        HookEnableGuard guard(false);
        int res = write(m_tickleFds[1], "T", 1);
        assert(1 == res);
    }
}

bool IOManager::stopping()
{
    // 重写了 Scheduler 的 stopping()。具体来说，它会检查定时器、挂起事件以及调度器状态，以决定是否可以安全地停止运行。需要注意的是：Scheduler 也有一个 stopping() 函数，但是因为重写的原因实际业务中真正执行的是 IOmanager 的函数，Scheduler 的 stopping() 函数只是对 Scheduler 类需要确保任务数量等于 0，还有是否需要终止的成员变量，活跃线程总和是否为 0 做一个判断。
    // 没有定时器超时，待处理的事件数量为 0 并且 Scheduler::stopping()。
    return ~0ull == getNextTimer() && 0 == m_pendingEventCount && Scheduler::stopping();
}

void IOManager::idle()
{
    // 1. 初始化事件存储空间。定义 epoll_wait 单次能处理的最大事件数 MAX_EVENTS（通常设为 256）。利用 std:unique_ptr<epoll_event[]> 在堆上动态分配内存，用于存储就绪事件数组，确保资源在函数退出时能自动释放。

    // 定义了 epoll_wait 能同时处理的最大事件数。
    static constexpr uint64_t MAX_EVENTS = 256;
    // 使用 std::unique_ptr 动态分配了一个大小为 MAX_EVENTS 的 epoll_event 数组，用于存储从 epoll_wait 获取的事件。
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVENTS]);

    // 2. 进入主循环与阻塞监听。整个逻辑运行在一个 while(true) 循环中。首先检查 stopping() 状态以决定是否退出。随后进入 epoll_wait() 阻塞调用。注意其超时机制：通过 getNextTimer() 获取定时器堆中最近的超时剩余时间，并与系统默认的 MAX_TIMEOUT（如 5000 ms）取最小值，作为 epoll_wait 的超时参数。这保证了定时器能准时触发。

    while (true)
    {
        if (DFIBER_CONFIG_DEBUG) std::cout << "IOManager::idle(), run in thread: " << Thread::GetThreadId() << std::endl;

        if (stopping())
        {
            if (DFIBER_CONFIG_DEBUG) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;

            break;
        }

        // 阻塞于 epoll_wait。使用循环是因为有信号中断的情况，需要重试，正常返回代表后续有事件需要处理。
        int res = 0;
        while (true)
        {
            // 定义了最大超时时间为 5000 毫秒。
            static constexpr uint64_t MAX_TIMEOUT = 5000;
            // 获取下一个超时的定时器。
            uint64_t nextTimeout = getNextTimer();
            // 获取下一个定时器的超时时间，并将其与 MAX_TIMEOUT 取较小值，避免等待时间过长。
            nextTimeout = std::min(nextTimeout, MAX_TIMEOUT);

            // epoll_wait 陷入阻塞，等待 tickle 信号的唤醒，并且使用了定时器堆中最早超时的定时器作为 epoll_wait 超时时间。
            res = epoll_wait(m_epfd, events.get(), MAX_EVENTS, (int)nextTimeout);
            // res 小于 0 代表失败，如果 errno 是 EINTR（表示信号中断），重试 epoll_wait()。
            if (res < 0 && EINTR == errno)
            {
                continue;
            }
            else
            {
                break;
            }
        }

        // 3. 处理超时定时器事件。当 epoll_wait 返回后（无论是超时还是事件触发），首先调用 listExpiredCb(cbs)。该函数会收集所有已到期的定时器回调，并将它们一次性推入调度器的任务队列中，等待后续执行。

        // epoll_wait 返回后检查定时器。无论 epoll 是因为 IO 事件触发、tickle 唤醒还是超时返回，都需要检查 TimerManager 中是否存在已经到期的定时器。
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);
        if (!cbs.empty())
        {
            for (const auto &cb : cbs) scheduleLock(cb);

            cbs.clear();
        }

        // 4. 处理 IO 事件。遍历 epoll_wait 返回的就绪事件数组，首先处理 Tickle 唤醒信号。若发现就绪的是 m_tickleFds[0]（管道读端），说明有其他线程通过 tickle() 唤醒了当前线程。此时通过一个 while 循环将管道中的数据彻底读完（直到返回 -1 且 errno 为 EAGAIN），从而清除唤醒标志。

        // epoll_wait() 返回准备好的事件个数。
        for (int i = 0; i < res; ++i)
        {
            // 获取第 i 个 epoll_event，用于处理该事件。
            epoll_event &event = events[i];

            // 检查当前事件是否是 tickle 事件（即用于唤醒空闲线程的事件）。
            if (event.data.fd == m_tickleFds[0])
            {
                uint8_t dummy[256];
                // 清空管道中的所有唤醒数据。由于 epoll 使用 EPOLLET 边缘触发模式，必须读取到管道为空，否则剩余数据不会再次触发 epoll 事件。
                {
                    HookEnableGuard guard(false);
                    while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0)
                        ;
                }

                // 继续处理其他 IO 事件。
                continue;
            }

            // 5. 事件映射与归类。对于其他的 IO 就绪事件，通过 event.data.ptr 获取绑定的文件描述符上下文 fdCtx。由于抽象层只对外暴露 READ 和 WRITE 事件，因此需要对 epoll 的原生事件进行转换：如果发生 EPOLLERR（错误）或 EPOLLHUP（挂起），将其映射为该 fd 已注册的 READ 或 WRITE 事件。这样可以确保即使发生错误，相关的协程也能被唤醒去处理异常（例如执行 read 并发现返回 0 或错误）。

            // 其他事件，通过 event.data.ptr 获取与当前事件关联的 FdContext 指针 fdCtx，该指针包含了与文件描述符相关的上下文信息。
            FdContext *fdCtx = reinterpret_cast<FdContext *>(event.data.ptr);
            std::lock_guard<std::mutex> lock(fdCtx->mutex);

            // 如果当前事件是错误或挂起（EPOLLERR 或 EPOLLHUP），则将其转换为可读或可写事件（EPOLLIN 或 EPOLLOUT），以便后续处理。
            // epoll 在发生错误（EPOLLERR）或者对端关闭连接（EPOLLHUP）时，不一定返回 EPOLLIN/EPOLLOUT，但对于协程调度来说，等待该 fd 的读写操作仍然需要被唤醒，否则对应 Fiber 可能永久阻塞。如果遇到错误/挂起事件，将其转换为用户注册过的读写事件，代表仍需处理它们。只转换 fd 当前关注的事件，避免错误地触发没有注册的读写事件。
            // 例如：
            // 1. 如果 Fiber 正在等待 READ，而 socket 被关闭，则转换为 EPOLLIN，让 read 协程被唤醒，并由 read 返回 0 或错误处理关闭情况。
            // 2. 如果 Fiber 正在等待 WRITE，则转换为 EPOLLOUT。
            if (event.events & (EPOLLERR | EPOLLHUP)) event.events |= (EPOLLIN | EPOLLOUT) & static_cast<int>(fdCtx->events);

            // 确定实际发生的事件类型（读取、写入或两者都有）。
            int realEvents = static_cast<int>(Event::NONE);
            if (event.events & EPOLLIN)
            {
                realEvents |= static_cast<int>(Event::READ);
            }
            if (event.events & EPOLLOUT)
            {
                realEvents |= static_cast<int>(Event::WRITE);
            }
            // 检查当前 epoll 返回的实际触发事件 realEvents 是否包含 fdCtx 注册关注的事件。fdCtx->events 保存了用户通过 addEvent 注册的 READ/WRITE 事件，两者进行按位与操作可以判断当前事件是否仍然有效。如果没有交集，说明该事件已经被取消或不再被关注，忽略该事件并继续处理下一个。
            if (static_cast<int>(Event::NONE) == (static_cast<int>(fdCtx->events) & realEvents))
            {
                continue;
            }

            // 6. 更新状态与触发调度。根据实际发生的 realEvents（读、写或两者组合），计算该 fd 剩余的关注事件 leftEvents。根据是否还有剩余事件，调用 epoll_ctl() 执行 MOD（修改）或 DEL（删除）操作。调用 triggerEvent()：将触发的读/写回调函数或协程推入任务队列。这一步是 IO 任务转为普通调度任务的关键。

            // 计算当前 fd 剩余需要监听的事件。使用按位取反和按位与操作，移除已经触发的事件，保留尚未触发的事件。后续根据 leftEvents 是否为空，决定继续使用 EPOLL_CTL_MOD 修改监听，或使用 EPOLL_CTL_DEL 删除该 fd 的 epoll 事件。
            int leftEvents = static_cast<int>(fdCtx->events) & ~static_cast<int>(realEvents);
            int op = leftEvents ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            // 构造新的 epoll 事件掩码。EPOLLET 表示继续使用边缘触发模式。
            event.events = leftEvents | EPOLLET;

            // 根据之前计算的操作（op），调用 epoll_ctl 更新或删除 epoll 监听，如果失败，打印错误并继续处理下一个事件。
            res = epoll_ctl(m_epfd, op, fdCtx->fd, &event);
            if (res)
            {
                std::cerr << "idle::epoll_ctl() failed: " << strerror(errno) << std::endl;

                continue;
            }

            // 调用 triggerEvent() 触发事件。
            if (realEvents & static_cast<int>(Event::READ))
            {
                fdCtx->triggerEvent(Event::READ);
                --m_pendingEventCount;
            }
            if (realEvents & static_cast<int>(Event::WRITE))
            {
                fdCtx->triggerEvent(Event::WRITE);
                --m_pendingEventCount;
            }
        }

        // 7. 协程让出（Yield）。在处理完本次所有的就绪事件后，idle 协程主动调用 yield() 让出 CPU 执行权。此时，调度器会立即切入刚才由定时器或 IO 事件产生的新任务。等到所有任务再次执行完毕，调度器会重新回到 idle 协程开始下一轮监听。

        // 当前线程的协程主动让出控制权，调度器可以选择执行其他任务或再次进入 idle 状态。
        Fiber::GetThis()->yield();
    }
}

void IOManager::contextResize(size_t size)
{
    // 调整 m_fdContexts 的大小。
    m_fdContexts.resize(size);

    //  遍历 m_fdContexts 向量，初始化尚未初始化的 FdContext 对象。
    for (size_t i = 0; i < m_fdContexts.size(); ++i)
    {
        if (!m_fdContexts[i])
        {
            m_fdContexts[i] = new FdContext();

            // 将文件描述符的编号 i 赋值给 fd。
            m_fdContexts[i]->fd = i;
        }
    }
}
