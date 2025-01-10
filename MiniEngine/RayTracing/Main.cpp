
#include "pch.h"
#include "GameCore.h"
#include "GraphicsCore.h"
#include "SystemTime.h"
#include "TextRenderer.h"
#include "GameInput.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "BufferManager.h"

#include "Camera.h"
#include "CameraController.h"
#include "RBCamera.h"

#include "TemporalEffects.h"
#include "MotionBlur.h"
#include "DepthOfField.h"
#include "PostEffects.h"
#include "SSAO.h"
#include "FXAA.h"

#include "D3DCompiler.h"
#include "DXSampleHelper.h"
#include "DXRaytracingHelper.h"

// Shader Compat
#include "Shaders/RayTracing_Compat.h"

// Compiled Shaders
#include "CompiledShaders\RayTracing.h"

#define SHADER_BYTECODE_FROM_HEADER(HeaderName) CD3DX12_SHADER_BYTECODE((const void*)g_p##HeaderName, (SIZE_T)ARRAYSIZE(g_p##HeaderName))

using namespace GameCore;
using namespace Graphics;

namespace ShaderNames
{
    static constexpr wchar_t c_hitGroupName[] = L"MyHitGroup";
    static constexpr wchar_t c_raygenShaderName[] = L"MyRaygenShader";
    static constexpr wchar_t c_closestHitShaderName[] = L"MyClosestHitShader";
    static constexpr wchar_t c_missShaderName[] = L"MyMissShader";
}

namespace Config
{
    static constexpr DXGI_FORMAT c_DepthBufferFormat = DXGI_FORMAT_D32_FLOAT;
    static constexpr DXGI_FORMAT c_BackBufferFormat = DXGI_FORMAT_R11G11B10_FLOAT;

    // PERFOMANCE TIP: Set max recursion depth as low as needed 
    // as drivers may apply optimization strategies for low recursion depths
    static constexpr UINT c_MaxRecursionDepth = 1; // ~ primary rays only. 
}

// Raytracing device and command list.
struct DXRInterface
{
    struct DXRCommandList
    {
        DXRCommandList(DXRInterface& DXR, GraphicsContext& Context) 
        {
            ThrowIfFailed(Context.GetCommandList()->QueryInterface(IID_PPV_ARGS(&m_dxrCommandList)), L"Couldn't get DirectX Raytracing interface for the command list.\n");
        }
        ~DXRCommandList() { m_dxrCommandList.Reset(); }

        DXRCommandList(const DXRCommandList&) = delete;
        DXRCommandList& operator = (const DXRCommandList&) = delete;

        DXRCommandList(DXRCommandList&& other) : 
            m_dxrCommandList( std::move(other.m_dxrCommandList) )
        {}

        DXRCommandList& operator = (DXRCommandList&& other)
        {
            if (this == &other) return *this;

            m_dxrCommandList = std::move(other.m_dxrCommandList);

            return *this;
        }

        ID3D12GraphicsCommandList4* Get() { return m_dxrCommandList.Get(); }
    private:
        ComPtr<ID3D12GraphicsCommandList4> m_dxrCommandList;
    };

    // Create raytracing device and command list.
    DXRInterface()
    {
        ThrowIfFailed(g_Device->QueryInterface(IID_PPV_ARGS(&m_dxrDevice)), L"Couldn't get DirectX Raytracing interface for the device.\n");
    }

    DXRCommandList GetCommandList(GraphicsContext& Context)
    {
        return DXRCommandList(*this, Context);
    }

    ~DXRInterface()
    {
        m_dxrDevice.Reset();
    }

    ComPtr<ID3D12Device5> m_dxrDevice;
};

struct RTRootSignatures
{
    struct GlobalRootSignatureParams {
        enum Value {
            OutputViewSlot = 0,
            AccelerationStructureSlot,
            SceneConstantSlot,
            VertexBufferSlot,
            Count
        };
    };

    struct LocalRootSignatureParams {
        enum Value {
            ViewportConstantSlot = 0,
            Count
        };
    };

    RTRootSignatures(SceneConstantBuffer& RayGenCB)
    {
        // Global Root Signature
        {
            CD3DX12_DESCRIPTOR_RANGE ranges[2]; // Perfomance TIP: Order from most frequent to least frequent.
            ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);  // 1 output texture
            ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 1);  // 2 static index and vertex buffers.

            CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParams::Count];
            rootParameters[GlobalRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &ranges[0]);
            rootParameters[GlobalRootSignatureParams::AccelerationStructureSlot].InitAsShaderResourceView(0);
            rootParameters[GlobalRootSignatureParams::SceneConstantSlot].InitAsConstantBufferView(0);
            rootParameters[GlobalRootSignatureParams::VertexBufferSlot].InitAsDescriptorTable(1, &ranges[1]);;

            CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_raytracingGlobalRootSignature);
        }

        // Local Root Signature
        {
            CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
            rootParameters[LocalRootSignatureParams::ViewportConstantSlot].InitAsConstants(SizeOfInUint32(RayGenCB), 1);

            CD3DX12_ROOT_SIGNATURE_DESC localRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            localRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
            SerializeAndCreateRaytracingRootSignature(localRootSignatureDesc, &m_raytracingLocalRootSignature);
        }
    }

    ~RTRootSignatures()
    {
        m_raytracingGlobalRootSignature.Reset();
        m_raytracingLocalRootSignature.Reset();
    }

    // Global Root Signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    ComPtr<ID3D12RootSignature> m_raytracingGlobalRootSignature;

    // Local Root Signature
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    ComPtr<ID3D12RootSignature> m_raytracingLocalRootSignature;

private:
    static void SerializeAndCreateRaytracingRootSignature(D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig)
    {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;

        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
        ThrowIfFailed(g_Device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
    }
};

// Create 7 subobjects that combine into a RTPSO:
// Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
// Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
// This simple sample utilizes default shader association except for local root signature subobject
// which has an explicit association specified purely for demonstration purposes.
// 1 - DXIL library
// 1 - Triangle hit group
// 1 - Shader config
// 2 - Local root signature and association
// 1 - Global root signature
// 1 - Pipeline config
struct RTPipelineState
{
    RTPipelineState(DXRInterface& DXR, RTRootSignatures& Signatures) :
        raytracingPipeline(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
    {
        // DXIL library
        RTShaderLib = raytracingPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
        D3D12_SHADER_BYTECODE libdxil = SHADER_BYTECODE_FROM_HEADER(RayTracing);
        RTShaderLib->SetDXILLibrary(&libdxil);

        // Define which shader exports to surface from the library.
        // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
        // In this sample, this could be omitted for convenience since the sample uses all shaders in the library. 
        {
            RTShaderLib->DefineExport(ShaderNames::c_raygenShaderName);
            RTShaderLib->DefineExport(ShaderNames::c_closestHitShaderName);
            RTShaderLib->DefineExport(ShaderNames::c_missShaderName);
        }

        // Triangle hit group
        RTHitGroup = raytracingPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
        RTHitGroup->SetClosestHitShaderImport(ShaderNames::c_closestHitShaderName);
        RTHitGroup->SetHitGroupExport(ShaderNames::c_hitGroupName);
        RTHitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

        // Shader config
        RTShaderConfig = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
        UINT payloadSize = 4 * sizeof(float);   // float4 color
        UINT attributeSize = 2 * sizeof(float); // float2 barycentrics
        RTShaderConfig->Config(payloadSize, attributeSize);

        // Local root signature to be used in a ray gen shader.
        {
            RTLocalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
            RTLocalRootSignature->SetRootSignature(Signatures.m_raytracingLocalRootSignature.Get());

            RTLocalRootSignatureAssociation = raytracingPipeline.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
            RTLocalRootSignatureAssociation->SetSubobjectToAssociate(*RTLocalRootSignature);
            RTLocalRootSignatureAssociation->AddExport(ShaderNames::c_raygenShaderName);
        }

        // Global root signature
        RTGlobalRootSignature = raytracingPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
        RTGlobalRootSignature->SetRootSignature(Signatures.m_raytracingGlobalRootSignature.Get());
        
        // Pipeline config
        RTPipelineConfig  = raytracingPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
        RTPipelineConfig->Config(Config::c_MaxRecursionDepth);

#if _DEBUG
        PrintStateObjectDesc(raytracingPipeline);
#endif

        ThrowIfFailed(DXR.m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }

    ~RTPipelineState()
    {
        m_dxrStateObject.Reset();

        //todo check for destructor call for all pointers
    }

    ComPtr<ID3D12StateObject> m_dxrStateObject;

    CD3DX12_STATE_OBJECT_DESC raytracingPipeline;

    // DXIL library
    // This contains the shaders and their entrypoints for the state object.
    // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
    CD3DX12_DXIL_LIBRARY_SUBOBJECT* RTShaderLib = nullptr;

    // Triangle hit group
    // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
    // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
    CD3DX12_HIT_GROUP_SUBOBJECT* RTHitGroup = nullptr;

    // Shader config
    // Defines the maximum sizes in bytes for the ray payload and attribute structure.
    CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* RTShaderConfig = nullptr;

    // Local root signature to be used in a ray gen shader.
    // Hit group and miss shaders in this sample are not using a local root signature and thus one is not associated with them.
    CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT* RTLocalRootSignature = nullptr;

    CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT* RTLocalRootSignatureAssociation = nullptr;

    // Global root signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* RTGlobalRootSignature = nullptr;

    // Pipeline config
    // Defines the maximum TraceRay() recursion depth.
    CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* RTPipelineConfig = nullptr;
};


// Allocate a heap for a single descriptor:
// 1 - raytracing output texture UAV
struct RTBuffersDescriptorHeap
{
    // todo replace with RTBufferDescriptorHeap from mini engine

    RTBuffersDescriptorHeap(UINT DescriptorCount = 1) : m_descriptorCount(DescriptorCount)
    {
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
        descriptorHeapDesc.NumDescriptors = DescriptorCount;
        descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descriptorHeapDesc.NodeMask = 0;
        g_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        NAME_D3D12_OBJECT(m_descriptorHeap);

        m_descriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    ~RTBuffersDescriptorHeap()
    {
        m_descriptorHeap.Reset();
    }

    UINT AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse = UINT_MAX)
    {
        auto descriptorHeapCpuBase = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (descriptorIndexToUse >= m_descriptorHeap->GetDesc().NumDescriptors)
        {
            descriptorIndexToUse = m_descriptorsAllocated++;
        }
        *cpuDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorHeapCpuBase, descriptorIndexToUse, m_descriptorSize);
        return descriptorIndexToUse;
    }

    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    UINT m_descriptorsAllocated = 0;
    UINT m_descriptorSize;
    UINT m_descriptorCount;
};


struct SceneGeometry
{
    // todo use real mesh and paramters

    typedef UINT16 Index;
    using Vertex = Vertex;

    enum Preset
    {
        HelloWorld = 0,
        Cube
    };

    SceneGeometry(RTBuffersDescriptorHeap& HeapDesciptor, Preset preset = Preset::Cube)
    {
        Index* Indices = nullptr;
        size_t IndicesSize = 0;
        SceneGeometry::Vertex* Vertices = nullptr ;
        size_t VerticesSize = 0;
        size_t FaceVertexCount = 0;

        switch (preset)
        {
        case SceneGeometry::HelloWorld:
            GetHelloWorldData(Indices, IndicesSize, Vertices, VerticesSize, FaceVertexCount);
            break;
        case SceneGeometry::Cube:
            GetCubeData(Indices, IndicesSize, Vertices, VerticesSize, FaceVertexCount);
            break;
        default:
            break;
        }

        m_FaceVertexCount = FaceVertexCount;
        
        AllocateUploadBuffer(g_Device, Vertices, VerticesSize, &m_vertexBuffer.resource);
        AllocateUploadBuffer(g_Device, Indices, IndicesSize, &m_indexBuffer.resource);

        UINT descriptorIndexIB = CreateBufferSRV(HeapDesciptor, &m_indexBuffer, (IndicesSize / sizeof(SceneGeometry::Index)) / 4 /*/ 4*/, 0); //todo investigate on why the / 4 in the sample code requiered by the cube sample
        UINT descriptorIndexVB = CreateBufferSRV(HeapDesciptor, &m_vertexBuffer, VerticesSize / sizeof(SceneGeometry::Vertex), sizeof(SceneGeometry::Vertex));
        ThrowIfFalse(descriptorIndexVB == descriptorIndexIB + 1, L"Vertex Buffer descriptor index must follow that of Index Buffer descriptor index!");
    }

    struct D3DBuffer
    {
        ~D3DBuffer()
        {
            if (resource.Get() == nullptr) return;

            resource.Reset();
        }

        ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
    };
    D3DBuffer m_indexBuffer;
    D3DBuffer m_vertexBuffer;

    size_t m_FaceVertexCount = 0;

private:
    void GetHelloWorldData(Index*& Indices, size_t& IndicesSize, SceneGeometry::Vertex*& Vertices, size_t& VerticesSize, size_t& FaceVertexCount)
    {
        static constexpr float depthValue = 0.5;
        static constexpr float offset = 0.002f;
        static Index indices[] =
        {
            0, 1, 2
        };
        static SceneGeometry::Vertex vertices[] =
        {
            { Vector3(0, -offset, depthValue),          Vector3(0,0,1) },
            { Vector3(-offset, offset, depthValue),     Vector3(0,0,1) },
            { Vector3(offset, offset, depthValue),      Vector3(0,0,1) }
        };

        Indices = indices;
        IndicesSize = sizeof(indices);
        Vertices = vertices;
        VerticesSize = sizeof(vertices);
        FaceVertexCount = 3;
    }

    void GetCubeData(Index*& Indices, size_t& IndicesSize, SceneGeometry::Vertex*& Vertices, size_t& VerticesSize, size_t& FaceVertexCount)
    {
        static Index indices[] =
        {
            3,1,0,
            2,1,3,

            6,4,5,
            7,4,6,

            11,9,8,
            10,9,11,

            14,12,13,
            15,12,14,

            19,17,16,
            18,17,19,

            22,20,21,
            23,20,22
        };
        static SceneGeometry::Vertex vertices[] =
        {
            { XMFLOAT3(-1.0f, 1.0f, -1.0f),     XMFLOAT3(0.0f, 1.0f, 0.0f) },
            { XMFLOAT3(1.0f, 1.0f, -1.0f),      XMFLOAT3(0.0f, 1.0f, 0.0f) },
            { XMFLOAT3(1.0f, 1.0f, 1.0f),       XMFLOAT3(0.0f, 1.0f, 0.0f) },
            { XMFLOAT3(-1.0f, 1.0f, 1.0f),      XMFLOAT3(0.0f, 1.0f, 0.0f) },

            { XMFLOAT3(-1.0f, -1.0f, -1.0f),    XMFLOAT3(0.0f, -1.0f, 0.0f) },
            { XMFLOAT3(1.0f, -1.0f, -1.0f),     XMFLOAT3(0.0f, -1.0f, 0.0f) },
            { XMFLOAT3(1.0f, -1.0f, 1.0f),      XMFLOAT3(0.0f, -1.0f, 0.0f) },
            { XMFLOAT3(-1.0f, -1.0f, 1.0f),     XMFLOAT3(0.0f, -1.0f, 0.0f) },

            { XMFLOAT3(-1.0f, -1.0f, 1.0f),     XMFLOAT3(-1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(-1.0f, -1.0f, -1.0f),    XMFLOAT3(-1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(-1.0f, 1.0f, -1.0f),     XMFLOAT3(-1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(-1.0f, 1.0f, 1.0f),      XMFLOAT3(-1.0f, 0.0f, 0.0f) },

            { XMFLOAT3(1.0f, -1.0f, 1.0f),      XMFLOAT3(1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(1.0f, -1.0f, -1.0f),     XMFLOAT3(1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(1.0f, 1.0f, -1.0f),      XMFLOAT3(1.0f, 0.0f, 0.0f) },
            { XMFLOAT3(1.0f, 1.0f, 1.0f),       XMFLOAT3(1.0f, 0.0f, 0.0f) },

            { XMFLOAT3(-1.0f, -1.0f, -1.0f),    XMFLOAT3(0.0f, 0.0f, -1.0f) },
            { XMFLOAT3(1.0f, -1.0f, -1.0f),     XMFLOAT3(0.0f, 0.0f, -1.0f) },
            { XMFLOAT3(1.0f, 1.0f, -1.0f),      XMFLOAT3(0.0f, 0.0f, -1.0f) },
            { XMFLOAT3(-1.0f, 1.0f, -1.0f),     XMFLOAT3(0.0f, 0.0f, -1.0f) },

            { XMFLOAT3(-1.0f, -1.0f, 1.0f),     XMFLOAT3(0.0f, 0.0f, 1.0f) },
            { XMFLOAT3(1.0f, -1.0f, 1.0f),      XMFLOAT3(0.0f, 0.0f, 1.0f) },
            { XMFLOAT3(1.0f, 1.0f, 1.0f),       XMFLOAT3(0.0f, 0.0f, 1.0f) },
            { XMFLOAT3(-1.0f, 1.0f, 1.0f),      XMFLOAT3(0.0f, 0.0f, 1.0f) },
        };

        Indices = indices;
        IndicesSize = sizeof(indices);
        Vertices = vertices;
        VerticesSize = sizeof(vertices);
        FaceVertexCount = 3;
    }

    UINT CreateBufferSRV(RTBuffersDescriptorHeap& HeapDesciptor, D3DBuffer* buffer, UINT numElements, UINT elementSize)
    {
        // SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = numElements;
        if (elementSize == 0)
        {
            srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            srvDesc.Buffer.StructureByteStride = 0;
        }
        else
        {
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            srvDesc.Buffer.StructureByteStride = elementSize;
        }
        UINT descriptorIndex = HeapDesciptor.AllocateDescriptor(&(buffer->cpuDescriptorHandle));
        g_Device->CreateShaderResourceView(buffer->resource.Get(), &srvDesc, buffer->cpuDescriptorHandle);
        buffer->gpuDescriptorHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(HeapDesciptor.m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorIndex, HeapDesciptor.m_descriptorSize);
        return descriptorIndex;
    }
};

//Acceleration structures needed for raytracing.
struct RTAccelerationSturctures
{
    RTAccelerationSturctures(SceneGeometry& Geometry, DXRInterface& DxrInterface)
    {
        GraphicsContext& gfxContext = GraphicsContext::Begin(L"Acceleration structure creation");

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDesc.Triangles.IndexBuffer = Geometry.m_indexBuffer.resource->GetGPUVirtualAddress();
        geometryDesc.Triangles.IndexCount = static_cast<UINT>(Geometry.m_indexBuffer.resource->GetDesc().Width) / sizeof(SceneGeometry::Index);
        geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geometryDesc.Triangles.Transform3x4 = 0;
        geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDesc.Triangles.VertexCount = static_cast<UINT>(Geometry.m_vertexBuffer.resource->GetDesc().Width) / sizeof(SceneGeometry::Vertex);
        geometryDesc.Triangles.VertexBuffer.StartAddress = Geometry.m_vertexBuffer.resource->GetGPUVirtualAddress();
        geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(SceneGeometry::Vertex);

        // todo move to SceneGeometry param
        // Mark the geometry as opaque. 
        // PERFORMANCE TIP: mark geometry as opaque whenever applicable as it can enable important ray processing optimizations.
        // Note: When rays encounter opaque geometry an any hit shader will not be executed whether it is present or not.
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

        // Get required sizes for an acceleration structure.
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        // Top level BVH settup
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& topLevelInputs = topLevelBuildDesc.Inputs;
        topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        topLevelInputs.Flags = buildFlags;
        topLevelInputs.NumDescs = 1;
        topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topLevelInputs.pGeometryDescs = nullptr;    //todo find how to fill the top level with instances

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
        DxrInterface.m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
        ThrowIfFalse(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        // Bottom level BVH settup
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& bottomLevelInputs = bottomLevelBuildDesc.Inputs;
        bottomLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottomLevelInputs.Flags = buildFlags;
        bottomLevelInputs.NumDescs = 1;
        bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomLevelInputs.pGeometryDescs = &geometryDesc;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};
        DxrInterface.m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &bottomLevelPrebuildInfo);
        ThrowIfFalse(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        ComPtr<ID3D12Resource> scratchResource;
        //AllocateUAVBuffer(g_Device, 128, scratchResource.GetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");
        AllocateUAVBuffer(g_Device, max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes), scratchResource.GetAddressOf(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");

        // Allocate resources for acceleration structures.
        // Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
        // Default heap is OK since the application doesn’t need CPU read/write access to them. 
        // The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
        // and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
        //  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
        //  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.
        {
            D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

            AllocateUAVBuffer(g_Device, bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes, m_bottomLevelAccelerationStructure.GetAddressOf(), initialResourceState, L"BottomLevelAccelerationStructure");
            AllocateUAVBuffer(g_Device, topLevelPrebuildInfo.ResultDataMaxSizeInBytes, m_topLevelAccelerationStructure.GetAddressOf(), initialResourceState, L"TopLevelAccelerationStructure");
        }

        // Create an instance desc for the bottom-level acceleration structure.
        ComPtr<ID3D12Resource> instanceDescs;
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = instanceDesc.Transform[1][1] = instanceDesc.Transform[2][2] = 1;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructure->GetGPUVirtualAddress();
        AllocateUploadBuffer(g_Device, &instanceDesc, sizeof(instanceDesc), &instanceDescs, L"InstanceDescs");

        // Bottom Level Acceleration Structure desc
        {
            bottomLevelBuildDesc.ScratchAccelerationStructureData = scratchResource->GetGPUVirtualAddress();
            bottomLevelBuildDesc.DestAccelerationStructureData = m_bottomLevelAccelerationStructure->GetGPUVirtualAddress();
        }

        // Top Level Acceleration Structure desc
        {
            topLevelBuildDesc.DestAccelerationStructureData = m_topLevelAccelerationStructure->GetGPUVirtualAddress();
            topLevelBuildDesc.ScratchAccelerationStructureData = scratchResource->GetGPUVirtualAddress();
            topLevelBuildDesc.Inputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
        }

        auto BuildAccelerationStructure = [&](auto* raytracingCommandList)
        {
            raytracingCommandList->BuildRaytracingAccelerationStructure(&bottomLevelBuildDesc, 0, nullptr);
            gfxContext.GetCommandList()->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_bottomLevelAccelerationStructure.Get()));
            raytracingCommandList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);
        };

        DXRInterface::DXRCommandList dxrCommandList = DxrInterface.GetCommandList(gfxContext);

        // Build acceleration structure.
        BuildAccelerationStructure(dxrCommandList.Get());

        gfxContext.Finish(true);
    }

    ~RTAccelerationSturctures()
    {
        m_bottomLevelAccelerationStructure.Reset();
        m_topLevelAccelerationStructure.Reset();
    }

    ComPtr<ID3D12Resource> m_bottomLevelAccelerationStructure;
    ComPtr<ID3D12Resource> m_topLevelAccelerationStructure;
};

struct RTShaderTables
{
    struct NoParams {};

    using RayGenParameters = NoParams;
    static constexpr bool HasRayGenParameters = !std::is_same<NoParams, RayGenParameters>::value;

    using RayHitParameters = NoParams;
    static constexpr bool HasRayHitParameters = !std::is_same<NoParams, RayHitParameters>::value;

    using RayMissParameters = NoParams;
    static constexpr bool HasRayMissParameters = !std::is_same<NoParams, RayMissParameters>::value;

    RTShaderTables(
        RTPipelineState& PipelineState, 
        const RayGenParameters& RayGenCB =      RayGenParameters(), 
        const RayHitParameters& RayHitCB =      RayHitParameters(),
        const RayMissParameters& RayMissCB =    RayMissParameters())
    {
        void* rayGenShaderIdentifier;
        void* missShaderIdentifier;
        void* hitGroupShaderIdentifier;

        // Get shader identifiers.
        UINT shaderIdentifierSize;
        {
            ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
            ThrowIfFailed(PipelineState.m_dxrStateObject.As(&stateObjectProperties));

            rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ShaderNames::c_raygenShaderName);
            missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ShaderNames::c_missShaderName);
            hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(ShaderNames::c_hitGroupName);

            shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        }

        // Ray gen shader table
        {
            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize ;
            if (HasRayGenParameters)
            {
                shaderRecordSize += sizeof(RayGenParameters);
            }

            ShaderTable rayGenShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
            if (HasRayGenParameters)
            {
                RayGenParameters rootArguments = RayGenCB;
                rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(RayGenParameters)));
            }
            else
            {
                rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize));
            }
            
            m_rayGenShaderTable = rayGenShaderTable.GetResource();
        }

        // Miss shader table
        {
            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize;
            if (HasRayMissParameters)
            {
                shaderRecordSize += sizeof(RayMissParameters);
            }

            ShaderTable missShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"MissShaderTable");
            if (HasRayMissParameters)
            {
                RayMissParameters rootArguments = RayMissCB;
                missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(RayMissParameters)));
            }
            else
            {
                missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
            }

            m_missShaderTable = missShaderTable.GetResource();
        }

        // Hit group shader table
        {
            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize;
            if (HasRayMissParameters)
            {
                shaderRecordSize += sizeof(RayHitParameters);
            }

            ShaderTable hitGroupShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
            if (HasRayMissParameters)
            {
                RayHitParameters rootArguments = RayHitCB;
                hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(RayHitParameters)));
            }
            else
            {
                hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize));
            }

            m_hitGroupShaderTable = hitGroupShaderTable.GetResource();
        }
    }

    ~RTShaderTables()
    {
        m_missShaderTable.Reset();
        m_hitGroupShaderTable.Reset();
        m_rayGenShaderTable.Reset();
    }

    ComPtr<ID3D12Resource> m_missShaderTable;
    ComPtr<ID3D12Resource> m_hitGroupShaderTable;
    ComPtr<ID3D12Resource> m_rayGenShaderTable;
};

struct RTSceneConstantBuffer
{
    // Raw buffer structure, cannot exceed D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT size (IE 256 byte)
    using Buffer = SceneConstantBuffer;

    // We'll allocate space for several of these and they will need to be padded for alignment.
    static_assert(sizeof(Buffer) <= D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, "Scene buffer structure is too big.");

    struct AlignedBuffer
    {
        Buffer constants;
        uint8_t alignmentPadding[D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - sizeof(Buffer)];
    };

    RTSceneConstantBuffer(Buffer&& SceneRawBuffer)
    {
        // Create the constant buffer memory and map the CPU and GPU addresses
        const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

        size_t cbSize = sizeof(AlignedBuffer);
        const D3D12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

        ThrowIfFailed(g_Device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &constantBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_Constants)));

        // Map the constant buffer and cache its heap pointers.
        // We don't unmap this until the app closes. Keeping buffer mapped for the lifetime of the resource is okay.
        CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_Constants->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedConstantData)));

        m_mappedConstantData->constants = SceneRawBuffer;
    }

    ~RTSceneConstantBuffer()
    {
        m_Constants->Unmap(0, nullptr);
        m_Constants.Reset();
        
        // todo see if needed
        //delete m_mappedConstantData;
    }

    AlignedBuffer* m_mappedConstantData;
    ComPtr<ID3D12Resource> m_Constants;
};

struct RTOutputBuffer
{
    // todo see if a preimplemented buffer from the Mini Engine could do the job

    RTOutputBuffer(UINT width, UINT height, RTBuffersDescriptorHeap& Heap)
    {
        // Create the output resource. The dimensions and format should match the swap-chain.
        auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(Config::c_BackBufferFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(g_Device->CreateCommittedResource(
            &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_raytracingOutput)));
        NAME_D3D12_OBJECT(m_raytracingOutput);

        D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
        m_raytracingOutputResourceUAVDescriptorHeapIndex = Heap.AllocateDescriptor(&uavDescriptorHandle, m_raytracingOutputResourceUAVDescriptorHeapIndex);
        D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
        UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_Device->CreateUnorderedAccessView(m_raytracingOutput.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
        m_raytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(Heap.m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), m_raytracingOutputResourceUAVDescriptorHeapIndex, Heap.m_descriptorSize);
    }

    ~RTOutputBuffer()
    {
        m_raytracingOutput.Reset();
    }

    ComPtr<ID3D12Resource> m_raytracingOutput;
    D3D12_GPU_DESCRIPTOR_HANDLE m_raytracingOutputResourceUAVGpuDescriptor;
    UINT m_raytracingOutputResourceUAVDescriptorHeapIndex;
};

class RayTracing : public GameCore::IGameApp
{
public:
    RayTracing() : 
        m_CameraController(new FlyingFPSCamera(m_Camera, Math::Vector3(0,1,0)))
    {
    }

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;

private:
    void InitializeShaders();

private:
    Math::Camera m_Camera;
    std::unique_ptr<FlyingFPSCamera> m_CameraController;

    FlyCamera m_RBCamera;

    DXRInterface* m_dxr = nullptr;

    RTRootSignatures* m_rtRootSignatures = nullptr;

    RTPipelineState* m_rtPipelineStateObject = nullptr;

    SceneGeometry* m_rtScene = nullptr;

    RTAccelerationSturctures* m_rtAccelerationStructures = nullptr;

    RTShaderTables* m_rtShaderTables = nullptr;

    RTBuffersDescriptorHeap* m_rtBufferDescriptorHeap = nullptr;

    RTSceneConstantBuffer* m_rtSceneConstantBuffer = nullptr;

    RTOutputBuffer* m_rtOutputBuffer = nullptr;
};

CREATE_APPLICATION( RayTracing )

void RayTracing::Startup( void )
{
    MotionBlur::Enable = false;
    TemporalEffects::EnableTAA = false;
    FXAA::Enable = false;
    PostEffects::BloomEnable = false;
    PostEffects::EnableHDR = false;
    PostEffects::EnableAdaptation = false;
    SSAO::Enable = true;

    SceneConstantBuffer m_RayGenCB;
    m_RayGenCB.viewport = { -1.0f, -1.0f, 1.0f, 1.0f };
    m_RayGenCB.stencil = { -1.0f, -1.0f, 1.0f, 1.0f };
    //m_RayGenCB.stencil =  { -0.25f, -0.25f, 0.25f, 0.25f };

    m_RBCamera.SetProjection(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight(), 90, 0.15f, 100.0f);
    m_RBCamera.SetTranslation(-4.75, 0, 0);

    //m_CameraController->SetHeadingPitchAndPosition(0, 0, Vector3(-2,0,0));

    //m_Camera.SetPosition(Vector3(-1,0,0));
    //m_Camera.SetRotation(Quaternion(0,0,0));
    //m_Camera.SetLookDirection(Vector3(1, 0, 0), Vector3(0, 1, 0));
    //m_Camera.Update();
    
    // Setup your data
    m_dxr = new DXRInterface();
    m_rtRootSignatures = new RTRootSignatures(m_RayGenCB);
    m_rtPipelineStateObject = new RTPipelineState(*m_dxr, *m_rtRootSignatures);

    // Allocate a heap for 3 descriptors:
    // 2 - vertex and index buffer SRVs
    // 1 - raytracing output texture SRV
    m_rtBufferDescriptorHeap = new RTBuffersDescriptorHeap(3);

    m_rtScene = new SceneGeometry(*m_rtBufferDescriptorHeap);
    m_rtAccelerationStructures = new RTAccelerationSturctures(*m_rtScene, *m_dxr);

    m_rtShaderTables = new RTShaderTables(*m_rtPipelineStateObject);

    m_rtSceneConstantBuffer = new RTSceneConstantBuffer(std::move(m_RayGenCB));
    m_rtOutputBuffer = new RTOutputBuffer(g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight(), *m_rtBufferDescriptorHeap);

}

void RayTracing::Cleanup( void )
{
    // Free up resources in an orderly fashion
    delete m_rtOutputBuffer;
    delete m_rtBufferDescriptorHeap;
    delete m_rtShaderTables;
    delete m_rtAccelerationStructures;
    delete m_rtScene;
    delete m_rtPipelineStateObject;
    delete m_rtRootSignatures;
    delete m_dxr;
}

void RayTracing::Update( float deltaT )
{
    ScopedTimer _prof(L"Update State");

    Math::Vector3 PositionDir(0, 0, 0);
    float rotateDir = 0.0f;
    const float speed = 1.5f;

    if (GameInput::IsPressed(GameInput::kKey_up))
        PositionDir.SetY(PositionDir.GetY().operator float() + 1);
    if (GameInput::IsPressed(GameInput::kKey_down))
        PositionDir.SetY(PositionDir.GetY().operator float() - 1);
    if (GameInput::IsPressed(GameInput::kKey_w))
        PositionDir.SetX(PositionDir.GetX().operator float() + 1);
    if (GameInput::IsPressed(GameInput::kKey_s))
        PositionDir.SetX(PositionDir.GetX().operator float() - 1);
    if (GameInput::IsPressed(GameInput::kKey_a))
        PositionDir.SetZ(PositionDir.GetZ().operator float() - 1);
    if (GameInput::IsPressed(GameInput::kKey_d))
        PositionDir.SetZ(PositionDir.GetZ().operator float() + 1);
    PositionDir = PositionDir * deltaT * speed * 2.0f;
    m_RBCamera.Translate(m_RBCamera.GetWorldRotation() * PositionDir);

    if (GameInput::IsPressed(GameInput::kKey_q))
        rotateDir += 1;
    if (GameInput::IsPressed(GameInput::kKey_e))
        rotateDir -= 1;
    m_RBCamera.RotateRadians(0, rotateDir * M_PI * deltaT * speed / 5);

    //m_CameraController->Update(deltaT);
}

void RayTracing::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render Ray Traced");

    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.ClearColor(g_SceneColorBuffer);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

    // Update camera data
    m_rtSceneConstantBuffer->m_mappedConstantData->constants.WorldToProjectedSpace = Transpose(m_RBCamera.Projection() * m_RBCamera.View());
    m_rtSceneConstantBuffer->m_mappedConstantData->constants.ProjectedSpaceToWorld = Transpose(m_RBCamera.InverseView() * m_RBCamera.InverseProjection());
    m_rtSceneConstantBuffer->m_mappedConstantData->constants.CameraPosition = m_RBCamera.GetWorldPosition();
    m_rtSceneConstantBuffer->m_mappedConstantData->constants.CameraDirection = m_RBCamera.GetWorldDirection();


    // Dispatch rays draw call execution
    auto DispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
    {
        // Since each shader table has only one shader record, the stride is same as the size.
        dispatchDesc->HitGroupTable.StartAddress = m_rtShaderTables->m_hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc->HitGroupTable.SizeInBytes = m_rtShaderTables->m_hitGroupShaderTable->GetDesc().Width;
        dispatchDesc->HitGroupTable.StrideInBytes = dispatchDesc->HitGroupTable.SizeInBytes;

        dispatchDesc->MissShaderTable.StartAddress = m_rtShaderTables->m_missShaderTable->GetGPUVirtualAddress();
        dispatchDesc->MissShaderTable.SizeInBytes = m_rtShaderTables->m_missShaderTable->GetDesc().Width;
        dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;

        dispatchDesc->RayGenerationShaderRecord.StartAddress = m_rtShaderTables->m_rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_rtShaderTables->m_rayGenShaderTable->GetDesc().Width;


        dispatchDesc->Width = g_SceneColorBuffer.GetWidth();
        dispatchDesc->Height = g_SceneColorBuffer.GetHeight();
        dispatchDesc->Depth = 1;

        commandList->SetPipelineState1(stateObject);
        commandList->DispatchRays(dispatchDesc);
    };

    ID3D12GraphicsCommandList* CommandList = gfxContext.GetCommandList();
    DXRInterface::DXRCommandList DxrCommandList = m_dxr->GetCommandList(gfxContext);

    CommandList->SetComputeRootSignature(m_rtRootSignatures->m_raytracingGlobalRootSignature.Get());

    // Bind the heaps, buffers, acceleration structure and dispatch rays.    
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    CommandList->SetDescriptorHeaps(1, m_rtBufferDescriptorHeap->m_descriptorHeap.GetAddressOf());
    CommandList->SetComputeRootDescriptorTable(RTRootSignatures::GlobalRootSignatureParams::OutputViewSlot, m_rtOutputBuffer-> m_raytracingOutputResourceUAVGpuDescriptor);
    CommandList->SetComputeRootDescriptorTable(RTRootSignatures::GlobalRootSignatureParams::VertexBufferSlot, m_rtScene->m_indexBuffer.gpuDescriptorHandle);
    CommandList->SetComputeRootShaderResourceView(RTRootSignatures::GlobalRootSignatureParams::AccelerationStructureSlot, m_rtAccelerationStructures->m_topLevelAccelerationStructure->GetGPUVirtualAddress());

    // Copy the updated scene constant buffer to GPU.
    CommandList->SetComputeRootConstantBufferView(RTRootSignatures::GlobalRootSignatureParams::SceneConstantSlot, m_rtSceneConstantBuffer->m_Constants->GetGPUVirtualAddress());

    // Dispatch rays draw call
    DispatchRays(DxrCommandList.Get(), m_rtPipelineStateObject->m_dxrStateObject.Get(), &dispatchDesc);

    // Copy result to back buffer
    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(g_SceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rtOutputBuffer->m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CommandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    CommandList->CopyResource(g_SceneColorBuffer.GetResource(), m_rtOutputBuffer->m_raytracingOutput.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(g_SceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rtOutputBuffer->m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CommandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);

    gfxContext.Finish();
}



void RayTracing::InitializeShaders()
{


    //Microsoft::WRL::ComPtr<ID3DBlob> bloob = CompileShader(L"Shaders/EmitSDFCS.hlsl", nullptr, "main", "cs_5_1");
}
