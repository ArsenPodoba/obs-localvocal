#include "cloud-stt.h"
#include <cmath>
#include <chrono>
#include <vector>
#include <cstring>
#include <algorithm>
#include <grpcpp/grpcpp.h>
#include "google/cloud/speech/v1/cloud_speech.grpc.pb.h"

#include "../transcription-utils.h"
#include "../transcription-filter-data.h"
#include "../transcription-filter-callbacks.h"

using google::cloud::speech::v1::Speech;
using google::cloud::speech::v1::StreamingRecognizeRequest;
using google::cloud::speech::v1::StreamingRecognizeResponse;
using google::cloud::speech::v1::RecognitionConfig;

struct GoogleSttStreamer::Impl {
	std::shared_ptr<grpc::Channel> channel;
	std::unique_ptr<Speech::Stub> stub;
	std::unique_ptr<
		grpc::ClientReaderWriter<StreamingRecognizeRequest, StreamingRecognizeResponse>>
		stream;
	grpc::ClientContext ctx;
};

static inline int16_t f32_to_s16(float x)
{
	if (x > 1.0f)
		x = 1.0f;
	if (x < -1.0f)
		x = -1.0f;

	return static_cast<int16_t>(std::lroundf(x * 32767.0f));
}

bool GoogleSttStreamer::shouldRotate()
{
	return running_ && (now_ms() - start_ms_) > rotate_interval_ms_;
}

GoogleSttStreamer::GoogleSttStreamer(transcription_filter_data *gf) : gf_(gf) {}
GoogleSttStreamer::~GoogleSttStreamer()
{
	stop();
}

bool GoogleSttStreamer::start(const std::string &api_key, const std::string &lang)
{
	if (running_)
		return true;
	obs_log(LOG_INFO, "CloudSTT: [start] ==> Starting stream (lang: %s)...", lang.c_str());

	api_key_ = api_key;
	language_code_ = lang;
	start_ms_ = now_ms();
	impl_ = std::make_unique<Impl>();

	auto creds = grpc::SslCredentials(grpc::SslCredentialsOptions{});
	impl_->channel = grpc::CreateChannel("speech.googleapis.com", creds);
	impl_->stub = Speech::NewStub(impl_->channel);

	impl_->ctx.AddMetadata("x-goog-api-key", api_key_);
	impl_->stream = impl_->stub->StreamingRecognize(&impl_->ctx);
	if (!impl_->stream) {
		obs_log(LOG_ERROR, "CloudSTT: [start] ==> Failed to create stream.");
		return false;
	}

	StreamingRecognizeRequest cfgReq;
	auto *streaming_cfg = cfgReq.mutable_streaming_config();
	auto *cfg = streaming_cfg->mutable_config();
	cfg->set_language_code(language_code_);
	cfg->set_sample_rate_hertz(16000);
	cfg->set_audio_channel_count(1);
	cfg->set_encoding(RecognitionConfig::LINEAR16);
	cfg->set_enable_automatic_punctuation(true);
	streaming_cfg->set_interim_results(true);
	streaming_cfg->set_single_utterance(false);

	obs_log(LOG_INFO, "CloudSTT: [start] ==> Sending initial config...");
	if (!impl_->stream->Write(cfgReq)) {
		obs_log(LOG_ERROR, "CloudSTT: [start] ==> FAILED to write config to stream!");
		if (gf_) {
			DetectionResultWithText out{};
			out.text =
				"CloudSTT error: failed to write config (check API key/billing).";
			out.result = DETECTION_RESULT_PARTIAL;
			out.language = "en";
			out.start_timestamp_ms = out.end_timestamp_ms = now_ms();
			set_text_callback(out.end_timestamp_ms, gf_, out);
		}
		impl_.reset();
		return false;
	}

	obs_log(LOG_INFO, "CloudSTT: [start] ==> Starting results thread...");
	running_ = true;
	reader_ = std::thread(&GoogleSttStreamer::resultsThread, this);
	return true;
}

void GoogleSttStreamer::stop()
{
	running_ = false;
	obs_log(LOG_INFO, "CloudSTT: [stop] ==> Stopping stream...");

	if (impl_ && impl_->stream) {
		obs_log(LOG_INFO, "CloudSTT: [stop] ==> Closing write stream (WritesDone).");
		impl_->stream->WritesDone();
		impl_->ctx.TryCancel();
	}

	obs_log(LOG_INFO, "CloudSTT: [stop] ==> Waiting for results thread to join...");
	if (reader_.joinable())
		reader_.join();

	impl_.reset();
	{
		std::lock_guard<std::mutex> lk(mtx_);
		pending_.clear();
	}
	obs_log(LOG_INFO, "CloudSTT: [stop] ==> Stream stopped successfully.");
}

void GoogleSttStreamer::requestRotate()
{
	bool expected = false;
	if (!rotating_.compare_exchange_strong(expected, true))
		return;
	if (rotator_.joinable()) {
		if (std::this_thread::get_id() != rotator_.get_id())
			rotator_.join();
		else
			rotator_.detach();
	}
	obs_log(LOG_INFO, "CloudSTT: [rotate] ==> Scheduling rotation...");
	rotator_ = std::thread(&GoogleSttStreamer::doRotate, this);
}

void GoogleSttStreamer::doRotate()
{
	obs_log(LOG_INFO, "CloudSTT: [rotate] ==> Background rotation started.");
	stop();
	obs_log(LOG_INFO, "CloudSTT: [rotate] ==> Stop finished, starting new stream...");
	bool ok = start(api_key_, language_code_);
	obs_log(LOG_INFO, "CloudSTT: [rotate] ==> Start result: %s", ok ? "OK" : "FAILED");
	rotating_ = false;
}

void GoogleSttStreamer::pushFloat16k(const float *data, size_t n)
{
	if (!running_ || !data || n == 0)
		return;

	if (shouldRotate())
		requestRotate();
	if (rotating_)
		return;

	constexpr size_t CHUNK = 1600;
	std::vector<std::vector<int16_t>> chunks_to_send;

	{
		std::lock_guard<std::mutex> lk(mtx_);
		pending_.insert(pending_.end(), data, data + n);

		while (pending_.size() >= CHUNK) {
			std::vector<int16_t> s16(CHUNK);
			for (size_t i = 0; i < CHUNK; ++i)
				s16[i] = f32_to_s16(pending_[i]);
			pending_.erase(pending_.begin(), pending_.begin() + CHUNK);
			chunks_to_send.emplace_back(std::move(s16));
			sent_samples_.fetch_add(CHUNK, std::memory_order_relaxed);
		}
	}

	for (auto &s16 : chunks_to_send) {
		if (!impl_ || !impl_->stream) {
			running_ = false;
			break;
		}
		StreamingRecognizeRequest req;
		req.set_audio_content(reinterpret_cast<const char *>(s16.data()),
				      s16.size() * sizeof(int16_t));
		if (!impl_->stream->Write(req)) {
			obs_log(LOG_ERROR,
				"CloudSTT: [pushFloat16k] ==> FAILED to write audio to stream!");
			running_ = false;
			break;
		}
	}
}

void GoogleSttStreamer::resultsThread()
{
	if (!impl_ || !impl_->stream)
		return;
	obs_log(LOG_INFO, "CloudSTT: [resultsThread] ==> Thread started, waiting for responses...");

	StreamingRecognizeResponse resp;
	while (running_ && impl_->stream->Read(&resp)) {
		std::string text;
		bool is_final = false;

		for (const auto &r : resp.results()) {
			if (r.alternatives_size() > 0) {
				text += r.alternatives(0).transcript();
			}
			if (r.is_final())
				is_final = true;
		}

		if (!text.empty()) {
			if (gf_) {
				DetectionResultWithText out{};
				out.text = text;
				out.result = is_final ? DETECTION_RESULT_SPEECH
						      : DETECTION_RESULT_PARTIAL;
				if (!gf_->cloud_transcription_language.empty()) {
					out.language = gf_->cloud_transcription_language;
				} else if (gf_->whisper_params.language &&
					   std::strlen(gf_->whisper_params.language) > 0) {
					out.language = gf_->whisper_params.language;
				} else {
					out.language = "en";
				}
				out.start_timestamp_ms = 0;
				out.end_timestamp_ms = now_ms();
				set_text_callback(out.end_timestamp_ms, gf_, out);
			}
		}
		resp.Clear();
	}

	if (impl_ && impl_->stream) {
		auto status = impl_->stream->Finish();
		if (!status.ok() && gf_) {
			std::string msg = "CloudSTT error: " + status.error_message();
			obs_log(LOG_ERROR,
				"CloudSTT: [resultsThread] ==> Stream finished with error: %s",
				status.error_message().c_str());
			DetectionResultWithText out{};
			out.text = std::move(msg);
			out.result = DETECTION_RESULT_PARTIAL;
			out.language = "en";
			out.start_timestamp_ms = out.end_timestamp_ms = now_ms();
			set_text_callback(out.end_timestamp_ms, gf_, out);
		} else {
			obs_log(LOG_INFO, "CloudSTT: [resultsThread] ==> Stream finished: %s",
				status.ok() ? "OK" : status.error_message().c_str());
		}
	}

	running_ = false;
	obs_log(LOG_INFO, "CloudSTT: [resultsThread] ==> Read loop finished. Thread exiting.");
}