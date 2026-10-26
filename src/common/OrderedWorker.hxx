#pragma once
// One thread that runs jobs in the order they were posted (the replay
// channel's queue folder is its user). Submit posts a job and gives the
// future of its answer, which the game thread may look at without waiting;
// Ask waits for it, from another thread, never the game's. The job alone owns
// the promise of that answer, so one that throws answers `failed`, and one
// dropped at Stop leaves a broken promise (Ask answers `failed`): no caller is
// left waiting. Portable, so ReplayChannelTest runs it.
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
	std::future<T> Submit(std::function<T()> job, T failed) {
		auto promise = std::make_shared<std::promise<T>>();
		auto answer = promise->get_future();
		// Dropped unrun when the worker has stopped: its promise breaks.
		Post([promise = std::move(promise), job = std::move(job), failed]() {
			try { promise->set_value(job()); }
			catch (...) { try { promise->set_value(failed); } catch (...) {} }
		});
		return answer;
	}
	template <class T>
	T Ask(std::function<T()> job, T failed) {
		auto answer = Submit<T>(std::move(job), failed);
		try { return answer.get(); } catch (...) { return failed; }
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
