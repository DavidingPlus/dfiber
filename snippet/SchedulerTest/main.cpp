#include <iostream>
#include <chrono>

#include "scheduler.h"


static unsigned int number;
std::mutex mutex;


void task()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << "task " << number++ << " is under processing in thread: " << dfiber::Thread::GetThreadId() << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

int main(int argc, char const *argv[])
{
    std::shared_ptr<dfiber::Scheduler> scheduler = std::make_shared<dfiber::Scheduler>(3, true, "scheduler_1");

    scheduler->start();

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\nbegin post\n\n";
    for (int i = 0; i < 5; ++i)
    {
        std::shared_ptr<dfiber::Fiber> fiber = std::make_shared<dfiber::Fiber>(task);
        scheduler->scheduleLock(fiber);
    }

    std::this_thread::sleep_for(std::chrono::seconds(6));

    std::cout << "\npost again\n\n";
    for (int i = 0; i < 15; ++i)
    {
        std::shared_ptr<dfiber::Fiber> fiber = std::make_shared<dfiber::Fiber>(task);
        scheduler->scheduleLock(fiber);
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    // scheduler 如果有设置将加入工作处理。
    scheduler->stop();


    return 0;
}
