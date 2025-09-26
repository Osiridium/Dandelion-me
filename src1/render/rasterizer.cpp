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
    while (!Context::rasterizer_finish) {
        VertexShaderPayload payload;
        Triangle            triangle;
        {
            if (Context::vertex_finish && Context::vertex_shader_output_queue.empty()) {
                Context::rasterizer_finish = true;
                return;
            }
            if (Context::vertex_shader_output_queue.size() < 3) {
                continue;
            }
            std::unique_lock<std::mutex> lock(Context::vertex_queue_mutex);
            if (Context::vertex_shader_output_queue.size() < 3) {
                continue;
            }
            for (size_t vertex_count = 0; vertex_count < 3; vertex_count++) {
                payload = Context::vertex_shader_output_queue.front();
                Context::vertex_shader_output_queue.pop();
                if (vertex_count == 0) {
                    triangle.world_pos[0]    = payload.world_position;
                    triangle.viewport_pos[0] = payload.viewport_position;
                    triangle.normal[0]       = payload.normal;
                } else if (vertex_count == 1) {
                    triangle.world_pos[1]    = payload.world_position;
                    triangle.viewport_pos[1] = payload.viewport_position;
                    triangle.normal[1]       = payload.normal;
                } else {
                    triangle.world_pos[2]    = payload.world_position;
                    triangle.viewport_pos[2] = payload.viewport_position;
                    triangle.normal[2]       = payload.normal;
                }
            }
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
    Eigen::Vector2f p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    Eigen::Vector2f v0(vertices[0].x(), vertices[0].y());
    Eigen::Vector2f v1(vertices[1].x(), vertices[1].y());
    Eigen::Vector2f v2(vertices[2].x(), vertices[2].y());

    float d0 = sign(p, v0, v1);
    float d1 = sign(p, v1, v2);
    float d2 = sign(p, v2, v0);

    bool has_neg = (d0 < 0.0f) || (d1 < 0.0f) || (d2 < 0.0f);
    bool has_pos = (d0 > 0.0f) || (d1 > 0.0f) || (d2 > 0.0f);

    return !(has_neg && has_pos);
}

// 给定坐标(x,y)以及三角形的三个顶点坐标，计算(x,y)对应的重心坐标[alpha, beta, gamma]
tuple<float, float, float> Rasterizer::compute_barycentric_2d(float x, float y, const Vector4f* v)
{
    float denom =
        v[0].x() * (v[1].y() - v[2].y()) + v[1].x() * (v[2].y() - v[0].y())
        + v[2].x() * (v[0].y() - v[1].y());

    if (denom > -1e-6f && denom < 1e-6f) {
        return {0.0f, 0.0f, 0.0f};
    }

    float c1 =
        (x * (v[1].y() - v[2].y()) + (v[2].x() - v[1].x()) * y + v[1].x() * v[2].y()
         - v[2].x() * v[1].y())
        / denom;
    float c2 =
        (x * (v[2].y() - v[0].y()) + (v[0].x() - v[2].x()) * y + v[2].x() * v[0].y()
         - v[0].x() * v[2].y())
        / denom;
    float c3 = 1.0f - c1 - c2;

    return {c1, c2, c3};
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

    float min_x = std::min({v[0].x(), v[1].x(), v[2].x()});
    float max_x = std::max({v[0].x(), v[1].x(), v[2].x()});
    float min_y = std::min({v[0].y(), v[1].y(), v[2].y()});
    float max_y = std::max({v[0].y(), v[1].y(), v[2].y()});

    int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
    int x1 = std::min(Uniforms::width - 1, static_cast<int>(std::ceil(max_x)));
    int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
    int y1 = std::min(Uniforms::height - 1, static_cast<int>(std::ceil(max_y)));

    if (x0 > x1 || y0 > y1) {
        return;
    }

    Vector3f weight(v[0].w(), v[1].w(), v[2].w());
    Vector3f world_pos0 = t.world_pos[0].head<3>();
    Vector3f world_pos1 = t.world_pos[1].head<3>();
    Vector3f world_pos2 = t.world_pos[2].head<3>();
    Vector3f normal0    = t.normal[0];
    Vector3f normal1    = t.normal[1];
    Vector3f normal2    = t.normal[2];

    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            if (!inside_triangle(x, y, v)) {
                continue;
            }

            float px = static_cast<float>(x) + 0.5f;
            float py = static_cast<float>(y) + 0.5f;
            auto  [alpha, beta, gamma] = compute_barycentric_2d(px, py, v);

            float w_reciprocal = alpha / v[0].w() + beta / v[1].w() + gamma / v[2].w();
            if (w_reciprocal <= 0.0f) {
                continue;
            }
            float Z = 1.0f / w_reciprocal;

            float depth =
                (alpha * v[0].z() / v[0].w() + beta * v[1].z() / v[1].w()
                 + gamma * v[2].z() / v[2].w())
                * Z;

            FragmentShaderPayload payload;
            payload.world_pos = interpolate(alpha, beta, gamma, world_pos0, world_pos1, world_pos2, weight, Z);
            Vector3f interpolated_normal =
                interpolate(alpha, beta, gamma, normal0, normal1, normal2, weight, Z);
            if (interpolated_normal.norm() > 0.0f) {
                interpolated_normal.normalize();
            }
            payload.world_normal = interpolated_normal;
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
