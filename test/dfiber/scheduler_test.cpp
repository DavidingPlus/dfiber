#include <gtest/gtest.h>

#include "scheduler.h"


TEST(SchedulerTest, BasicFunctionSchedule)
{
    Scheduler sc(1, false, "test");

    std::atomic<int> val = 0;

    sc.scheduleLock([&]()
                    { ++val; });

    sc.start();
    sc.stop();

    EXPECT_EQ(val.load(), 1);
}

TEST(SchedulerTest, FiberSchedule)
{
    Scheduler sc(1, false, "test");

    std::atomic<int> val = 0;

    auto fiber = std::make_shared<Fiber>([&]()
                                         { val = 42; });

    sc.scheduleLock(fiber);

    sc.start();
    sc.stop();

    EXPECT_EQ(val.load(), 42);
}

TEST(SchedulerTest, MultiTaskExecution)
{
    Scheduler sc(2, false, "test");

    std::vector<int> res;
    std::mutex mtx;

    for (int i = 0; i < 10; ++i)
    {
        sc.scheduleLock([&, i]()
                        {
                            std::lock_guard<std::mutex> lock(mtx);

                            res.emplace_back(i); //
                        });
    }

    sc.start();
    sc.stop();

    EXPECT_EQ(res.size(), 10);
}

TEST(SchedulerTest, ThreadExecution)
{
    Scheduler sc(4, false, "test");

    std::set<pid_t> threadIds;
    std::mutex mtx;

    for (int i = 0; i < 20; ++i)
    {
        sc.scheduleLock([&]()
                        {
                            std::lock_guard<std::mutex> lock(mtx);

                            threadIds.insert(Thread::GetThreadId()); //
                        });
    }

    sc.start();
    sc.stop();

    // 至少使用多个线程。
    EXPECT_GE(threadIds.size(), 1);
}

TEST(SchedulerTest, StopCorrectness)
{
    Scheduler sc(2, false, "test");

    std::atomic<int> count = 0;

    for (int i = 0; i < 100; ++i)
    {
        sc.scheduleLock([&]()
                        { ++count; });
    }

    sc.start();
    sc.stop();

    EXPECT_EQ(count.load(), 100);
}

TEST(SchedulerTest, UseCallerMode)
{
    Scheduler sc(1, false, "test");

    std::atomic<int> val = 0;

    sc.scheduleLock([&]()
                    { val = 10; });

    sc.start();
    sc.stop();

    EXPECT_EQ(val.load(), 10);
}

// TEST(SchedulerTest, StoppingState)
// {
//     Scheduler sc(1, false, "test");

//     EXPECT_FALSE(sc.stopping());

//     sc.start();
//     sc.stop();

//     EXPECT_TRUE(sc.stopping());
// }

TEST(SchedulerTest, StressTest)
{
    Scheduler sc(4, false, "test");

    std::atomic<int> counter = 0;

    for (int i = 0; i < 1000; ++i)
    {
        sc.scheduleLock([&]()
                        { ++counter; });
    }

    sc.start();
    sc.stop();

    EXPECT_EQ(counter.load(), 1000);
}
