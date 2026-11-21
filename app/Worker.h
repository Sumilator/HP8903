// SPDX-License-Identifier: GPL-2.0-only
//
// One background thread that owns all GPIB traffic, so the UI never blocks.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class Worker {
public:
    Worker() : th_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(job));
        }
        cv_.notify_one();
    }
    bool busy() const {
        std::lock_guard<std::mutex> lk(m_);
        return running_ || !q_.empty();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop_front();
                running_ = true;
            }
            job();
            std::lock_guard<std::mutex> lk(m_);
            running_ = false;
        }
    }
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false, running_ = false;
    std::thread th_;
};
