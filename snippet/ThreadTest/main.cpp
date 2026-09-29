#include <iostream>
#include <memory>
#include <vector>
#include <chrono>

#include "thread.h"


void func()
{
    std::cout << "id: " << dfiber::Thread::GetThreadId() << ", name: " << dfiber::Thread::GetName();
    std::cout << ", this id: " << dfiber::Thread::GetThis()->getId() << ", this name: " << dfiber::Thread::GetThis()->getName() << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(3));
}


int main()
{
    std::vector<std::shared_ptr<dfiber::Thread>> thrs;

    for (int i = 0; i < 5; ++i)
    {
        std::shared_ptr<dfiber::Thread> thr = std::make_shared<dfiber::Thread>(&func, "thread_" + std::to_string(i));

        thrs.push_back(thr);
    }

    for (int i = 0; i < 5; ++i)
    {
        thrs[i]->join();
    }


    return 0;
}
