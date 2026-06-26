#include "media_pipeline.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace snes {

class VideoPipeline::Impl {
public:
  explicit Impl(Processor processor) : processor_(std::move(processor)) {}

  void start() {
    std::lock_guard lock(mutex_);
    if (running_)
      return;
    running_ = true;
    worker_ = std::thread([this] { run(); });
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      running_ = false;
    }
    ready_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

  void submit(std::shared_ptr<VideoFrame> frame) {
    {
      std::lock_guard lock(mutex_);
      frame->serial = next_serial_++;
      pending_ = std::move(frame);
    }
    ready_.notify_one();
  }

  std::shared_ptr<const VideoFrame> latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
  }

private:
  void run() {
    while (true) {
      std::shared_ptr<VideoFrame> frame;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !running_ || pending_; });
        if (!running_ && !pending_)
          return;
        frame = std::move(pending_);
      }
      if (processor_)
        processor_(*frame);
      {
        std::lock_guard lock(mutex_);
        latest_ = std::move(frame);
      }
    }
  }

  Processor processor_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::shared_ptr<VideoFrame> pending_;
  std::shared_ptr<VideoFrame> latest_;
  std::thread worker_;
  bool running_ = false;
  uint64_t next_serial_ = 1;
};

VideoPipeline::VideoPipeline(Processor processor)
    : impl_(std::make_unique<Impl>(std::move(processor))) {}
VideoPipeline::~VideoPipeline() { stop(); }
void VideoPipeline::start() { impl_->start(); }
void VideoPipeline::stop() { impl_->stop(); }
void VideoPipeline::submit(std::shared_ptr<VideoFrame> frame) {
  impl_->submit(std::move(frame));
}
std::shared_ptr<const VideoFrame> VideoPipeline::latest() const {
  return impl_->latest();
}

class AudioPipeline::Impl {
public:
  Impl(SDL_AudioDeviceID device, int sample_rate)
      : device_(device),
        bytes_per_second_(static_cast<Uint32>(
            std::max(sample_rate, 1) * 2 * sizeof(int16_t))) {}

  void start() {
    if (device_)
      SDL_PauseAudioDevice(device_, 1);
  }

  void stop() {
    if (!device_)
      return;
    SDL_PauseAudioDevice(device_, 1);
    SDL_ClearQueuedAudio(device_);
    started_ = false;
  }

  void submit(std::span<const int16_t> samples) {
    if (!device_ || samples.empty())
      return;

    const Uint32 queued = SDL_GetQueuedAudioSize(device_);
    if (started_ && queued == 0) {
      SDL_PauseAudioDevice(device_, 1);
      started_ = false;
    }

    const Uint32 max_queued = bytes_per_second_ * 3 / 4;
    if (queued > max_queued) {
      return;
    }

    SDL_QueueAudio(device_, samples.data(),
                   static_cast<Uint32>(samples.size() * sizeof(int16_t)));

    if (!started_ && SDL_GetQueuedAudioSize(device_) >= target_queued_bytes()) {
      SDL_PauseAudioDevice(device_, 0);
      started_ = true;
    }
  }

  uint32_t queued_milliseconds() const {
    if (!device_ || bytes_per_second_ == 0)
      return 0;
    return SDL_GetQueuedAudioSize(device_) * 1000U / bytes_per_second_;
  }

private:
  Uint32 target_queued_bytes() const {
    return bytes_per_second_ * 180U / 1000U;
  }

  SDL_AudioDeviceID device_ = 0;
  Uint32 bytes_per_second_ = 48000 * 2 * sizeof(int16_t);
  bool started_ = false;
};

AudioPipeline::AudioPipeline(SDL_AudioDeviceID device, int sample_rate)
    : impl_(std::make_unique<Impl>(device, sample_rate)) {}
AudioPipeline::~AudioPipeline() { stop(); }
void AudioPipeline::start() { impl_->start(); }
void AudioPipeline::stop() { impl_->stop(); }
void AudioPipeline::submit(std::span<const int16_t> samples) {
  impl_->submit(samples);
}
uint32_t AudioPipeline::queued_milliseconds() const {
  return impl_->queued_milliseconds();
}

} // namespace snes
