#include "parallel.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace absent {

namespace {

struct Pool {
    std::mutex gate;
    std::mutex state;
    std::condition_variable wake;
    std::condition_variable done;
    const std::function<void(size_t)>* job = nullptr;
    size_t count = 0;
    std::atomic<size_t> next{0};
    size_t round = 0;
    size_t finished = 0;
    size_t helpers = 0;

    Pool() {
        helpers = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 8) - 1;
        for (size_t i = 0; i < helpers; ++i) std::thread([this] { serve(); }).detach();
    }

    void drain() {
        for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) (*job)(i);
    }

    void serve() {
        size_t seen = 0;
        std::unique_lock<std::mutex> lock(state);
        for (;;) {
            wake.wait(lock, [&] { return round != seen; });
            seen = round;
            lock.unlock();
            drain();
            lock.lock();
            if (++finished == helpers) done.notify_all();
        }
    }

    void run(size_t jobs, const std::function<void(size_t)>& work) {
        std::unique_lock<std::mutex> only(gate, std::try_to_lock);
        if (!only.owns_lock() || !helpers || jobs < 2) {
            for (size_t i = 0; i < jobs; ++i) work(i);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state);
            job = &work;
            count = jobs;
            next = 0;
            finished = 0;
            ++round;
        }
        wake.notify_all();
        drain();
        std::unique_lock<std::mutex> lock(state);
        done.wait(lock, [&] { return finished == helpers; });
    }
};

Pool& pool() {
    static Pool* made = new Pool;
    return *made;
}

}

size_t parallelWidth() { return pool().helpers + 1; }

void parallelFor(size_t count, const std::function<void(size_t)>& job) { pool().run(count, job); }

}
