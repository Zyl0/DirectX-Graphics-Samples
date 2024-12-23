#ifndef RAYTRACING_COMPAT_H
#define RAYTRACING_COMPAT_H

struct Viewport
{
    float left;
    float top;
    float right;
    float bottom;
};

#ifdef HLSL
typedef float3 vec3;
typedef float4 vec4;
typedef float3x3 matrix3;
typedef float4x4 matrix4;

#else
#include "VectorMath.h"

typedef Math::Vector3   vec3;
typedef Math::Vector4   vec4;
typedef Math::Matrix3   matrix3;
typedef Math::Matrix4   matrix4;
#endif

struct SceneConstantBuffer
{
    // Viewport Settings
    Viewport viewport;
    Viewport stencil;

    // Camera
    matrix4 WorldToProjectedSpace;
    matrix4 ProjectedSpaceToWorld;
    vec3 CameraPosition;
    vec3 CameraDirection;
};

struct Vertex
{
    vec3 position;
    vec3 normal;
};

#endif // RAYTRACING_COMPAT_H