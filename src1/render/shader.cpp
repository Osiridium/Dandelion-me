#include "rasterizer_renderer.h"
#include "../utils/math.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

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
    Eigen::Matrix4f model = Uniforms::inv_trans_M.inverse().transpose();
    Eigen::Vector4f model_position = payload.world_position;
    Eigen::Vector4f world_position  = model * model_position;
    Eigen::Vector4f clip_position   = Uniforms::MVP * model_position;

    Eigen::Vector3f ndc = clip_position.head<3>() / clip_position.w();
    float           x   = 0.5f * (ndc.x() + 1.0f) * static_cast<float>(Uniforms::width);
    float           y   = 0.5f * (1.0f - ndc.y()) * static_cast<float>(Uniforms::height);

    output_payload.world_position    = world_position;
    output_payload.viewport_position = Eigen::Vector4f(x, y, clip_position.z(), clip_position.w());

    // Viewport transformation

    // Vertex normal transformation
    Eigen::Vector4f normal4(payload.normal.x(), payload.normal.y(), payload.normal.z(), 0.0f);
    Eigen::Vector3f world_normal = (Uniforms::inv_trans_M * normal4).head<3>();
    world_normal.normalize();
    output_payload.normal = world_normal;

    return output_payload;
}

Vector3f phong_fragment_shader(
    const FragmentShaderPayload& payload, const GL::Material& material,
    const std::list<Light>& lights, const Camera& camera
)
{
    Vector3f result = Vector3f::Zero();

    // Ambient term (constant global ambient light)
    const Vector3f ambient_light = Vector3f::Constant(0.1f);
    result += material.ambient.cwiseProduct(ambient_light);

    Vector3f normal = payload.world_normal.normalized();
    Vector3f view_vec = camera.position - payload.world_pos;
    Vector3f view_dir = view_vec.squaredNorm() > 1e-6f ? view_vec.normalized() : Vector3f(0.0f, 0.0f, 1.0f);

    for (const auto& light: lights) {
        Vector3f light_vec   = light.position - payload.world_pos;
        float    distance_sq = std::max(light_vec.squaredNorm(), 1e-4f);
        Vector3f light_dir   = light_vec.normalized();
        float    attenuation = light.intensity / distance_sq;
        Vector3f radiance    = Vector3f::Ones() * attenuation;

        float    ndotl  = std::max(0.0f, normal.dot(light_dir));
        Vector3f diffuse = material.diffuse.cwiseProduct(radiance) * ndotl;

        Vector3f half_vec = (light_dir + view_dir).normalized();
        float    ndoth    = std::max(0.0f, normal.dot(half_vec));
        Vector3f specular = material.specular.cwiseProduct(radiance)
                          * std::pow(ndoth, material.shininess);

        result += diffuse + specular;
    }

    result = result.cwiseMax(Vector3f::Zero()).cwiseMin(Vector3f::Constant(1.0f));
    return result * 255.f;
}
