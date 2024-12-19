#ifndef RAYTRACING_COMPAT_H
#define RAYTRACING_COMPAT_H

struct Viewport
{
    float left;
    float top;
    float right;
    float bottom;
};

struct RayGenConstantBuffer
{
    Viewport viewport;
    Viewport stencil;
};

#endif // RAYTRACING_COMPAT_H