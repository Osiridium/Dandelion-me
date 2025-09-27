#include "render_engine.h"

#include <algorithm>

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

void SpinLock::lock()
{
    while (true) {
        if (!this->locked.test_and_set())
            return;
        do {
            for (std::atomic<int> i = 0; i < 150; ++i);
        } while (this->locked.test());
    }
}

void SpinLock::unlock()
{
    this->locked.clear();
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

void FrameBuffer::set_pixel(int index, float depth, const Eigen::Vector3f& color)
{
    // spin locks lock
    spin_locks[index].lock();

    depth_buffer[index] = depth;
    color_buffer[index] = color;

    // spin locks unlock
    spin_locks[index].unlock();
}
