#include <gtest/gtest.h>

#include <thread>
#include <queue>

#include "semaphore.h"


TEST(SemaphoreTest, InitialCount)
{
    Semaphore sem(1);

    EXPECT_NO_THROW({
        sem.wait();
    });
}

TEST(SemaphoreTest, SignalThenWait)
{
    Semaphore sem;

    sem.signal();

    EXPECT_NO_THROW({
        sem.wait();
    });
}

TEST(SemaphoreTest, WaitBlocksUntilSignal)
{
    Semaphore sem;

    bool finished = false;

    std::thread t([&]
                  {
                      sem.wait();
                      finished = true; //
                  });

    EXPECT_FALSE(finished);

    sem.signal();

    t.join();

    EXPECT_TRUE(finished);
}

TEST(SemaphoreTest, MultipleSignal)
{
    Semaphore sem;

    sem.signal();
    sem.signal();
    sem.signal();

    EXPECT_NO_THROW(sem.wait());
    EXPECT_NO_THROW(sem.wait());
    EXPECT_NO_THROW(sem.wait());
}

TEST(SemaphoreTest, MultipleWaiters)
{
    Semaphore sem;
    std::atomic<int> counter{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i)
    {
        threads.emplace_back([&]
                             {
                                 sem.wait();
                                 ++counter; //
                             });
    }

    EXPECT_EQ(counter.load(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < 10; ++i) sem.signal();

    for (auto &t : threads) t.join();

    EXPECT_EQ(counter.load(), 10);
}

TEST(SemaphoreTest, OneSignalWakeOneThread)
{
    Semaphore sem;
    std::atomic<int> counter = 0;

    std::thread t1([&]
                   {
                       sem.wait();
                       ++counter; //
                   });

    std::thread t2([&]
                   {
                       sem.wait();
                       ++counter; //
                   });

    EXPECT_EQ(counter.load(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sem.signal();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(counter.load(), 1);

    sem.signal();

    t1.join();
    t2.join();

    EXPECT_EQ(counter.load(), 2);
}

TEST(SemaphoreTest, InitialCountThree)
{
    Semaphore sem(3);

    sem.wait();
    sem.wait();
    sem.wait();
}

TEST(SemaphoreTest, Stress)
{
    constexpr int N = 100;

    Semaphore sem;
    std::atomic<int> counter = 0;
    std::vector<std::thread> threads;

    for (int i = 0; i < N; i++)
    {
        threads.emplace_back([&]
                             {
                                 sem.wait();
                                 ++counter; //
                             });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < N; i++) sem.signal();

    for (auto &t : threads) t.join();

    EXPECT_EQ(counter.load(), N);
}

TEST(SemaphoreTest, SignalBeforeWait)
{
    Semaphore sem;

    for (int i = 0; i < 5; i++) sem.signal();

    for (int i = 0; i < 5; i++) sem.wait();

    SUCCEED();
}

TEST(SemaphoreTest, ProducerConsumer)
{
    constexpr int N = 100;
    int sum = 0;

    Semaphore sem;
    std::queue<int> q;
    std::mutex mtx;

    std::thread producer([&]
                         {
                             for (int i = 0; i < N; i++)
                             {
                                 {
                                     std::lock_guard<std::mutex> lock(mtx);
                                     q.push(i);
                                 }

                                 sem.signal();
                             } //
                         });

    std::thread consumer([&]
                         {
                             for (int i = 0; i < N; i++)
                             {
                                 sem.wait();

                                 {
                                     std::lock_guard<std::mutex> lock(mtx);

                                     sum += q.front();
                                     q.pop();
                                 }
                             } //
                         });

    producer.join();
    consumer.join();

    EXPECT_EQ(sum, (N - 1) * N / 2);
}
