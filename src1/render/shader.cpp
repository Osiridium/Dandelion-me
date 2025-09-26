#include "rasterizer_renderer.h"
#include "../utils/math.hpp"
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
    Eigen::Vector4f model_pos = payload.world_position;
    Eigen::Vector4f clip_pos  = Uniforms::MVP * model_pos;

    float w = clip_pos.w();
    if (w > -1e-6f && w < 1e-6f) {
        w = (w >= 0.0f ? 1e-6f : -1e-6f);
    }
    float inv_w = 1.0f / w;
    Eigen::Vector3f ndc = clip_pos.head<3>() * inv_w;

    Eigen::Vector4f viewport_pos = Eigen::Vector4f::Zero();
    viewport_pos.x() = (ndc.x() + 1.0f) * 0.5f * static_cast<float>(Uniforms::width);
    viewport_pos.y() = (ndc.y() + 1.0f) * 0.5f * static_cast<float>(Uniforms::height);
    viewport_pos.z() = clip_pos.z();
    viewport_pos.w() = w;

    // Viewport transformation
    output_payload.viewport_position = viewport_pos;

    // Vertex normal transformation
    Eigen::Vector4f normal4(payload.normal.x(), payload.normal.y(), payload.normal.z(), 0.0f);
    Eigen::Vector3f world_normal = (Uniforms::inv_trans_M * normal4).head<3>().normalized();
    output_payload.normal        = world_normal;

    Eigen::Matrix4f model_matrix = Uniforms::inv_trans_M.inverse().transpose();
    output_payload.world_position = model_matrix * model_pos;

    return output_payload;
}

Vector3f phong_fragment_shader(
    const FragmentShaderPayload& payload, const GL::Material& material,
    const std::list<Light>& lights, const Camera& camera
)
{
    Vector3f result = {0, 0, 0};

    // ka,kd,ks can be got from material.ambient,material.diffuse,material.specular

    // set ambient light intensity
    const Vector3f ambient_light = Vector3f::Constant(0.1f);
    result += material.ambient.cwiseProduct(ambient_light);

    // Light Direction
    Vector3f point = payload.world_pos;
    Vector3f normal = payload.world_normal.normalized();

    // View Direction
    Vector3f view_dir = (camera.position - point).normalized();

    // Half Vector

    // Light Attenuation

    // Ambient

    // Diffuse

    // Specular
    for (const auto& light: lights) {
        Vector3f light_dir = (light.position - point);
        float    distance2 = std::max(light_dir.squaredNorm(), 1e-6f);
        light_dir.normalize();

        Vector3f attenuation = Vector3f::Constant(light.intensity / distance2);

        Vector3f half_vec = (light_dir + view_dir).normalized();

        float ndotl = std::max(0.0f, normal.dot(light_dir));
        Vector3f diffuse = material.diffuse.cwiseProduct(attenuation) * ndotl;

        float ndoth = std::max(0.0f, normal.dot(half_vec));
        Vector3f specular = material.specular.cwiseProduct(attenuation)
                             * std::pow(ndoth, material.shininess);

        result += diffuse + specular;
    }

    result = result.cwiseMax(Vector3f::Zero());
    result = result.cwiseMin(Vector3f::Ones());

    // set rendering result max threshold to 255
    return result * 255.f;
}
