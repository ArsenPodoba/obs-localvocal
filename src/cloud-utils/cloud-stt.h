#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

struct transcription_filter_data;

class GoogleSttStreamer {
public:
	explicit GoogleSttStreamer(transcription_filter_data *gf);
	~GoogleSttStreamer();

	bool start(const std::string &api_key, const std::string &language_code);
	void stop();
	void pushFloat16k(const float *data, size_t n);

private:
	void resultsThread();

	bool shouldRotate();
	void requestRotate();
	void doRotate();

	transcription_filter_data *gf_{};
	std::mutex mtx_;
	std::deque<float> pending_;
	std::thread reader_;
	std::atomic<bool> running_{false};

	struct Impl;
	std::unique_ptr<Impl> impl_;

	std::atomic<uint64_t> sent_samples_{0};
	std::string language_code_;
	std::string api_key_;

	// rotation state
	std::atomic<bool> rotating_{false};
	std::thread rotator_;
	uint64_t start_ms_{0};
	uint64_t rotate_interval_ms_{240000}; // 4 minutes
};