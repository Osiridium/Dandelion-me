#include <cstddef>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "render_engine.h"
#include "../scene/light.h"
#include "../utils/logger.h"

using std::chrono::steady_clock;
using std::size_t;
using Eigen::Vector3f;
using Eigen::Vector4f;

// vertex processor & rasterizer & fragement processor can visit
// all the static variables below from Uniforms structure
Eigen::Matrix4f Uniforms::MVP;
Eigen::Matrix4f Uniforms::inv_trans_M;
int             Uniforms::width  = 0;
int             Uniforms::height = 0;

GL::Material     ini_material = GL::Material();
std::list<Light> ini_lights   = {};
Camera           ini_camera = Camera(Vector3f::Ones(), Vector3f::Ones(), 0.1f, 10.0f, 45.0f, 1.33f);

GL::Material&     Uniforms::material = ini_material;
std::list<Light>& Uniforms::lights   = ini_lights;
Camera&           Uniforms::camera   = ini_camera;

std::mutex                        Context::vertex_queue_mutex;
std::mutex                        Context::rasterizer_queue_mutex;
std::queue<VertexShaderPayload>   Context::vertex_shader_output_queue;
std::condition_variable           Context::vertex_output_cv;
std::condition_variable           Context::rasterizer_output_cv;
std::atomic<std::size_t>          Context::rasterizer_triangle_index{0};
std::size_t                       Context::rasterizer_triangle_count = 0;

std::atomic<bool> Context::vertex_finish{false};
std::atomic<bool> Context::rasterizer_finish{false};
std::atomic<bool> Context::fragment_finish{false};

std::atomic<std::size_t>           Context::vertex_input_index{0};
std::atomic<std::size_t>           Context::vertex_processed_count{0};
std::size_t                        Context::vertex_total_count = 0;
std::size_t                        Context::vertex_flush_index = 0;
std::vector<VertexShaderPayload>   Context::vertex_shader_output_buffer;
std::vector<char>                  Context::vertex_output_ready;
std::queue<std::vector<FragmentShaderPayload>> Context::fragment_batch_queue;

FrameBuffer Context::frame_buffer(Uniforms::width, Uniforms::height);

FrameBuffer::FrameBuffer(int width, int height) :
    width(width), height(height), color_buffer(width * height, Eigen::Vector3f(0, 0, 0)),
    depth_buffer(width * height, std::numeric_limits<float>::infinity()), spin_locks(width * height)
{
    for (auto& lock: spin_locks) {
        lock.unlock();
    }
}

// 光栅化渲染器的构造函数
RasterizerRenderer::RasterizerRenderer(
    RenderEngine& engine, int num_vertex_threads, int num_rasterizer_threads,
    int num_fragment_threads
) :
    width(engine.width), height(engine.height), n_vertex_threads(num_vertex_threads),
    n_rasterizer_threads(num_rasterizer_threads), n_fragment_threads(num_fragment_threads),
    pipeline_mode(PipelineMode::Parallel),
    vertex_processor(), rasterizer(), fragment_processor(), rendering_res(engine.rendering_res)
{
    logger = get_logger("Rasterizer Renderer");
}

void RasterizerRenderer::set_pipeline_mode(PipelineMode mode)
{
    pipeline_mode = mode;
}

void RasterizerRenderer::set_parallel_thread_count(int count)
{
    int clamped          = std::max(1, count);
    n_vertex_threads     = clamped;
    n_rasterizer_threads = clamped;
    n_fragment_threads   = clamped;
}

int RasterizerRenderer::parallel_thread_count() const
{
    return n_vertex_threads;
}

// 光栅化渲染器的渲染调用接口
void RasterizerRenderer::render(const Scene& scene)
{
    Uniforms::width  = static_cast<int>(width);
    Uniforms::height = static_cast<int>(height);
    Context::frame_buffer = FrameBuffer(Uniforms::width, Uniforms::height);
    Context::frame_buffer.clear(BufferType::Color | BufferType::Depth);
    rendering_res.clear();

    auto begin_time = steady_clock::now();

    vertex_processor.vertex_shader_ptr     = vertex_shader;
    fragment_processor.fragment_shader_ptr = phong_fragment_shader;

    int vertex_threads     = pipeline_mode == PipelineMode::Serial ? 1 : n_vertex_threads;
    int rasterizer_threads = pipeline_mode == PipelineMode::Serial ? 1 : n_rasterizer_threads;
    int fragment_threads   = pipeline_mode == PipelineMode::Serial ? 1 : n_fragment_threads;

    const char* mode_name = pipeline_mode == PipelineMode::Serial ? "Serial" : "Parallel";
    logger->info(
        "Rasterizer rendering started (mode: {}, vertex: {}, rasterizer: {}, fragment: {})",
        mode_name, vertex_threads, rasterizer_threads, fragment_threads
    );

    Camera cam = scene.camera;

    for (const auto& group: scene.groups) {
        for (const auto& object: group->objects) {
            const auto& vertices = object->mesh.vertices.data;
            const auto& normals  = object->mesh.normals.data;
            const auto& faces    = object->mesh.faces.data;

            std::size_t num_indices   = faces.size();
            std::size_t triangle_count = num_indices / 3;

            if (num_indices == 0 || triangle_count == 0) {
                continue;
            }

            Uniforms::MVP         = cam.projection() * cam.view() * object->model();
            Uniforms::inv_trans_M = object->model().inverse().transpose();
            Uniforms::width       = static_cast<int>(width);
            Uniforms::height      = static_cast<int>(height);
            Uniforms::material    = object->mesh.material;
            Uniforms::lights      = scene.lights;
            Uniforms::camera      = cam;

            std::vector<VertexShaderPayload> vertex_outputs(num_indices);
            Context::vertex_finish.store(false, std::memory_order_relaxed);

            auto process_vertex = [&](int thread_id) {
                for (std::size_t idx = thread_id; idx < num_indices; idx += vertex_threads) {
                    unsigned int vertex_id = faces[idx];
                    VertexShaderPayload payload;
                    payload.world_position = Vector4f(
                        vertices[3 * vertex_id], vertices[3 * vertex_id + 1],
                        vertices[3 * vertex_id + 2], 1.0f
                    );
                    payload.viewport_position = Vector4f::Zero();
                    payload.normal             = Vector3f(
                        normals[3 * vertex_id], normals[3 * vertex_id + 1], normals[3 * vertex_id + 2]
                    );
                    VertexShaderPayload transformed = vertex_processor.vertex_shader_ptr(payload);
                    vertex_outputs[idx]              = transformed;
                }
            };

            if (vertex_threads == 1) {
                process_vertex(0);
            } else {
                std::vector<std::thread> vertex_workers;
                vertex_workers.reserve(vertex_threads);
                for (int i = 0; i < vertex_threads; ++i) {
                    vertex_workers.emplace_back(process_vertex, i);
                }
                for (auto& worker: vertex_workers) {
                    worker.join();
                }
            }

            Context::vertex_shader_output_buffer = std::move(vertex_outputs);
            Context::vertex_total_count           = num_indices;
            Context::vertex_finish.store(true, std::memory_order_release);

            Context::rasterizer_triangle_index.store(0);
            Context::rasterizer_triangle_count = triangle_count;
            Context::rasterizer_finish.store(false, std::memory_order_relaxed);
            Context::fragment_finish.store(false, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock(Context::rasterizer_queue_mutex);
                Context::fragment_batch_queue = {};
            }

            std::vector<std::thread> fragment_workers;
            fragment_workers.reserve(fragment_threads);
            for (int i = 0; i < fragment_threads; ++i) {
                fragment_workers.emplace_back(&FragmentProcessor::worker_thread, &fragment_processor);
            }

            std::vector<std::thread> raster_workers;
            raster_workers.reserve(rasterizer_threads);
            for (int i = 0; i < rasterizer_threads; ++i) {
                raster_workers.emplace_back(&Rasterizer::worker_thread, &rasterizer);
            }

            for (auto& worker: raster_workers) {
                worker.join();
            }

            {
                std::lock_guard<std::mutex> done_lock(Context::rasterizer_queue_mutex);
                Context::rasterizer_finish.store(true, std::memory_order_release);
            }
            Context::rasterizer_output_cv.notify_all();

            for (auto& worker: fragment_workers) {
                worker.join();
            }

            Context::fragment_finish.store(true, std::memory_order_release);
            Context::vertex_shader_output_buffer.clear();
        }
    }

    auto end_time = steady_clock::now();
    double rendering_duration = std::chrono::duration<double>(end_time - begin_time).count();

    logger->info(
        "Rasterizer rendering finished (mode: {}) in {:.6f} seconds", mode_name,
        rendering_duration
    );

    for (std::size_t i = 0; i < Context::frame_buffer.depth_buffer.size(); ++i) {
        rendering_res.push_back(static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].x()));
        rendering_res.push_back(static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].y()));
        rendering_res.push_back(static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].z()));
    }
}

void VertexProcessor::input_vertices(const Vector4f& positions, const Vector3f& normals)
{
    VertexShaderPayload payload;
    payload.world_position  = positions;
    payload.viewport_position = Eigen::Vector4f::Zero();
    payload.normal          = normals;
    if (positions.w() == -1.0f) {
        payload.terminate = true;
    } else {
        payload.vertex_index = Context::vertex_input_index.fetch_add(1);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        vertex_queue.push(payload);
    }

    if (payload.terminate) {
        queue_cv.notify_all();
    } else {
        queue_cv.notify_one();
    }
}

void VertexProcessor::worker_thread()
{
    while (true) {
        VertexShaderPayload payload;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [&] { return !vertex_queue.empty(); });
            payload = vertex_queue.front();
            vertex_queue.pop();
        }

        if (payload.terminate) {
            queue_cv.notify_all();
            return;
        }

        VertexShaderPayload output_payload = vertex_shader_ptr(payload);

        {
            std::lock_guard<std::mutex> output_lock(Context::vertex_queue_mutex);
            if (payload.vertex_index < Context::vertex_shader_output_buffer.size()) {
                Context::vertex_shader_output_buffer[payload.vertex_index] = output_payload;
                Context::vertex_output_ready[payload.vertex_index]          = 1;
                while (
                    Context::vertex_flush_index < Context::vertex_total_count
                    && Context::vertex_output_ready[Context::vertex_flush_index]
                ) {
                    Context::vertex_shader_output_queue.push(
                        Context::vertex_shader_output_buffer[Context::vertex_flush_index]
                    );
                    ++Context::vertex_flush_index;
                }
            }
        }

        Context::vertex_output_cv.notify_all();

        std::size_t processed = ++Context::vertex_processed_count;
        if (processed == Context::vertex_total_count) {
            Context::vertex_finish.store(true, std::memory_order_release);
            Context::vertex_output_cv.notify_all();
        }
    }
}

void FragmentProcessor::worker_thread()
{
    while (true) {
        std::vector<FragmentShaderPayload> batch;
        {
            std::unique_lock<std::mutex> lock(Context::rasterizer_queue_mutex);
            Context::rasterizer_output_cv.wait(lock, [] {
                return !Context::fragment_batch_queue.empty()
                    || Context::rasterizer_finish.load(std::memory_order_acquire);
            });

            if (Context::fragment_batch_queue.empty()) {
                if (Context::rasterizer_finish.load(std::memory_order_acquire)) {
                    Context::fragment_finish.store(true, std::memory_order_release);
                    return;
                }
                continue;
            }

            batch = std::move(Context::fragment_batch_queue.front());
            Context::fragment_batch_queue.pop();
        }

        for (const auto& fragment: batch) {
            int index = (Uniforms::height - 1 - fragment.y) * Uniforms::width + fragment.x;
            if (fragment.depth > Context::frame_buffer.depth_buffer[index]) {
                continue;
            }
            Eigen::Vector3f color = fragment_shader_ptr(
                fragment, Uniforms::material, Uniforms::lights, Uniforms::camera
            );
            Context::frame_buffer.set_pixel(index, fragment.depth, color);
        }
    }
}
