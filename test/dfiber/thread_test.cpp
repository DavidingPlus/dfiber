#include <gtest/gtest.h>

#include "thread.h"


TEST(ThreadTest, CreateThread)
{
    bool executed = false;

    Thread t([&]
             { executed = true; },
             "worker");

    t.join();

    EXPECT_TRUE(executed);
}

TEST(ThreadTest, MultiThread)
{
    std::atomic<int> counter = 0;

    std::vector<std::unique_ptr<Thread>> threads;

    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back(std::make_unique<Thread>(
            [&]
            {
                ++counter;
            },
            "worker_" + std::to_string(i)));
    }

    for (auto &t : threads) t->join();

    EXPECT_EQ(counter.load(), 10);
}

TEST(ThreadTest, GetThis)
{
    Thread *self = nullptr;

    Thread t([&]
             {
                 self = Thread::GetThis(); // 获取创建出的工作线程的 Thread 对象指针。
             },
             "worker");

    t.join();

    EXPECT_NE(self, nullptr);
}

TEST(ThreadTest, GetName)
{
    std::string name;

    Thread t([&]
             {
                 name = Thread::GetName(); // 获取创建出的工作线程的名称。
             },
             "worker");

    t.join();

    EXPECT_EQ(name, "worker");
}

TEST(ThreadTest, SetName)
{
    std::string name;

    Thread t([&]
             {
                 Thread::SetName("newName");
                 name = Thread::GetName(); //
             },
             "worker");

    t.join();

    EXPECT_EQ(name, "newName");
}

TEST(ThreadTest, GetId)
{
    Thread t([] {}, "worker");

    t.join();

    EXPECT_GT(t.getId(), 0);
}

TEST(ThreadTest, Join)
{
    bool finished = false;

    Thread t([&]
             {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        finished = true; },
             "worker");

    t.join();

    EXPECT_TRUE(finished);
}

TEST(ThreadTest, ConcurrentIncrement)
{
    std::atomic<int> sum = 0;
    std::vector<std::unique_ptr<Thread>> threads;

    for (int i = 0; i < 100; ++i)
    {
        threads.emplace_back(std::make_unique<Thread>(
            [&]
            {
                ++sum;
            },
            "worker"));
    }

    for (auto &t : threads) t->join();

    EXPECT_EQ(sum.load(), 100);
}

TEST(ThreadTest, ThreadLocal)
{
    std::vector<std::string> names(5);
    std::vector<std::unique_ptr<Thread>> threads;

    for (int i = 0; i < 5; ++i)
    {
        threads.emplace_back(std::make_unique<Thread>(
            [&, i]
            {
                names[i] = Thread::GetName();
            },
            "thread_" + std::to_string(i)));
    }

    for (auto &t : threads) t->join();

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(names[i], "thread_" + std::to_string(i));
    }
}

TEST(ThreadTest, ThreadLocalPointer)
{
    std::set<Thread *> threadSet;
    std::mutex mutex;
    std::vector<std::unique_ptr<Thread>> threads;

    for (int i = 0; i < 20; i++)
    {
        threads.emplace_back(std::make_unique<Thread>(
            [&]
            {
                std::lock_guard<std::mutex> lock(mutex);
                threadSet.insert(Thread::GetThis());
            },
            "worker"));
    }

    for (auto &t : threads) t->join();

    EXPECT_EQ(threadSet.size(), 20);
}

TEST(ThreadTest, ThreadIdUnique)
{
    std::set<pid_t> ids;
    std::mutex mutex;
    std::vector<std::unique_ptr<Thread>> workers;

    for (int i = 0; i < 20; i++)
    {
        workers.emplace_back(std::make_unique<Thread>(
            [&]
            {
                std::lock_guard<std::mutex> lock(mutex);
                ids.insert(Thread::GetThreadId());
            },
            "worker"));
    }

    for (auto &t : workers) t->join();

    EXPECT_EQ(ids.size(), 20);
}

TEST(ThreadTest, ExceptionInThread)
{
    Thread t([]
             {
                 throw std::runtime_error("boom"); //
             },
             "worker");

    EXPECT_NO_THROW(t.join());
}
