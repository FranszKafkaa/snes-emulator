#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "libretro.h"

namespace snes {

struct VideoFrame {
    std::vector<uint8_t> pixels;
    unsigned width = 0;
    unsigned height = 0;
    size_t pitch = 0;
    retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
    uint64_t serial = 0;
};

class VideoPipeline {
public:
    using Processor = std::function<void(const VideoFrame &)>;

    explicit VideoPipeline(Processor processor);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline &) = delete;
    VideoPipeline &operator=(const VideoPipeline &) = delete;

    void start();
    void stop();
    void submit(std::shared_ptr<VideoFrame> frame);
    std::shared_ptr<const VideoFrame> latest() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class AudioPipeline {
public:
    explicit AudioPipeline(SDL_AudioDeviceID device);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline &) = delete;
    AudioPipeline &operator=(const AudioPipeline &) = delete;

    void start();
    void stop();
    void submit(std::span<const int16_t> samples);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snes
