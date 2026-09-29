#ifndef _DFIBER_IOMANAGER_H_
#define _DFIBER_IOMANAGER_H_

#include "scheduler.h"
#include "timer.h"


// workflow：1 注册事件 -> 2 等待事件 -> 3 事件触发调度回调 -> 4 注销事件回调后从 epoll 注销 -> 5 执行回调进入调度器中执行调度。
// Scheduler 调度器的基本思想是协程作为任务添加到任务调度器就调度了，很简单的放入就执行的先到先服务的逻辑。但是服务器中需要处理大量的 socketfd，比如连接，对应的读写事件处理，所以需要使用到 IO 多路复用来管理这些大量的 fd，所以设计 IO +协程调度器，目的就是让协程参与到其中，因为虽然 IO 多路复用处理的不需要阻塞等待数据，比如执行 read(系统调用的时候看到是有读的数据才进行处理，如果现在的read的执行过程中想做其他的事情如果没有协程的话显然是不行的，所以我们引入 IO +协程即解决了因资源阻塞的问题，又解决了运行函数的处理复杂时可以处理简单的任务，增加了灵活性和并发效率。
// IO 协程调度可以看成是增强版的协程调度。在前面的协程调度模块中，调度器对协程的调度是无条件执行的，在调度器已经启动调度的情况下，任务一旦添加成功，就会排队等待调度器执行。调度器不支持删除调度任务，并且调度器在正常退出之前一定会执行完全部的调度任务，所以某种程度上可以认为，把一个协程添加到调度器的任务队列，就相当于调用了协程的 resume 方法。
// IO 协程调度支持协程调度的全部功能，因为 IO 协程调度器是直接继承协程协程调度器实现的。除了协程调度，IO 协程调度还增加了 IO 事件调度的功能，这个功能针对描述符（一般是套接字描述符）的。IO 协程调度支持为描述符注册可读和可写事件，当描述符可读或可写时，执行对应的回调函数。（可以把回调函数等效为协程因为实际用还是要转换成协程。）
// 事实上很多的库都可以实现类似的工作，比如 libevent，libuv，libev 等，这些库被称为异步事件库或 IO 库。有的库不仅可以处理 socketfd 事件，还可以处理定时器事件和信号事件。这些事件库的实现原理基本类似，都是先将套接字设置为非阻塞状态，然后将套接字与回调函数绑定，接下来进入一个基于 IO 多路复用的事件循环，等待事件发生，然后调用对应的回调函数。
class IOManager : public Scheduler, public TimerManager
{

public:

    enum class Event
    {
        NONE = 0x0,      // 表示没有事件。
        READ = 0x1,      // READ == EPOLLIN，表示读事件，对应于 epoll 的 EPOLLIN 事件。
        WRITE = 0x1 << 2 // WRITE == EPOLLOUT，表示写事件，对应于 epoll 的 EPOLLOUT 事件。
    };


private:

    // 用于描述一个文件描述的事件上下文。每个 socket fd 都对应一个 FdContext，包括 fd 的值，fd 上的事件，以及 fd 的读写事件上下文。
    // 对于 IO 协程调度，每次调度都包含一个三元组信息，分别是描述符、事件类型（可读可写事件）、回调函数，调度器记录全部需要调度的三元组信息，其中描述符和事件类型用于 epoll_wait，回调函数用于协程调度。这三元组信息在源码上通过 Fdcontext 结构体来存储，在执行 epoll_wait 时通过 epoll_event 的私有数据指针 data.ptr 来保存 Fdcontext 的结构体信息。
    struct FdContext
    {
        // 描述一个具体事件的上下文，如读事件或写事件。
        // Eventcontext 是为了在 fd 触发了 event 事件（读或写或读写组合）的时候具体让执行的系统调用如 read()，write()，send() 作为协程的入口函数进行任务的调度，目的是提高 IO 等待 read()，write() 数据较多的时候可以先解决小任务，增加灵活性和提高效率。所以 Eventcontext 有函数对象和协程，协程调度器，目的是为了在触发事件后找到对应的调度器对象去运行协程或者函数对象绑定的入口函数（read()，write()）。
        struct EventContext
        {
            // 关联的调度器。
            Scheduler *scheduler = nullptr;

            // 关联的回调协程。
            std::shared_ptr<Fiber> fiber;

            // 关联的回调函数，最后都会注册为协程对象。
            std::function<void()> cb;
        };


        // 根据事件类型获取相应的事件上下文（如读事件上下文或写事件上下文）。
        EventContext &getEventContext(Event event);

        // 重置 EventContext 事件的上下文，将其恢复到初始或者空的状态。主要作用是清理并重置传入的 Event context 对象，使其不再与任何调度器、线程或回调函数相关联。
        void resetEventContext(EventContext &ctx);

        // 触发事件，负责在指定的 IO 事件被触发时，执行相应的回调函数或线程，并且在执行完之后清理相关的事件上下文。
        void triggerEvent(Event event);


        // 读事件的上下文。
        EventContext readEc;

        // 写事件的上下文。
        EventContext writeEc;

        // 事件关联的 fd（句柄）。
        int fd = 0;

        // 当前注册的事件，可能是 READ、WRITE 或二者的组合。
        Event events = Event::NONE;

        std::mutex mutex;
    };


public:

    // 获取当前线程的调度器对象，然后将其动态转换为 IOManager* 类型，如果转换成功，表示当前线程的调度器对象确实是一个 IOManager 对象。否则，如果是转化的是指针类型返回 nullptr，引用类型抛出 std::bad_cast 异常。
    static IOManager *GetThis() { return dynamic_cast<IOManager *>(Scheduler::GetThis()); }


public:

    // 允许设置 threads 线程数量，useCaller 是否讲主线程或调度线程包含进行，name 调度器的名字。
    // TODO 注意：默认不使用 (1, true)。当前实现中，useCaller == true 时，主线程不会在构造函数中立即进入调度循环，而是在 stop() 中切换到调度协程执行 Scheduler::run()。这是因为构造函数返回后，调用者通常还需要继续执行后续初始化代码，不能直接将主线程切换到调度协程。因此，当 threads == 1 && useCaller == true 时，系统中没有任何后台调度线程，而主线程又尚未进入调度循环，导致 schedule() 或 addEvent() 添加的任务无法被执行。如果仅依赖析构函数调用 stop()，则所有任务都会推迟到对象销毁阶段才开始执行，无法满足正常的 IO 调度需求，因此当前实现默认不采用 (1, true)。
    // 这个设计和 Scheduler 里面的那个是对应的，都是同一个问题。
    IOManager(size_t threads = 1, bool useCaller = false, const std::string &name = "IOManager");

    virtual ~IOManager();

    // 为一个 contextresize() 分配好的 fd，添加一个 event 事件，并在事件触发时执行指定的回调函数（cb）或回调协程。具体的触发是在 triggerEvent()。
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

    // 删除文件描述符 fd 上的某个事件，但是不触发回调函数。
    bool delEvent(int fd, Event event);

    // 取消文件描述符 fd 上的某个事件，并触发其回调函数。
    // IO 协程调度器支持取消事件。取消事件表示不关心某个 fd 的某个事件了，如果某个 fd 的可读或可写事件都被取消了，那这个 fd 会从调度器的 epoll_wait 中删除。支持使用 IO 事件调度，针对套接字描述符，将描述符注册可读和可写事件的回调函数，当事件触发时执行对应的回调函数。针对定时器，处理定时的任务主要是处理 sleep、usleep 等。
    // 相比 delEvent() 不同在于删除事件后，还需要将删除的事件直接交给 trigger() 函数放入到协程调度器中进行触发。
    bool cancelEvent(int fd, Event event);

    // 取消文件描述符 fd 上的所有事件，并触发所有回调函数。
    bool cancelAll(int fd);


protected:

    // 通知调度器有任务调度。写 pipe 让 idle 协程从 epoll_wait 退出，待 idle 协程 yield 之后 Scheduler::run 就可以调度其他任务。重写了 Scheduler 的 tickle()，作用是检测到有空闲线程时，通过写入一个字符到管道（m_tickleFds[1]）中，唤醒那些等待任务的线程。
    void tickle() override;

    // 判断调度器是否可以停止。判断条件是 Scheduler::stopping() 加 IOManager 的 m_pendingEventCount 为 0 以及没有定时器超时（~0ull == getNextTimer()），表示没有 IO 事件可调度。
    bool stopping() override;

    // 实际是 idle 协程只负责收集所有已触发的 fd 的回调函数并将其加⼊调度器的任务队列，真正的执⾏时机是 idle 协程退出后，调度器在下⼀轮调度时执⾏。这里也是 scheduler 的重写，当没有事件处理时，线程处于空闲状态时的处理逻辑。
    // IO 协程调度器在 idle 时会 epoll_wait 所有注册的 fd，如果有 fd 满足条件，epoll_wait 返回，从私有数据中拿到 fd 的上下文信息，并且将其加入调度器的任务队列。实际是 idle 协程只负责收集所有已触发的 fd 的回调函数并将其加入调度器的任务队列，真正的执行时机是 idle 协程退出后，调度器在下一轮调度时执行。
    // 通常在没有任务处理时运行（或者即使当前没有任务处理，线程也会在idle()）中持续休眠并等待新的任务。保证了在所有任务完成之前调度器不会退出，等待和处理 I/O 事件。
    // idle() 是 IOManager 的核心调度循环。idle 协程在线程没有可执行任务时被调度运行，负责监听外部异步事件，并将其转换为普通调度任务。
    //
    // 主要处理两类事件：
    // 1. IO 事件：通过 epoll_wait 监听文件描述符的 READ/WRITE 就绪状态。当 IO 事件触发时，通过 triggerEvent() 唤醒等待该事件的 Fiber，并将其重新加入调度队列。
    // 2. 定时器事件：根据最近一次定时器到期时间设置 epoll_wait 超时时间。当等待超时或 IO 事件返回后，通过 listExpiredCb() 获取已到期的定时器回调，并加入调度队列执行。
    //
    // 此外，idle 协程还负责处理 tickle 唤醒信号，使其他线程能够唤醒阻塞在 epoll_wait 中的线程。处理完当前批次事件后，idle 协程主动让出 CPU，将执行权交还给 Scheduler，等待下一轮事件监听。
    //
    // epoll 监听(IO 事件/定时器超时/tickle 唤醒)
    //              |
    //              v
    //      IOManager::idle()
    //              |
    //              v
    //   转换为 Scheduler 可执行任务
    //              |
    //              v
    //       Fiber 被调度执行
    void idle() override;

    // 重写当有新的定时器插入到前面时的处理逻辑。函数的作用是在定时器被插入到最前面时，触发 tickle 事件，唤醒阻塞的 epoll_wait 回收超时的定时任务（回调 cb 和协程）放入协程调度器中等待调度。
    void onTimerInsertedAtFront() override { tickle(); }

    // 调整 m_fdContexts 数组的大小，并为新的文件描述符 fd 创建并初始化相应的 Fdcontext 对象。
    void contextResize(size_t size);


private:

    // 用于 epoll 的文件描述符。
    int m_epfd = 0;

    // 用于进程间通信的管道文件描述符，fd[0] 是读端，fd[1] 是写端。
    // pipe 通常用于进程间通信，但由于线程共享进程的文件描述符表，因此同一进程内的多个线程也可以通过 pipe 通信。IOManager 使用 pipe 并不是为了传递业务数据，而是创建一个可被 epoll 监听的 fd。当其他线程向调度器添加任务时，通过向 pipe 写入数据触发可读事件，使阻塞在 epoll_wait 中的线程被唤醒。本质是利用 fd 事件机制实现线程间唤醒。
    int m_tickleFds[2];

    // 原子计数器，用于记录待处理的事件数量。使用 atomic 的好处是这个变量再进行 + 或 - 都是不会被多线程影响。
    std::atomic<size_t> m_pendingEventCount = {0};

    // 读写锁。
    std::shared_mutex m_mutex;

    // 文件描述符上下文数组，用于存储每个文件描述符的 FdContext。
    std::vector<FdContext *> m_fdContexts;
};


#endif
