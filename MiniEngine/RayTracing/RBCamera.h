#pragma once

#include <pch.h>

#ifndef M_PI
#define M_PI       3.14159265358979323846   // pi
#endif

INLINE float Radians(float degrees)
{
    return (static_cast<float>(M_PI) / 180.f) * degrees;
}

INLINE float Degrees(float radians)
{
    return (180.f / static_cast<float>(M_PI)) * radians;
}

class RBCamera
{
public:
    INLINE const Math::Matrix4& View() const { return m_view; }
    INLINE const Math::Matrix4& Projection() const { return m_projection; }

    INLINE const Math::Matrix4& InverseView() const { return m_inverse_view; }
    INLINE const Math::Matrix4& InverseProjection() const { return m_inverse_projection; }

    INLINE Math::Vector3 GetWorldPosition() const { return m_Position; };
    INLINE Math::Vector3 GetWorldDirection() const { return m_Direction; };
    INLINE Math::Quaternion GetWorldRotation() const { return m_Rotation; };

protected:
    inline void SetViewTransform(const Math::Matrix4& inView)
    {
        m_view = inView;
        m_inverse_view = Math::Matrix4(XMMatrixInverse(nullptr, inView));
    }

    inline void SetProjectionTransform(const Math::Matrix4& inProjection)
    {
        m_projection = inProjection;
        m_inverse_projection = Math::Matrix4(XMMatrixInverse(nullptr, inProjection));
    }

    INLINE Math::Vector3& Position()            { return m_Position; }
    INLINE Math::Vector3& Direction()           { return m_Direction; }
    INLINE Math::Quaternion& Rotation()         { return m_Rotation; }

    INLINE const Math::Vector3& Position()      const { return m_Position; }
    INLINE const Math::Vector3& Direction()     const { return m_Direction; }
    INLINE const Math::Quaternion& Rotation()   const { return m_Rotation; }

private:
    Math::Matrix4 m_view;
    Math::Matrix4 m_projection;

    Math::Matrix4 m_inverse_view;
    Math::Matrix4 m_inverse_projection;

    Math::Vector3 m_Position = Math::Vector3(0, 0, 0);
    Math::Vector3 m_Direction = Math::Vector3(1, 0, 0);
    Math::Quaternion m_Rotation = Math::Quaternion(0, 0, 0);
};

class FlyCamera : public RBCamera
{
public:
    FlyCamera() : m_Up(0,1,0), m_Right()
    {
        Position().SetX(0);
        Position().SetY(0);
        Position().SetZ(0);
        SetRotationRadians(0, 0);
        Direction().SetX(1);
        Direction().SetX(0);
        Direction().SetX(0);
    }

    void Translate(float x, float y, float z);

    INLINE void Translate(Math::Vector3 translation) { Translate(translation.GetX(), translation.GetY(), translation.GetZ()); }

    void SetTranslation(float x, float y, float z);

    INLINE void SetTranslation(Math::Vector3 translation) { SetTranslation(translation.GetX(), translation.GetY(), translation.GetZ()); }

    void RotateRadians(float Pitch, float Yaw);

    INLINE void RotateDegrees(float Pitch, float Yaw) { RotateRadians(Radians(Pitch), Radians(Yaw)); }

    void SetRotationRadians(float Pitch, float Yaw);

    INLINE void SetRotationDegrees(float Pitch, float Yaw) { SetRotationRadians(Radians(Pitch), Radians(Yaw)); }

    void SetProjection(unsigned Width, unsigned Height, float FieldOfView, float NearDistance = 0.15, float FarDistance = 2.15);

    void SetProjection(float AspectRatio, float FieldOfView, float NearDistance = 0.15, float FarDistance = 2.15);

    INLINE const Math::Matrix3& GetRotator() const { return m_Rotator; }

private:
    Math::Matrix4 ComputeView();
    inline void UpdateView() { SetViewTransform(ComputeView()); }

    Math::Matrix4 ComputeProjection() const;
    inline void UpdateProjection() { SetProjectionTransform(ComputeProjection()); }

    Math::Vector3 m_Up;
    Math::Vector3 m_Right;

    Math::Matrix3 m_Rotator;

    float m_NearDistance = 0;
    float m_FarDistance = 0;

    float m_FieldOfView = 0;
    float m_AspectRatio = 0;

    float m_Pitch = 0;
    float m_Yaw = 0;
};