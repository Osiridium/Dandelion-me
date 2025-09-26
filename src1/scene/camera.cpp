#include "camera.h"

#include <cmath>
#include <Eigen/Geometry>
#include "../utils/formatter.hpp"
#include <spdlog/spdlog.h>

#include "../utils/math.hpp"

using Eigen::Affine3f;
using Eigen::Matrix3f;
using Eigen::Matrix4f;
using Eigen::Vector3f;
using Eigen::Vector4f;

Camera::Camera(
    const Eigen::Vector3f& position, const Eigen::Vector3f& target, float near_plane,
    float far_plane, float fov_y_degrees, float aspect_ratio
) :
    position(position), target(target), near_plane(near_plane), far_plane(far_plane),
    fov_y_degrees(fov_y_degrees), aspect_ratio(aspect_ratio)
{
    world_up.x() = 0.0f;
    world_up.y() = 1.0f;
    world_up.z() = 0.0f;
}

Matrix4f Camera::view()
{
    // Compute the inverted view direction, up and right vectors related to
    // the camera itself.
    Vector3f inv_direction = (position - target).normalized();
    Vector3f right         = (world_up).cross(inv_direction).normalized();
    Vector3f up            = inv_direction.cross(right);
    // The view matrix is multiplication of a rotation and a translation
    // matrices: rotation represented by [right, up, inv_direction]^T and
    // translation represented by -position.
    Matrix4f view_matrix          = Matrix4f::Identity();
    view_matrix.block<1, 3>(0, 0) = right;
    view_matrix.block<1, 3>(1, 0) = up;
    view_matrix.block<1, 3>(2, 0) = inv_direction;
    view_matrix(0, 3)             = -right.dot(position);
    view_matrix(1, 3)             = -up.dot(position);
    view_matrix(2, 3)             = -inv_direction.dot(position);
    return view_matrix;
}

Matrix4f Camera::projection()
{
    const float fov_y = radians(fov_y_degrees);
    
    // 透视投影矩阵的标准参数
    const float n = near_plane;  // 近平面距离
    const float f = far_plane;   // 远平面距离
    const float t = n * std::tan(fov_y / 2.0f);  // 近平面上半高度
    const float r = t * aspect_ratio;            // 近平面右半宽度
    
    // 构造透视投影矩阵
    Matrix4f projection = Matrix4f::Zero();
    
    // 透视投影矩阵的标准形式
    projection(0, 0) = n / r;                    // 2*n / (r-l), 由于对称所以l=-r
    projection(1, 1) = n / t;                    // 2*n / (t-b), 由于对称所以b=-t
    projection(2, 2) = -(f + n) / (f - n);      // -(f+n) / (f-n)
    projection(2, 3) = -2.0f * f * n / (f - n); // -2*f*n / (f-n)
    projection(3, 2) = -1.0f;                   // -1，用于齐次坐标的透视除法
    projection(3, 3) = 0.0f;                    // 0

    return projection;
}
