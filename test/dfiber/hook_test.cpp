#include <gtest/gtest.h>

#include "hook.h"

#include "fdmanager.h"
#include "iomanager.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>


static uint64_t getCurrentMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int getRawFdFlags(int fd)
{
    return fcntl_f(fd, F_GETFL, 0);
}


TEST(HookTest, HookEnableDefault)
{
    setHookEnable(false);

    EXPECT_FALSE(isHookEnable());

    setHookEnable(false);
}

TEST(HookTest, HookEnableOpen)
{
    // 这里只验证“accept 成功后，新返回的 client fd 会被 hook 注册进 FdMgr”。不验证 accept 的等待/挂起路径，因为当前这套 hook 的前提是：可能阻塞的 hook IO 必须运行在 IOManager 调度的协程里，而不能直接跑在测试主线程上。
    // 一个典型的错误写法（例 RegisterAcceptedFd）是：主线程 setHookEnable(true) 后直接调用 accept()。当监听队列里还没有连接时，accept 会先返回 EAGAIN，随后 hook 会先 addEvent，再调用 Fiber::yield() 试图挂起当前执行流。但主线程此时并不在 IOManager::run() 的调度协程上下文里，yield() 不会真正挂起到事件循环等待 epoll，而是马上回到主线程主协程，导致代码继续 retry。retry 后又会重复对 listenFd 注册 READ 事件，最终表现为 addEvent 失败。
    setHookEnable(true);

    EXPECT_TRUE(isHookEnable());

    setHookEnable(false);
}

TEST(HookTest, HookEnableClose)
{
    setHookEnable(true);
    setHookEnable(false);

    EXPECT_FALSE(isHookEnable());

    setHookEnable(false);
}

TEST(SocketHookTest, TCPRegister)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);

    EXPECT_TRUE(ctx->isSocket());
    EXPECT_FALSE(ctx->isClosed());

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(SocketHookTest, UDPRegister)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);

    EXPECT_TRUE(ctx->isSocket());

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(SocketHookTest, CreateFailed)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(-1, SOCK_STREAM, 0);
    EXPECT_EQ(fd, -1);

    setHookEnable(false);
}

TEST(CloseHookTest, RemoveFdCtx)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    ASSERT_NE(FdMgr::GetInstance()->get(fd), nullptr);
    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(FdMgr::GetInstance()->get(fd), nullptr);

    setHookEnable(false);
}

TEST(CloseHookTest, ClosePipe)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd[2];

    ASSERT_EQ(pipe(fd), 0);
    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(CloseHookTest, DoubleClose)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    EXPECT_EQ(close(fd), 0);
    EXPECT_EQ(close(fd), -1);

    setHookEnable(false);
}

TEST(SocketTimeoutTest, ReceiveTimeout)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    timeval tv = {
        .tv_sec = 3,
        .tv_usec = 500000,
    };

    ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)), 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(ctx->getTimeout(SO_RCVTIMEO), 3500);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(SocketTimeoutTest, SendTimeout)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };

    ASSERT_EQ(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)), 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);

    EXPECT_EQ(ctx->getTimeout(SO_SNDTIMEO), 1000);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(SocketTimeoutTest, NormalOption)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int val = 1;
    EXPECT_EQ(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)), 0);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(IOHookTest, PipeReadWrite)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    ASSERT_EQ(write(fd[1], "hello", 5), 5);

    char buf[16] = {};

    ASSERT_EQ(read(fd[0], buf, 16), 5);
    EXPECT_STREQ(buf, "hello");

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(IOHookTest, PipeMultiReadWrite)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    write(fd[1], "abc", 3);
    write(fd[1], "def", 3);

    char buf[8] = {};

    EXPECT_EQ(read(fd[0], buf, 6), 6);
    EXPECT_STREQ(buf, "abcdef");

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(IOHookTest, InvalidRead)
{
    setHookEnable(true);

    IOManager iom(1, false);

    char buf[8];

    EXPECT_EQ(read(-1, buf, sizeof(buf)), -1);
    EXPECT_EQ(errno, EBADF);

    setHookEnable(false);
}

TEST(HookIOTest, ReadvWritevBasic)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    char msg1[] = "hello ";
    char msg2[] = "world";

    iovec wiov[2];

    wiov[0].iov_base = msg1;
    wiov[0].iov_len = strlen(msg1);

    wiov[1].iov_base = msg2;
    wiov[1].iov_len = strlen(msg2);

    ASSERT_EQ(writev(fd[1], wiov, 2), 11);

    char buf1[7] = {};
    char buf2[6] = {};

    iovec riov[2];

    riov[0].iov_base = buf1;
    riov[0].iov_len = 6;

    riov[1].iov_base = buf2;
    riov[1].iov_len = 5;

    ASSERT_EQ(readv(fd[0], riov, 2), 11);

    EXPECT_STREQ(buf1, "hello ");
    EXPECT_STREQ(buf2, "world");

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(HookIOTest, ReadvWritevManyBuffers)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    std::vector<std::string> data{"a", "bb", "ccc", "dddd"};
    std::vector<iovec> wiov;

    for (auto &s : data)
    {
        iovec v;
        v.iov_base = (void *)s.data();
        v.iov_len = s.size();

        wiov.push_back(v);
    }

    size_t total = 10;

    ASSERT_EQ(writev(fd[1], wiov.data(), wiov.size()), total);

    char buffer[32] = {};

    EXPECT_EQ(read(fd[0], buffer, sizeof(buffer)), total);
    EXPECT_STREQ(buffer, "abbcccdddd");

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(HookIOTest, ReadvBasic)
{
    int fd[2];
    pipe(fd);

    std::thread t([&]
                  {
                      char a[8]{};
                      char b[8]{};
                      size_t total = 0;

                      while (total < 7)
                      {
                          iovec iov[2];
                          int iovcnt = 0;

                          if (total < 5)
                          {
                              iov[iovcnt++] = {a + total, 5 - total};
                              iov[iovcnt++] = {b, 2};
                          }
                          else
                          {
                              iov[iovcnt++] = {b + (total - 5), 7 - total};
                          }

                          ssize_t n = readv(fd[0], iov, iovcnt);
                          ASSERT_GT(n, 0);

                          total += static_cast<size_t>(n);
                      }

                      EXPECT_STREQ(a, "hello");
                      EXPECT_STREQ(b, "!!"); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(write(fd[1], "hello", 5), 5);
    ASSERT_EQ(write(fd[1], "!!", 2), 2);

    t.join();
}

TEST(HookIOTest, ReadvWaitEvent)
{
    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    IOManager iom(1, false);
    std::atomic<bool> finished = false;

    iom.scheduleLock([&]
                     {
                         setHookEnable(true);

                         char a[8] = {};
                         char b[8] = {};
                         size_t total = 0;

                         // readv 对 pipe/socket 只保证“读取当前可用的数据”，并不保证一次把两个 iovec
                         // 全部填满。因此这里循环读取到 7 字节为止，避免把部分读取的合法行为误判成失败。
                         while (total < 7)
                         {
                             iovec iov[2];
                             int iovcnt = 0;

                             if (total < 5)
                             {
                                 iov[iovcnt++] = {a + total, 5 - total};
                                 iov[iovcnt++] = {b, 2};
                             }
                             else
                             {
                                 iov[iovcnt++] = {b + (total - 5), 7 - total};
                             }

                             ssize_t n = readv(fd[0], iov, iovcnt);
                             ASSERT_GT(n, 0);

                             total += static_cast<size_t>(n);
                         }

                         EXPECT_STREQ(a, "hello");
                         EXPECT_STREQ(b, "!!");

                         finished = true;

                         setHookEnable(false); //
                     });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_FALSE(finished);

    setHookEnable(true);

    ASSERT_EQ(write(fd[1], "hello", 5), 5);
    ASSERT_EQ(write(fd[1], "!!", 2), 2);

    setHookEnable(false);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    EXPECT_TRUE(finished);

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);
}

TEST(ConnectHookTest, InvalidFd)
{
    setHookEnable(true);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(1);

    EXPECT_EQ(connect(-1, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), -1);
    EXPECT_EQ(errno, EBADF);

    setHookEnable(false);
}

TEST(ConnectHookTest, NonSocketFdFallsBackToSyscall)
{
    setHookEnable(true);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(1);

    EXPECT_EQ(connect(fd[0], reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), -1);
    EXPECT_EQ(errno, ENOTSOCK);

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(AcceptHookTest, RegisterAcceptedFd)
{
    setHookEnable(true);

    IOManager iom(1, false);

    // 创建监听 socket。
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listenFd, 0);

    // 设置端口复用。
    int reuse = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0); // 随机端口。

    ASSERT_EQ(bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0) << strerror(errno);

    ASSERT_EQ(listen(listenFd, 5), 0) << strerror(errno);

    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(listenFd, reinterpret_cast<sockaddr *>(&addr), &len), 0) << strerror(errno);
    std::cout << "Listen Port = " << ntohs(addr.sin_port) << std::endl;

    std::atomic<bool> finished = false;
    int acceptedFd = -1;
    std::shared_ptr<FdCtx> acceptedCtx;

    // 客户端线程。
    std::thread client([&]()
                       {
                           int fd = socket(AF_INET, SOCK_STREAM, 0);
                           ASSERT_GE(fd, 0);

                           int res = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

                           ASSERT_EQ(res, 0) << strerror(errno);

                           EXPECT_EQ(close(fd), 0); //
                       });

    std::cout << "Waiting accept..." << std::endl;

    // 主线程这里使用 sleep_for 只是为了协调测试时序。如果主线程保持 hook 打开，底层可能走到被 hook 的 nanosleep/sleep 路径，注册定时器事件以后就 yield() 让出调度权了，显然会遇到很多不可预期的问题，所以临时关闭主线程 hook。
    setHookEnable(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    setHookEnable(true);

    iom.scheduleLock([&]()
                     {
                         // 阻塞型 hook IO 必须在 IOManager 调度到的协程里执行，原因和 hook 的实现方式直接相关：
                         //
                         // 1. accept/read/write 这类 hook 在首次遇到 EAGAIN 时，不会阻塞线程，而是先把 fd 的 READ/WRITE 事件注册到 IOManager；再调用当前 Fiber::yield() 主动让出执行权；等 epoll_wait 检测到事件就绪后，再由 IOManager 把这个 Fiber 重新调度回来。
                         //
                         // 2. 这条链路成立的前提是：当前执行流本身就是一个“被 IOManager 调度的工作协程”。只有这种协程 yield 之后，控制权才会正确回到 Scheduler/IOManager 的调度循环，后者才能继续跑 epoll_wait，等事件触发后再恢复这个协程。
                         //
                         // 3. 之前错误的写法是在测试主线程里直接调用 accept()。主线程虽然 setHookEnable(true) 了，但它并没有运行在 IOManager::run() 调度出来的工作协程里，因此 accept hook 里的 yield 并不会把执行权交回真正的 IO 调度循环。结果是：accept 第一次因为还没有连接而返回 EAGAIN；hook 给 listenFd 注册 READ 事件；但 yield 后没有正确“挂起等待事件”，而是很快又回到当前执行路径；随后的 retry 会再次对同一个 listenFd addEvent；最终因为重复注册同一 READ 事件而失败。
                         //
                         // 4. 所以这个用例必须把 accept 放进 scheduleLock() 调度的协程里执行，才能满足 hook 的运行前提，保证“注册事件 -> 挂起 -> 事件触发 -> 恢复协程”这条路径在语义上是成立的。
                         setHookEnable(true);

                         sockaddr_in clientAddr{};
                         socklen_t clientLen = sizeof(clientAddr);

                         acceptedFd = accept(listenFd, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
                         if (acceptedFd >= 0) acceptedCtx = FdMgr::GetInstance()->get(acceptedFd);

                         finished = true;

                         setHookEnable(false); //
                     });

    client.join();

    setHookEnable(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    setHookEnable(true);

    ASSERT_TRUE(finished);
    ASSERT_GE(acceptedFd, 0) << strerror(errno);
    ASSERT_NE(acceptedCtx, nullptr);

    EXPECT_TRUE(acceptedCtx->isSocket());
    EXPECT_FALSE(acceptedCtx->isClosed());

    EXPECT_EQ(close(acceptedFd), 0);
    EXPECT_EQ(close(listenFd), 0);

    setHookEnable(false);
}

TEST(SleepHookTest, SingleWorkerWithoutHookBlocksFollowingTask)
{
    // 只给一个 worker，确保两个任务一定串行竞争同一条执行线程。这个用例验证“不启用 hook 时，sleep 会直接阻塞 worker 线程”，因此后面的任务在第一个任务 sleep 返回前连开始执行的机会都拿不到。
    IOManager iom(1, false);

    std::atomic<bool> firstStarted = false;
    std::atomic<bool> firstCompleted = false;
    std::atomic<bool> secondStarted = false;
    std::atomic<bool> secondCompleted = false;

    iom.scheduleLock([&]()
                     {
                         firstStarted = true;

                         sleep(1);

                         firstCompleted = true; //
                     });

    iom.scheduleLock([&]()
                     {
                         secondStarted = true;

                         sleep(1);

                         secondCompleted = true; //
                     });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(firstStarted);
    ASSERT_FALSE(firstCompleted);
    ASSERT_FALSE(secondStarted);
    ASSERT_FALSE(secondCompleted);

    // 大约 1 秒后，第一个任务会完成，但第二个任务此时才刚拿到运行机会。
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    ASSERT_TRUE(firstCompleted);
    ASSERT_TRUE(secondStarted);
    ASSERT_FALSE(secondCompleted);
}

TEST(SleepHookTest, SingleWorkerWithHookYieldsFollowingTask)
{
    // 仍然只给一个 worker，但在 worker 协程里启用 hook。此时 sleep 不再阻塞线程，而是“注册定时器 + 当前 Fiber yield”，这样同一个 worker 就能继续调度后续任务执行。对比上一个用例，这里第二个任务会很快拿到执行机会。
    IOManager iom(1, false);

    std::atomic<bool> firstStarted = false;
    std::atomic<bool> firstCompleted = false;
    std::atomic<bool> secondStarted = false;
    std::atomic<bool> secondCompleted = false;

    iom.scheduleLock([&]()
                     {
                         firstStarted = true;

                         setHookEnable(true);
                         sleep(1);
                         setHookEnable(false);

                         firstCompleted = true; //
                     });

    iom.scheduleLock([&]()
                     {
                         secondStarted = true;

                         setHookEnable(true);
                         sleep(1);
                         setHookEnable(false);

                         secondCompleted = true; //
                     });

    // 主线程不启用 hook，避免 sleep_for 误走 hook 的 nanosleep 路径。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(firstStarted);
    ASSERT_FALSE(firstCompleted);
    ASSERT_TRUE(secondStarted);
    ASSERT_FALSE(secondCompleted);

    // 两个任务都会在各自 sleep(1) 到期后被重新调度回来，因此这一轮等待后应当都已完成。
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    ASSERT_TRUE(firstCompleted);
    ASSERT_TRUE(secondCompleted);
}

TEST(UsleepHookTest, Precision)
{
    IOManager iom(2, false);
    std::atomic<uint64_t> cost = 0;

    iom.scheduleLock([&]()
                     {
                         setHookEnable(true);

                         auto begin = getCurrentMs();

                         usleep(100000);

                         auto end = getCurrentMs();

                         cost = end - begin;

                         setHookEnable(false); //
                     });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    EXPECT_GE(cost, 100);
    EXPECT_LT(cost, 200);
}

TEST(UsleepHookTest, Zero)
{
    IOManager iom(2, false);

    iom.scheduleLock([&]()
                     {
                         setHookEnable(true);

                         EXPECT_EQ(usleep(0), 0);

                         setHookEnable(false); //
                     });
}

TEST(NanoSleepHookTest, Basic)
{
    IOManager iom(2, false);
    std::atomic<uint64_t> cost = 0;
    timespec ts = {
        .tv_sec = 1,
        .tv_nsec = 0,
    };

    iom.scheduleLock([&]()
                     {
                         setHookEnable(true);

                         auto begin = getCurrentMs();

                         EXPECT_EQ(nanosleep(&ts, nullptr), 0);

                         auto end = getCurrentMs();

                         cost = end - begin;

                         setHookEnable(false); //
                     });

    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    EXPECT_GE(cost, 1000);
    EXPECT_LT(cost, 2000);
}

TEST(FcntlHookTest, SocketGetflHidesFrameworkNonblock)
{
    setHookEnable(true);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);
    EXPECT_TRUE(ctx->getSysNonblock());
    EXPECT_FALSE(ctx->getUserNonblock());

    int rawFlags = getRawFdFlags(fd);
    ASSERT_NE(rawFlags, -1) << strerror(errno);
    EXPECT_TRUE(rawFlags & O_NONBLOCK);

    int visibleFlags = fcntl(fd, F_GETFL, 0);
    ASSERT_NE(visibleFlags, -1) << strerror(errno);
    EXPECT_FALSE(visibleFlags & O_NONBLOCK);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(FcntlHookTest, SocketSetflTracksUserNonblockAndKeepsKernelNonblock)
{
    setHookEnable(true);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);

    int visibleFlags = fcntl(fd, F_GETFL, 0);
    ASSERT_NE(visibleFlags, -1) << strerror(errno);
    EXPECT_FALSE(visibleFlags & O_NONBLOCK);

    ASSERT_EQ(fcntl(fd, F_SETFL, visibleFlags | O_NONBLOCK), 0) << strerror(errno);
    EXPECT_TRUE(ctx->getUserNonblock());
    EXPECT_TRUE(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(getRawFdFlags(fd) & O_NONBLOCK);

    visibleFlags = fcntl(fd, F_GETFL, 0);
    ASSERT_NE(visibleFlags, -1) << strerror(errno);
    ASSERT_EQ(fcntl(fd, F_SETFL, visibleFlags & ~O_NONBLOCK), 0) << strerror(errno);
    EXPECT_FALSE(ctx->getUserNonblock());
    EXPECT_FALSE(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(getRawFdFlags(fd) & O_NONBLOCK);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(FcntlHookTest, NonSocketSetflFallsBackToSyscall)
{
    setHookEnable(true);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);
    EXPECT_EQ(FdMgr::GetInstance()->get(fd[0]), nullptr);

    int initialFlags = fcntl(fd[0], F_GETFL, 0);
    ASSERT_NE(initialFlags, -1) << strerror(errno);
    EXPECT_EQ(initialFlags, getRawFdFlags(fd[0]));

    ASSERT_EQ(fcntl(fd[0], F_SETFL, initialFlags | O_NONBLOCK), 0) << strerror(errno);
    EXPECT_TRUE(fcntl(fd[0], F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(getRawFdFlags(fd[0]) & O_NONBLOCK);

    ASSERT_EQ(fcntl(fd[0], F_SETFL, initialFlags & ~O_NONBLOCK), 0) << strerror(errno);
    EXPECT_FALSE(fcntl(fd[0], F_GETFL, 0) & O_NONBLOCK);
    EXPECT_FALSE(getRawFdFlags(fd[0]) & O_NONBLOCK);

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}

TEST(FcntlHookTest, OtherCommandsPassThrough)
{
    setHookEnable(true);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    ASSERT_EQ(fcntl(fd, F_SETFD, FD_CLOEXEC), 0) << strerror(errno);

    int flags = fcntl(fd, F_GETFD, 0);
    ASSERT_NE(flags, -1) << strerror(errno);
    EXPECT_TRUE(flags & FD_CLOEXEC);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(FcntlHookTest, InvalidFdFallsBackToSyscall)
{
    setHookEnable(true);

    errno = 0;
    EXPECT_EQ(fcntl(-1, F_GETFL, 0), -1);
    EXPECT_EQ(errno, EBADF);

    errno = 0;
    EXPECT_EQ(fcntl(-1, F_SETFL, O_NONBLOCK), -1);
    EXPECT_EQ(errno, EBADF);

    setHookEnable(false);
}

TEST(IoctlHookTest, SocketFionbioSyncsUserNonblock)
{
    setHookEnable(true);

    IOManager iom(1, false);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    auto ctx = FdMgr::GetInstance()->get(fd);
    ASSERT_NE(ctx, nullptr);
    EXPECT_FALSE(ctx->getUserNonblock());

    int flag = 1;
    ASSERT_EQ(ioctl(fd, FIONBIO, &flag), 0) << strerror(errno);
    EXPECT_TRUE(ctx->getUserNonblock());
    EXPECT_TRUE(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(getRawFdFlags(fd) & O_NONBLOCK);

    flag = 0;
    ASSERT_EQ(ioctl(fd, FIONBIO, &flag), 0) << strerror(errno);
    EXPECT_FALSE(ctx->getUserNonblock());
    EXPECT_FALSE(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    EXPECT_TRUE(getRawFdFlags(fd) & O_NONBLOCK);

    EXPECT_EQ(close(fd), 0);

    setHookEnable(false);
}

TEST(IoctlHookTest, NonSocketFionbioFallsBackToSyscall)
{
    setHookEnable(true);

    int fd[2];
    ASSERT_EQ(pipe(fd), 0);
    EXPECT_EQ(FdMgr::GetInstance()->get(fd[0]), nullptr);

    int flag = 1;
    ASSERT_EQ(ioctl(fd[0], FIONBIO, &flag), 0) << strerror(errno);

    EXPECT_EQ(FdMgr::GetInstance()->get(fd[0]), nullptr);
    EXPECT_TRUE(fcntl(fd[0], F_GETFL, 0) & O_NONBLOCK);

    EXPECT_EQ(close(fd[0]), 0);
    EXPECT_EQ(close(fd[1]), 0);

    setHookEnable(false);
}
