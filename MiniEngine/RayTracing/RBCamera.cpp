#include "RBCamera.h"

void FlyCamera::Translate(float x, float y, float z)
{
    Position().SetX(Position().GetX() + x);
    Position().SetY(Position().GetY() + y);
    Position().SetZ(Position().GetZ() + z);

    UpdateView();
}

void FlyCamera::SetTranslation(float x, float y, float z)
{
    Position().SetX(x);
    Position().SetY(y);
    Position().SetZ(z);

    UpdateView();
}

void FlyCamera::RotateRadians(float Pitch, float Yaw)
{
    m_Pitch += Pitch;
    m_Yaw += Yaw;
    Rotation() = Rotation() * Math::Quaternion(Pitch, Yaw, 0);

    Direction().SetX(cos(m_Yaw) * cos(m_Pitch));
    Direction().SetY(sin(m_Pitch));
    Direction().SetZ(sin(m_Yaw) * cos(m_Pitch));

    m_Right = Normalize(Cross(Math::Vector3(0, 1, 0), Direction()));
    m_Up = Normalize(Cross(Direction(), m_Right));

    UpdateView();
}

void FlyCamera::SetRotationRadians(float Pitch, float Yaw)
{
    m_Pitch = Pitch;
    m_Yaw = Yaw;
    Rotation() = Math::Quaternion(Pitch, Yaw, 0);

    Direction().SetX(cos(m_Yaw) * cos(m_Pitch));
    Direction().SetY(sin(m_Pitch));
    Direction().SetZ(sin(m_Yaw) * cos(m_Pitch));

    m_Right = Normalize(Cross(Math::Vector3(0, 1, 0), Direction()));
    m_Up = Normalize(Cross(Direction(), m_Right));

    UpdateView();
}

void FlyCamera::SetProjection(unsigned Width, unsigned Height, float FieldOfView, float NearDistance, float FarDistance)
{
    m_AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
    m_FieldOfView = FieldOfView;
    m_NearDistance = NearDistance;
    m_FarDistance = FarDistance;

    UpdateProjection();
}

void FlyCamera::SetProjection(float AspectRatio, float FieldOfView, float NearDistance, float FarDistance)
{
    m_AspectRatio = AspectRatio;
    m_FieldOfView = FieldOfView;
    m_NearDistance = NearDistance;
    m_FarDistance = FarDistance;

    UpdateProjection();
}

Math::Matrix4 FlyCamera::ComputeView()
{
    m_Rotator = Math::Matrix3(
        m_Right,
        m_Up,
        Direction()
    );

    return Math::Matrix4(
            m_Right,
            m_Up,
            Direction(),
            Math::Vector3(0, 0, 0)
        ) *
        Math::Matrix4(
            Math::Vector3(1, 0, 0),
            Math::Vector3(0, 1, 0),
            Math::Vector3(0, 0, 1),
            -Position()
        );
}

Math::Matrix4 FlyCamera::ComputeProjection() const
{
    float ZNear = std::max(float(0.1), m_NearDistance);
    float ZFar = std::max(float(1), m_FarDistance);

    float g = 1.0f / tan(m_FieldOfView / 2.0f);
    float k = ZFar / (ZFar - ZNear);

    Math::Matrix4 proj;

    proj.SetX(Math::Vector4(g / m_AspectRatio,  0.0f,   0.0f,     0.0f));
    proj.SetY(Math::Vector4(0.0f,               g,      0.0f,     0.0f));
    proj.SetZ(Math::Vector4(0.0f,               0.0f,   k,        -ZNear * k));
    proj.SetW(Math::Vector4(0.0f,               0.0f,   1.0f,     0.0f));

    return proj;
}
