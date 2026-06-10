#include "media_pipeline.h"

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
  explicit Impl(SDL_AudioDeviceID device) : device_(device) {}

  void start() {
    std::lock_guard lock(mutex_);
    if (running_ || !device_)
      return;
    running_ = true;
    worker_ = std::thread([this] { run(); });
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      running_ = false;
      blocks_.clear();
    }
    ready_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

  void submit(std::span<const int16_t> samples) {
    if (!device_ || samples.empty())
      return;
    std::vector<int16_t> block(samples.begin(), samples.end());
    {
      std::lock_guard lock(mutex_);
      constexpr size_t max_blocks = 8;
      if (blocks_.size() >= max_blocks)
        blocks_.pop_front();
      blocks_.push_back(std::move(block));
    }
    ready_.notify_one();
  }

private:
  void run() {
    while (true) {
      std::vector<int16_t> block;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !running_ || !blocks_.empty(); });
        if (!running_ && blocks_.empty())
          return;
        block = std::move(blocks_.front());
        blocks_.pop_front();
      }
      constexpr Uint32 max_queued_bytes = 32040 * 2 * sizeof(int16_t) / 4;
      if (SDL_GetQueuedAudioSize(device_) > max_queued_bytes) {
        SDL_ClearQueuedAudio(device_);
      }
      SDL_QueueAudio(device_, block.data(),
                     static_cast<Uint32>(block.size() * sizeof(int16_t)));
    }
  }

  SDL_AudioDeviceID device_ = 0;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::vector<int16_t>> blocks_;
  std::thread worker_;
  bool running_ = false;
};

AudioPipeline::AudioPipeline(SDL_AudioDeviceID device)
    : impl_(std::make_unique<Impl>(device)) {}
AudioPipeline::~AudioPipeline() { stop(); }
void AudioPipeline::start() { impl_->start(); }
void AudioPipeline::stop() { impl_->stop(); }
void AudioPipeline::submit(std::span<const int16_t> samples) {
  impl_->submit(samples);
}

} // namespace snes
