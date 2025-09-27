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
using duration   = std::chrono::duration<float>;
using time_point = std::chrono::time_point<steady_clock, duration>;
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
std::queue<FragmentShaderPayload> Context::rasterizer_output_queue;
std::condition_variable           Context::vertex_output_cv;
std::condition_variable           Context::rasterizer_output_cv;

volatile bool Context::vertex_finish     = false;
volatile bool Context::rasterizer_finish = false;
volatile bool Context::fragment_finish   = false;

std::atomic<std::size_t>           Context::vertex_input_index{0};
std::atomic<std::size_t>           Context::vertex_processed_count{0};
std::size_t                        Context::vertex_total_count = 0;
std::size_t                        Context::vertex_flush_index = 0;
std::vector<VertexShaderPayload>   Context::vertex_shader_output_buffer;
std::vector<char>                  Context::vertex_output_ready;

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
    Uniforms::width       = static_cast<int>(width);
    Uniforms::height      = static_cast<int>(height);
    Context::frame_buffer = FrameBuffer(Uniforms::width, Uniforms::height);
    // clear Color Buffer & Depth Buffer & rendering_res
    Context::frame_buffer.clear(BufferType::Color | BufferType::Depth);
    this->rendering_res.clear();
    // run time statistics
    time_point begin_time                  = steady_clock::now();
    Camera     cam                         = scene.camera;
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

    for (const auto& group: scene.groups) {
        for (const auto& object: group->objects) {
            Context::vertex_finish     = false;
            Context::rasterizer_finish = false;
            Context::fragment_finish   = false;

            {
                std::lock_guard<std::mutex> lock(Context::vertex_queue_mutex);
                while (!Context::vertex_shader_output_queue.empty()) {
                    Context::vertex_shader_output_queue.pop();
                }
            }
            {
                std::lock_guard<std::mutex> lock(Context::rasterizer_queue_mutex);
                while (!Context::rasterizer_output_queue.empty()) {
                    Context::rasterizer_output_queue.pop();
                }
            }

            const std::vector<float>&        vertices  = object->mesh.vertices.data;
            const std::vector<unsigned int>& faces     = object->mesh.faces.data;
            const std::vector<float>&        normals   = object->mesh.normals.data;
            size_t                           num_indices = faces.size();

            Context::vertex_input_index.store(0);
            Context::vertex_processed_count.store(0);
            Context::vertex_total_count = num_indices;
            Context::vertex_flush_index = 0;
            Context::vertex_shader_output_buffer.assign(num_indices, VertexShaderPayload());
            Context::vertex_output_ready.assign(num_indices, 0);

            if (num_indices == 0) {
                Context::vertex_finish     = true;
                Context::rasterizer_finish = true;
                Context::vertex_output_cv.notify_all();
                Context::rasterizer_output_cv.notify_all();
            }

            std::vector<std::thread> workers;
            for (int i = 0; i < vertex_threads; ++i) {
                workers.emplace_back(&VertexProcessor::worker_thread, &vertex_processor);
            }
            for (int i = 0; i < rasterizer_threads; ++i) {
                workers.emplace_back(&Rasterizer::worker_thread, &rasterizer);
            }
            for (int i = 0; i < fragment_threads; ++i) {
                workers.emplace_back(&FragmentProcessor::worker_thread, &fragment_processor);
            }

            // set Uniforms for vertex shader
            Uniforms::MVP         = cam.projection() * cam.view() * object->model();
            Uniforms::inv_trans_M = object->model().inverse().transpose();
            Uniforms::width       = static_cast<int>(this->width);
            Uniforms::height      = static_cast<int>(this->height);
            Uniforms::material    = object->mesh.material;
            Uniforms::lights      = scene.lights;
            Uniforms::camera      = scene.camera;

            // process vertices
            for (size_t i = 0; i < num_indices; i += 3) {
                for (size_t j = 0; j < 3; j++) {
                    size_t idx = faces[i + j];
                    vertex_processor.input_vertices(
                        Vector4f(
                            vertices[3 * idx], vertices[3 * idx + 1], vertices[3 * idx + 2], 1.0f
                        ),
                        Vector3f(normals[3 * idx], normals[3 * idx + 1], normals[3 * idx + 2])
                    );
                }
            }
            for (int i = 0; i < vertex_threads; ++i) {
                vertex_processor.input_vertices(
                    Eigen::Vector4f(0, 0, 0, -1.0f), Eigen::Vector3f::Zero()
                );
            }
            for (auto& worker: workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }
    }

    time_point end_time           = steady_clock::now();
    duration   rendering_duration = end_time - begin_time;

    this->logger->info(
        "Rasterizer rendering finished (mode: {}) in {:.6f} seconds", mode_name,
        rendering_duration.count()
    );

    for (long unsigned int i = 0; i < Context::frame_buffer.depth_buffer.size(); i++) {
        rendering_res.push_back(
            static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].x())
        );
        rendering_res.push_back(
            static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].y())
        );
        rendering_res.push_back(
            static_cast<unsigned char>(Context::frame_buffer.color_buffer[i].z())
        );
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
            Context::vertex_finish = true;
            Context::vertex_output_cv.notify_all();
        }
    }
}

void FragmentProcessor::worker_thread()
{
    while (true) {
        FragmentShaderPayload fragment;
        {
            std::unique_lock<std::mutex> lock(Context::rasterizer_queue_mutex);
            Context::rasterizer_output_cv.wait(lock, [&] {
                return !Context::rasterizer_output_queue.empty() || Context::rasterizer_finish;
            });
            if (Context::rasterizer_output_queue.empty()) {
                if (Context::rasterizer_finish) {
                    Context::fragment_finish = true;
                    return;
                }
                continue;
            }
            fragment = Context::rasterizer_output_queue.front();
            Context::rasterizer_output_queue.pop();
        }
        int index = (Uniforms::height - 1 - fragment.y) * Uniforms::width + fragment.x;
        if (fragment.depth > Context::frame_buffer.depth_buffer[index]) {
            continue;
        }
        fragment.color =
            fragment_shader_ptr(fragment, Uniforms::material, Uniforms::lights, Uniforms::camera);
        Context::frame_buffer.set_pixel(index, fragment.depth, fragment.color);
    }
}
