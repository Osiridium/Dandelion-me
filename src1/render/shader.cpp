#include "rasterizer_renderer.h"
#include "../utils/math.hpp"
#include <cstdio>
#include <cmath>

#ifdef _WIN32
    #undef min
    #undef max
#endif

using Eigen::Vector3f;
using Eigen::Vector4f;

// vertex shader
VertexShaderPayload vertex_shader(const VertexShaderPayload& payload)
{
    VertexShaderPayload output_payload = payload;

    // Vertex position transformation
    const Eigen::Matrix4f model = Uniforms::inv_trans_M.inverse().transpose();
    const Eigen::Vector4f model_pos = payload.world_position;
    const Eigen::Vector4f world_pos = model * model_pos;
    const Eigen::Vector4f clip_pos  = Uniforms::MVP * model_pos;

    float inv_w = 0.0f;
    if (std::abs(clip_pos.w()) > 1e-6f) {
        inv_w = 1.0f / clip_pos.w();
    }

    Eigen::Vector3f ndc = clip_pos.head<3>() * inv_w;
    Eigen::Vector4f viewport_pos;
    viewport_pos.x() = 0.5f * (ndc.x() + 1.0f) * static_cast<float>(Uniforms::width);
    viewport_pos.y() = 0.5f * (ndc.y() + 1.0f) * static_cast<float>(Uniforms::height);
    viewport_pos.z() = ndc.z();
    viewport_pos.w() = clip_pos.w();

    output_payload.world_position   = world_pos;
    output_payload.viewport_position = viewport_pos;

    // Viewport transformation

    // Vertex normal transformation
    Eigen::Vector3f normal = Uniforms::inv_trans_M.block<3, 3>(0, 0) * payload.normal;
    const float normal_len = normal.norm();
    if (normal_len > 0.0f) {
        normal /= normal_len;
    } else {
        normal = Vector3f::UnitY();
    }
    output_payload.normal = normal;

    return output_payload;
}

Vector3f phong_fragment_shader(
    const FragmentShaderPayload& payload, const GL::Material& material,
    const std::list<Light>& lights, const Camera& camera
)
{
    Vector3f color = material.ambient;

    Vector3f normal = payload.world_normal;
    const float normal_len = normal.norm();
    if (normal_len > 0.0f) {
        normal /= normal_len;
    } else {
        normal = Vector3f::UnitY();
    }
    Vector3f       view_dir = camera.position - payload.world_pos;
    const float    view_len = view_dir.norm();
    if (view_len > 0.0f) {
        view_dir /= view_len;
    } else {
        view_dir = Vector3f(0.0f, 0.0f, 1.0f);
    }

    for (const auto& light: lights) {
        Vector3f light_dir = light.position - payload.world_pos;
        float     dist2     = light_dir.squaredNorm();
        if (dist2 <= 1e-6f) {
            dist2 = 1e-6f;
        }
        light_dir.normalize();

        const float attenuation = light.intensity / dist2;
        const Vector3f light_color = Vector3f::Constant(attenuation);

        const float diff = std::max(0.0f, normal.dot(light_dir));
        const Vector3f diffuse = material.diffuse.cwiseProduct(light_color) * diff;

        Vector3f half_vec = view_dir + light_dir;
        float    spec = 0.0f;
        if (half_vec.norm() > 0.0f && diff > 0.0f) {
            half_vec.normalize();
            spec = std::pow(std::max(0.0f, normal.dot(half_vec)), material.shininess);
        }
        const Vector3f specular = material.specular.cwiseProduct(light_color) * spec;

        color += diffuse + specular;
    }

    color = color.cwiseMax(Vector3f::Zero());
    color = color.cwiseMin(Vector3f::Ones());
    return color * 255.0f;
}
