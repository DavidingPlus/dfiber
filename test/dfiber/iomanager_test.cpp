#include <gtest/gtest.h>

#include "iomanager.h"

#include <atomic>

#include <unistd.h>


TEST(IOManagerTest, ReadEvent)
{
    IOManager iom(2, true); // 成功。
    // IOManager iom(2, false); // 成功。
    // IOManager iom(1, true);  // 默认的参数。失败。因为主线程参与工作调度的情况下，目前设计需要 IOManager 析构时调用 stop() 才会调度主线程，因此肯定是失败的。

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    std::atomic<bool> called = false;

    std::cout << "scheduler= " << Scheduler::GetThis() << std::endl;

    char buffer[16] = {0};
    auto res = iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            called = true;

            read(pipefd[0], buffer, sizeof(buffer));
        });

    ASSERT_EQ(res, 0);

    // 触发读事件。
    write(pipefd[1], "hello", 5);

    // 等待调度。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(called);
    EXPECT_STREQ(buffer, "hello");

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, WriteEvent)
{
    IOManager iom(1, false);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    std::atomic<bool> called = false;

    iom.addEvent(
        pipefd[1],
        IOManager::Event::WRITE,
        [&]()
        {
            called = true;

            write(pipefd[1], "hello", 5);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(called);

    char buffer[16] = {0};
    read(pipefd[0], buffer, sizeof(buffer));
    EXPECT_STREQ(buffer, "hello");

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, ReadWriteEvent)
{
    IOManager iom(1, false);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    std::atomic<int> count = 0;

    char buffer[16] = {0};
    iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            ++count;

            read(pipefd[0], buffer, sizeof(buffer));
        });

    iom.addEvent(
        pipefd[1],
        IOManager::Event::WRITE,
        [&]()
        {
            ++count;

            write(pipefd[1], "hello", 5);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(count, 2);
    EXPECT_STREQ(buffer, "hello");

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, DuplicateAddEvent)
{
    IOManager iom(2, false);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    ASSERT_EQ(
        iom.addEvent(
            pipefd[0],
            IOManager::Event::READ),
        0);

    ASSERT_EQ(
        iom.addEvent(
            pipefd[0],
            IOManager::Event::READ),
        -1);

    ASSERT_EQ(
        iom.delEvent(
            pipefd[0],
            IOManager::Event::READ),
        true);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, DeleteEvent)
{
    IOManager iom;

    int pipefd[2];
    pipe(pipefd);

    std::atomic<bool> called = false;

    iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            called = true;
        });

    ASSERT_TRUE(iom.delEvent(pipefd[0], IOManager::Event::READ));

    write(pipefd[1], "x", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_FALSE(called);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, CancelEvent)
{
    IOManager iom;

    int pipefd[2];
    pipe(pipefd);

    std::atomic<bool> called = false;

    iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            called = true;
        });

    ASSERT_TRUE(iom.cancelEvent(pipefd[0], IOManager::Event::READ));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(called);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, CancelAll)
{
    // cancelAll 测试需要避免事件循环线程和测试线程之间的时序竞争。pipe 写端通常一直处于可写状态，如果注册 WRITE 事件后立即在主线程调用 cancelAll，IOManager 线程可能提前收到 EPOLLOUT 并执行回调，导致事件已经被移除，使 cancelAll 返回 false。因此这里将 addEvent 和 cancelAll 放入同一个调度任务中串行执行，确保测试验证的是 cancelAll 的取消并触发回调语义，而不是线程调度顺序。
    IOManager iom(1, false);

    int pipefd[2];
    pipe(pipefd);

    std::atomic<int> count = 0;
    std::atomic<bool> finished = false;

    iom.scheduleLock([&]()
                     {
                         ASSERT_EQ(
                             iom.addEvent(
                                 pipefd[0],
                                 IOManager::Event::READ,
                                 [&]()
                                 {
                                     ++count;
                                 }),
                             0);

                         ASSERT_EQ(
                             iom.addEvent(
                                 pipefd[1],
                                 IOManager::Event::WRITE,
                                 [&]()
                                 {
                                     ++count;
                                 }),
                             0);

                         ASSERT_TRUE(iom.cancelAll(pipefd[0]));
                         ASSERT_TRUE(iom.cancelAll(pipefd[1]));

                         finished = true; //
                     });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(finished);
    ASSERT_EQ(count, 2);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, MultiThread)
{
    IOManager iom(4, false);

    int pipefd[2];
    pipe(pipefd);

    std::atomic<int> count = 0;

    iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            ++count;
        });

    write(pipefd[1], "hello", 5);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(count, 1);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(IOManagerTest, FiberReadEvent)
{
    IOManager iom(1, false);

    int pipefd[2];
    pipe(pipefd);

    std::atomic<bool> finished = false;

    iom.scheduleLock(
        [&]()
        {
            iom.addEvent(pipefd[0], IOManager::Event::READ);

            read(pipefd[0], nullptr, 0);

            finished = true;
        });

    write(pipefd[1], "x", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(finished);
}

TEST(IOManagerTest, IOAndTimer)
{
    IOManager iom;

    std::atomic<bool> timer = false;
    std::atomic<bool> io = false;

    iom.addTimer(
        50,
        [&]()
        {
            timer = true;
        });

    int pipefd[2];
    pipe(pipefd);

    iom.addEvent(
        pipefd[0],
        IOManager::Event::READ,
        [&]()
        {
            io = true;
        });

    write(pipefd[1], "x", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_TRUE(timer);
    ASSERT_TRUE(io);
}
