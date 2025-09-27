#include "render_engine.h"

#include <algorithm>
#include <limits>

Eigen::Vector3f RenderEngine::background_color(RGB_COLOR(100, 100, 100));

RenderEngine::RenderEngine() :
    width(0.0f),
    height(0.0f),
    n_threads(4),
    rasterizer_mode(PipelineMode::Parallel),
    rasterizer_parallel_threads(2)
{
    // unique pointer to Rasterizer Renderer
    rasterizer_render = std::make_unique<RasterizerRenderer>(
        *this, rasterizer_parallel_threads, rasterizer_parallel_threads, rasterizer_parallel_threads
    );
    // unique pointer to Whitted Style Renderer
    whitted_render = std::make_unique<WhittedRenderer>(*this);

    set_rasterizer_pipeline_mode(rasterizer_mode);
    set_rasterizer_parallel_thread_count(rasterizer_parallel_threads);
}

void RenderEngine::set_rasterizer_pipeline_mode(PipelineMode mode)
{
    rasterizer_mode = mode;
    if (rasterizer_render) {
        rasterizer_render->set_pipeline_mode(mode);
    }
}

PipelineMode RenderEngine::get_rasterizer_pipeline_mode() const
{
    return rasterizer_mode;
}

void RenderEngine::set_rasterizer_parallel_thread_count(int threads)
{
    int clamped = std::max(1, threads);
    rasterizer_parallel_threads = clamped;
    if (rasterizer_render) {
        rasterizer_render->set_parallel_thread_count(clamped);
    }
}

int RenderEngine::get_rasterizer_parallel_thread_count() const
{
    if (rasterizer_render) {
        return rasterizer_render->parallel_thread_count();
    }
    return rasterizer_parallel_threads;
}

// choose render type
void RenderEngine::render(Scene& scene, RendererType type)
{
    switch (type) {
    case RendererType::RASTERIZER: rasterizer_render->render(scene); break;
    // case RendererType::RASTERIZER_MT: rasterizer_render->render_mt(scene); break;
    case RendererType::WHITTED_STYLE: whitted_render->render(scene); break;
    default: break;
    }
}

SpinLock::SpinLock() noexcept : locked(ATOMIC_FLAG_INIT)
{
    locked.clear();
}

SpinLock::SpinLock(const SpinLock&) noexcept : SpinLock() {}

SpinLock& SpinLock::operator=(const SpinLock&) noexcept
{
    locked.clear();
    return *this;
}

SpinLock::SpinLock(SpinLock&&) noexcept : SpinLock() {}

SpinLock& SpinLock::operator=(SpinLock&&) noexcept
{
    locked.clear();
    return *this;
}

void SpinLock::lock()
{
    while (true) {
        if (!this->locked.test_and_set(std::memory_order_acquire))
            return;
        do {
            for (int i = 0; i < 150; ++i)
                ;
        } while (this->locked.test(std::memory_order_relaxed));
    }
}

void SpinLock::unlock()
{
    this->locked.clear(std::memory_order_release);
}

void FrameBuffer::clear(BufferType buff)
{
    if ((buff & BufferType::Color) == BufferType::Color) {
        fill(color_buffer.begin(), color_buffer.end(), RenderEngine::background_color * 255.0f);
    }
    if ((buff & BufferType::Depth) == BufferType::Depth) {
        fill(depth_buffer.begin(), depth_buffer.end(), std::numeric_limits<float>::infinity());
    }
}

void FrameBuffer::resize(int new_width, int new_height)
{
    width  = std::max(0, new_width);
    height = std::max(0, new_height);
    std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    color_buffer.assign(pixel_count, Eigen::Vector3f(0, 0, 0));
    depth_buffer.assign(pixel_count, std::numeric_limits<float>::infinity());
    spin_locks.assign(pixel_count, SpinLock());
}

bool FrameBuffer::set_pixel(int index, float depth, const Eigen::Vector3f& color)
{
    return set_pixel_lazy(index, depth, [color]() { return color; });
}
