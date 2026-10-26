#pragma once
// One thread that runs jobs in the order they were posted (the replay
// channel's queue folder is its user). Submit posts a job and gives the
// future of its answer, which the game thread may look at without waiting;
// Ask waits for it, from another thread, never the game's. The job alone owns
// that answer: one that throws puts its exception in it, and one dropped at
// Stop breaks it, so no caller is left waiting (Ask answers `failed` for
// both). Portable, so ReplayChannelTest runs it.
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace sf4e {

class OrderedWorker {
public:
	OrderedWorker() : thread_([this] { Run(); }) {}
	~OrderedWorker() { Stop(); }
	OrderedWorker(const OrderedWorker&) = delete;
	OrderedWorker& operator=(const OrderedWorker&) = delete;

	// False once stopped: the job is dropped.
	bool Post(std::function<void()> job) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (stop_) return false;
		jobs_.push_back(std::move(job));
		wake_.notify_one();
		return true;
	}
	template <class T>
	std::future<T> Submit(std::function<T()> job) {
		auto task = std::make_shared<std::packaged_task<T()>>(std::move(job));
		auto answer = task->get_future();
		// Dropped unrun when the worker has stopped: its answer breaks.
		Post([task = std::move(task)] { (*task)(); });
		return answer;
	}
	template <class T>
	T Ask(std::function<T()> job, T failed) {
		try { return Submit<T>(std::move(job)).get(); } catch (...) { return failed; }
	}
	// Drops the jobs not started, which answers every Ask waiting on one, then
	// waits for the job under way.
	void Stop() {
		std::deque<std::function<void()>> dropped;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
			dropped.swap(jobs_);
			wake_.notify_all();
		}
		dropped.clear();
		if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
	}

private:
	void Run() noexcept {
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
				if (stop_) return;
				job = std::move(jobs_.front());
				jobs_.pop_front();
			}
			try { job(); } catch (...) {}
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::function<void()>> jobs_;
	bool stop_ = false;
	std::thread thread_;
};

}
