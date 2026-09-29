#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "timer.h"

#include "config.h"


TEST(TimeWheelTest, EmptyTick)
{
    TimerManager tmr;

    EXPECT_NO_THROW(
        {
            tmr.tick();
        });
}

TEST(TimeWheelTest, OneTimer)
{
    TimerManager tmr(8);
    int count = 0;

    tmr.addTimer(3,
                 [&]
                 {
                     ++count;
                 });

    EXPECT_EQ(count, 0);

    // 执行槽 1,2 的任务。
    for (int i = 1; i <= 2; ++i)
    {
        tmr.tick();
        EXPECT_EQ(count, 0);
    }

    // 执行槽 3 的任务。
    tmr.tick();
    EXPECT_EQ(count, 1);
}

TEST(TimeWheelTest, MultipleTimers)
{
    TimerManager tmr(8);
    std::vector<int> vec;

    tmr.addTimer(1, [&]
                 { vec.push_back(1); });
    tmr.addTimer(3, [&]
                 { vec.push_back(3); });
    tmr.addTimer(5, [&]
                 { vec.push_back(5); });

    tmr.tick();
    ASSERT_EQ(vec.size(), 1);
    EXPECT_EQ(vec[0], 1);

    tmr.tick();
    EXPECT_EQ(vec.size(), 1);

    tmr.tick();
    ASSERT_EQ(vec.size(), 2);
    EXPECT_EQ(vec[1], 3);

    tmr.tick();
    EXPECT_EQ(vec.size(), 2);

    tmr.tick();
    ASSERT_EQ(vec.size(), 3);
    EXPECT_EQ(vec[2], 5);
}

TEST(TimeWheelTest, SameSlotDifferentRound)
{
    TimerManager tmr(8);
    std::vector<int> vec;

    tmr.addTimer(1, [&]
                 { vec.push_back(1); });
    tmr.addTimer(9, [&]
                 { vec.push_back(9); });
    tmr.addTimer(17, [&]
                 { vec.push_back(17); });

    for (int i = 1; i <= 17; ++i) tmr.tick();

    ASSERT_EQ(vec.size(), 3);

    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 9);
    EXPECT_EQ(vec[2], 17);
}

TEST(TimeWheelTest, ZeroTimeout)
{
    TimerManager tmr(8);

    bool called = false;

    if (DFIBER_CONFIG_DEBUG)
    {
        ASSERT_DEATH({ tmr.addTimer(0,
                                    [&]
                                    {
                                        called = true;
                                    }); },
                     "");
    }
}

TEST(TimeWheelTest, SameExpire)
{
    TimerManager tmr(8);
    int count = 0;

    for (int i = 0; i < 3; ++i)
    {
        tmr.addTimer(3, [&]
                     { ++count; });
    }

    for (int i = 1; i <= 3; ++i) tmr.tick();

    EXPECT_EQ(count, 3);
}

TEST(TimeWheelTest, CrossRound)
{
    TimerManager tmr(8);
    bool called = false;

    tmr.addTimer(15,
                 [&]
                 {
                     called = true;
                 });

    for (int i = 1; i <= 14; ++i) tmr.tick();

    EXPECT_FALSE(called);
    tmr.tick();
    EXPECT_TRUE(called);
}

TEST(TimeWheelTest, ManyTimers)
{
    TimerManager tmr(16);

    int count = 0;

    for (int i = 1; i <= 100; ++i)
    {
        tmr.addTimer(i,
                     [&]
                     {
                         ++count;
                     });
    }

    for (int i = 1; i <= 100; ++i) tmr.tick();

    EXPECT_EQ(count, 100);
}


int main(int argc, char *argv[])
{
    ::testing::InitGoogleTest(&argc, argv);


    return RUN_ALL_TESTS();
}
