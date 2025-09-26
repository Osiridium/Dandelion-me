#include <array>
#include <limits>
#include <tuple>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <spdlog/spdlog.h>

#include "rasterizer.h"
#include "triangle.h"
#include "../utils/math.hpp"

using Eigen::Matrix4f;
using Eigen::Vector2i;
using Eigen::Vector3f;
using Eigen::Vector4f;
using std::fill;
using std::tuple;

void Rasterizer::worker_thread()
{
    while (true) {
        VertexShaderPayload payloads[3];
        {
            std::unique_lock<std::mutex> lock(Context::vertex_queue_mutex);

            if (Context::vertex_shader_output_queue.size() < 3) {
                if (Context::vertex_finish) {
                    Context::rasterizer_finish = true;
                    return;
                }
                continue;
            }

            for (int i = 0; i < 3; ++i) {
                payloads[i] = Context::vertex_shader_output_queue.front();
                Context::vertex_shader_output_queue.pop();
            }
        }

        Triangle triangle;
        for (int i = 0; i < 3; ++i) {
            triangle.world_pos[i]    = payloads[i].world_position;
            triangle.viewport_pos[i] = payloads[i].viewport_position;
            triangle.normal[i]       = payloads[i].normal;
        }

        rasterize_triangle(triangle);
    }
}

float sign(Eigen::Vector2f p1, Eigen::Vector2f p2, Eigen::Vector2f p3)
{
    return (p1.x() - p3.x()) * (p2.y() - p3.y()) - (p2.x() - p3.x()) * (p1.y() - p3.y());
}

// 给定坐标(x,y)以及三角形的三个顶点坐标，判断(x,y)是否在三角形的内部
bool Rasterizer::inside_triangle(int x, int y, const Vector4f* vertices)
{
    float px = static_cast<float>(x) + 0.5f;
    float py = static_cast<float>(y) + 0.5f;
    auto  [alpha, beta, gamma] = compute_barycentric_2d(px, py, vertices);
    constexpr float eps = 1e-4f;
    return alpha >= -eps && beta >= -eps && gamma >= -eps;
}

// 给定坐标(x,y)以及三角形的三个顶点坐标，计算(x,y)对应的重心坐标[alpha, beta, gamma]
tuple<float, float, float> Rasterizer::compute_barycentric_2d(float x, float y, const Vector4f* v)
{
    const double x0 = static_cast<double>(v[0].x());
    const double y0 = static_cast<double>(v[0].y());
    const double x1 = static_cast<double>(v[1].x());
    const double y1 = static_cast<double>(v[1].y());
    const double x2 = static_cast<double>(v[2].x());
    const double y2 = static_cast<double>(v[2].y());

    const double denom = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);

    if (std::abs(denom) < 1e-12) {
        return {0.0f, 0.0f, 0.0f};
    }

    const double c1 =
        (x * (y1 - y2) + (x2 - x1) * y + x1 * y2 - x2 * y1) / denom;
    const double c2 =
        (x * (y2 - y0) + (x0 - x2) * y + x2 * y0 - x0 * y2) / denom;
    const double c3 = 1.0 - c1 - c2;

    return {static_cast<float>(c1), static_cast<float>(c2), static_cast<float>(c3)};
}

// 对顶点的某一属性插值
Vector3f Rasterizer::interpolate(
    float alpha, float beta, float gamma, const Eigen::Vector3f& vert1,
    const Eigen::Vector3f& vert2, const Eigen::Vector3f& vert3, const Eigen::Vector3f& weight,
    const float& Z
)
{
    Vector3f interpolated_res;
    for (int i = 0; i < 3; i++) {
        interpolated_res[i] = alpha * vert1[i] / weight[0] + beta * vert2[i] / weight[1]
                            + gamma * vert3[i] / weight[2];
    }
    interpolated_res *= Z;
    return interpolated_res;
}

// 对当前三角形进行光栅化
void Rasterizer::rasterize_triangle(Triangle& t)
{
    const Vector4f* v = t.viewport_pos;

    const float min_x = std::min({v[0].x(), v[1].x(), v[2].x()});
    const float max_x = std::max({v[0].x(), v[1].x(), v[2].x()});
    const float min_y = std::min({v[0].y(), v[1].y(), v[2].y()});
    const float max_y = std::max({v[0].y(), v[1].y(), v[2].y()});

    int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    int x1 = std::min(Uniforms::width - 1, static_cast<int>(std::ceil(max_x)));
    int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    int y1 = std::min(Uniforms::height - 1, static_cast<int>(std::ceil(max_y)));

    if (x0 > x1 || y0 > y1) {
        return;
    }

    const float w0 = v[0].w();
    const float w1 = v[1].w();
    const float w2 = v[2].w();
    if (std::abs(w0) < 1e-8f || std::abs(w1) < 1e-8f || std::abs(w2) < 1e-8f) {
        return;
    }

    const Vector3f world_pos0 = t.world_pos[0].head<3>();
    const Vector3f world_pos1 = t.world_pos[1].head<3>();
    const Vector3f world_pos2 = t.world_pos[2].head<3>();
    const Vector3f normal0    = t.normal[0];
    const Vector3f normal1    = t.normal[1];
    const Vector3f normal2    = t.normal[2];

    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            if (!inside_triangle(x, y, v)) {
                continue;
            }

            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            auto  [alpha, beta, gamma] = compute_barycentric_2d(px, py, v);

            const float inv_w_sum = alpha / w0 + beta / w1 + gamma / w2;
            if (inv_w_sum <= 0.0f || std::abs(inv_w_sum) < 1e-8f) {
                continue;
            }
            const float w = 1.0f / inv_w_sum;

            const float depth =
                alpha * v[0].z() / w0 + beta * v[1].z() / w1 + gamma * v[2].z() / w2;

            Eigen::Vector3f interp_world =
                (alpha * world_pos0 / w0 + beta * world_pos1 / w1 + gamma * world_pos2 / w2)
                * w;
            Eigen::Vector3f interp_normal =
                (alpha * normal0 / w0 + beta * normal1 / w1 + gamma * normal2 / w2) * w;
            if (interp_normal.norm() > 0.0f) {
                interp_normal.normalize();
            }
            FragmentShaderPayload payload;
            payload.world_pos    = interp_world;
            payload.world_normal = interp_normal;
            payload.x     = x;
            payload.y     = y;
            payload.depth = depth;

            {
                std::unique_lock<std::mutex> lock(Context::rasterizer_queue_mutex);
                Context::rasterizer_output_queue.push(payload);
            }
        }
    }
}
