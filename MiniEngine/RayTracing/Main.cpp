
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
            Count
        };
    };

    struct LocalRootSignatureParams {
        enum Value {
            ViewportConstantSlot = 0,
            Count
        };
    };

    RTRootSignatures(RayGenConstantBuffer& RayGenCB)
    {
        // Global Root Signature
        {
            CD3DX12_DESCRIPTOR_RANGE UAVDescriptor;
            UAVDescriptor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParams::Count];
            rootParameters[GlobalRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &UAVDescriptor);
            rootParameters[GlobalRootSignatureParams::AccelerationStructureSlot].InitAsShaderResourceView(0);
            CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
            SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_raytracingGlobalRootSignature);
        }

        // Local Root Signature
        {
            CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
            rootParameters[LocalRootSignatureParams::ViewportConstantSlot].InitAsConstants(SizeOfInUint32(RayGenCB), 0, 0);
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

struct SceneGeometry
{
    // todo use real mesh and paramters

    typedef UINT16 Index;
    struct Vertex { float v1, v2, v3; };

    SceneGeometry()
    {
        Index indices[] =
        {
            0, 1, 2
        };

        const float depthValue = 1.0;
        const float offset = 0.7f;
        Vertex vertices[] =
        {
            // The sample raytraces in screen space coordinates.
            // Since DirectX screen space coordinates are right handed (i.e. Y axis points down).
            // Define the vertices in counter clockwise order ~ clockwise in left handed.
            { 0, -offset, depthValue },
            { -offset, offset, depthValue },
            { offset, offset, depthValue }
        };

        AllocateUploadBuffer(g_Device, vertices, sizeof(vertices), &m_vertexBuffer);
        AllocateUploadBuffer(g_Device, indices, sizeof(indices), &m_indexBuffer);
    }

    ~SceneGeometry()
    {
        m_indexBuffer.Reset();
        m_vertexBuffer.Reset();
    }

    ComPtr<ID3D12Resource> m_indexBuffer;
    ComPtr<ID3D12Resource> m_vertexBuffer;
};

//Acceleration structures needed for raytracing.
struct RTAccelerationSturctures
{
    RTAccelerationSturctures(SceneGeometry& Geometry, DXRInterface& DxrInterface)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDesc.Triangles.IndexBuffer = Geometry.m_indexBuffer->GetGPUVirtualAddress();
        geometryDesc.Triangles.IndexCount = static_cast<UINT>(Geometry.m_indexBuffer->GetDesc().Width) / sizeof(SceneGeometry::Index);
        geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geometryDesc.Triangles.Transform3x4 = 0;
        geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDesc.Triangles.VertexCount = static_cast<UINT>(Geometry.m_vertexBuffer->GetDesc().Width) / sizeof(SceneGeometry::Vertex);
        geometryDesc.Triangles.VertexBuffer.StartAddress = Geometry.m_vertexBuffer->GetGPUVirtualAddress();
        geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(SceneGeometry::Vertex);

        // todo move to SceneGeometry param
        // Mark the geometry as opaque. 
        // PERFORMANCE TIP: mark geometry as opaque whenever applicable as it can enable important ray processing optimizations.
        // Note: When rays encounter opaque geometry an any hit shader will not be executed whether it is present or not.
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

        // Get required sizes for an acceleration structure.
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs = {};
        topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        topLevelInputs.Flags = buildFlags;
        topLevelInputs.NumDescs = 1;
        topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
        DxrInterface.m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
        ThrowIfFalse(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomLevelInputs = topLevelInputs;
        bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomLevelInputs.pGeometryDescs = &geometryDesc;
        DxrInterface.m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &bottomLevelPrebuildInfo);
        ThrowIfFalse(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

        ComPtr<ID3D12Resource> scratchResource;
        AllocateUAVBuffer(g_Device, max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes), &scratchResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");

        // Allocate resources for acceleration structures.
        // Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
        // Default heap is OK since the application doesn’t need CPU read/write access to them. 
        // The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
        // and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
        //  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
        //  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.
        {
            D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

            AllocateUAVBuffer(g_Device, bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes, &m_bottomLevelAccelerationStructure, initialResourceState, L"BottomLevelAccelerationStructure");
            AllocateUAVBuffer(g_Device, topLevelPrebuildInfo.ResultDataMaxSizeInBytes, &m_topLevelAccelerationStructure, initialResourceState, L"TopLevelAccelerationStructure");
        }

        // Create an instance desc for the bottom-level acceleration structure.
        ComPtr<ID3D12Resource> instanceDescs;
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = instanceDesc.Transform[1][1] = instanceDesc.Transform[2][2] = 1;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructure->GetGPUVirtualAddress();
        AllocateUploadBuffer(g_Device, &instanceDesc, sizeof(instanceDesc), &instanceDescs, L"InstanceDescs");

        // Bottom Level Acceleration Structure desc
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
        {
            bottomLevelBuildDesc.Inputs = bottomLevelInputs;
            bottomLevelBuildDesc.ScratchAccelerationStructureData = scratchResource->GetGPUVirtualAddress();
            bottomLevelBuildDesc.DestAccelerationStructureData = m_bottomLevelAccelerationStructure->GetGPUVirtualAddress();
        }

        // Top Level Acceleration Structure desc
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {};
        {
            topLevelInputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
            topLevelBuildDesc.Inputs = topLevelInputs;
            topLevelBuildDesc.DestAccelerationStructureData = m_topLevelAccelerationStructure->GetGPUVirtualAddress();
            topLevelBuildDesc.ScratchAccelerationStructureData = scratchResource->GetGPUVirtualAddress();
        }

        GraphicsContext& gfxContext = GraphicsContext::Begin(L"Acceleration structure creation");

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
    RTShaderTables(RTPipelineState& PipelineState, const RayGenConstantBuffer& RayGenCB)
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
            struct RootArguments {
                RayGenConstantBuffer cb;
            } rootArguments;
            rootArguments.cb = RayGenCB;

            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize + sizeof(rootArguments);
            ShaderTable rayGenShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"RayGenShaderTable");
            rayGenShaderTable.push_back(ShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize, &rootArguments, sizeof(rootArguments)));
            m_rayGenShaderTable = rayGenShaderTable.GetResource();
        }

        // Miss shader table
        {
            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize;
            ShaderTable missShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"MissShaderTable");
            missShaderTable.push_back(ShaderRecord(missShaderIdentifier, shaderIdentifierSize));
            m_missShaderTable = missShaderTable.GetResource();
        }

        // Hit group shader table
        {
            UINT numShaderRecords = 1;
            UINT shaderRecordSize = shaderIdentifierSize;
            ShaderTable hitGroupShaderTable(g_Device, numShaderRecords, shaderRecordSize, L"HitGroupShaderTable");
            hitGroupShaderTable.push_back(ShaderRecord(hitGroupShaderIdentifier, shaderIdentifierSize));
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

struct RTOutputBuffer
{
    // todo see if a preimplemented buffer from the Mini Engine could do the job

    struct DescriptorHeap
    {
        DescriptorHeap()
        {
            D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
            // Allocate a heap for a single descriptor:
            // 1 - raytracing output texture UAV
            descriptorHeapDesc.NumDescriptors = 1;
            descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            descriptorHeapDesc.NodeMask = 0;
            g_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
            NAME_D3D12_OBJECT(m_descriptorHeap);

            m_descriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        ~DescriptorHeap()
        {
            m_descriptorHeap.Reset();
        }

        UINT AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse)
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
    };

    RTOutputBuffer(UINT width, UINT height, DescriptorHeap& Heap)
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

    RayTracing()
    {
    }

    virtual void Startup( void ) override;
    virtual void Cleanup( void ) override;

    virtual void Update( float deltaT ) override;
    virtual void RenderScene( void ) override;

private:
    void InitializeShaders();

private:
    Math::Camera Camera;
    std::unique_ptr<CameraController> CameraController;

    DXRInterface* m_dxr = nullptr;

    RTRootSignatures* m_rtRootSignatures = nullptr;

    RTPipelineState* m_rtPipelineStateObject = nullptr;

    SceneGeometry* m_rtScene = nullptr;

    RTAccelerationSturctures* m_rtAccelerationStructures = nullptr;

    RTShaderTables* m_rtShaderTables = nullptr;

    RTOutputBuffer::DescriptorHeap* m_rtBufferDescriptorHeap = nullptr;

    RTOutputBuffer* m_rtOutputBuffer = nullptr;

    RayGenConstantBuffer m_RayGenCB;
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

    m_RayGenCB.viewport = { -1.0f, -1.0f, 1.0f, 1.0f };

    // Setup your data
    m_dxr = new DXRInterface();
    m_rtRootSignatures = new RTRootSignatures(m_RayGenCB);
    m_rtPipelineStateObject = new RTPipelineState(*m_dxr, *m_rtRootSignatures);

    m_rtScene = new SceneGeometry();
    m_rtAccelerationStructures = new RTAccelerationSturctures(*m_rtScene, *m_dxr);

    m_rtShaderTables = new RTShaderTables(*m_rtPipelineStateObject, m_RayGenCB);

    m_rtBufferDescriptorHeap = new RTOutputBuffer::DescriptorHeap();
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

void RayTracing::Update( float /*deltaT*/ )
{
    ScopedTimer _prof(L"Update State");

    // Update something
}

void RayTracing::RenderScene( void )
{
    GraphicsContext& gfxContext = GraphicsContext::Begin(L"Scene Render Ray Traced");

    gfxContext.TransitionResource(g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
    gfxContext.ClearColor(g_SceneColorBuffer);
    gfxContext.SetRenderTarget(g_SceneColorBuffer.GetRTV());
    gfxContext.SetViewportAndScissor(0, 0, g_SceneColorBuffer.GetWidth(), g_SceneColorBuffer.GetHeight());

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

    // Bind the heaps, acceleration structure and dispatch rays.    
    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    CommandList->SetDescriptorHeaps(1, m_rtBufferDescriptorHeap->m_descriptorHeap.GetAddressOf());
    CommandList->SetComputeRootDescriptorTable(RTRootSignatures::GlobalRootSignatureParams::OutputViewSlot, m_rtOutputBuffer-> m_raytracingOutputResourceUAVGpuDescriptor);
    CommandList->SetComputeRootShaderResourceView(RTRootSignatures::GlobalRootSignatureParams::AccelerationStructureSlot, m_rtAccelerationStructures->m_topLevelAccelerationStructure->GetGPUVirtualAddress());
    DispatchRays(DxrCommandList.Get(), m_rtPipelineStateObject->m_dxrStateObject.Get(), &dispatchDesc);

    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(g_SceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rtOutputBuffer->m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CommandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    CommandList->CopyResource(g_SceneColorBuffer.GetResource(), m_rtOutputBuffer->m_raytracingOutput.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(g_SceneColorBuffer.GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rtOutputBuffer->m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CommandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);

    gfxContext.Finish();
}



void RayTracing::InitializeShaders()
{


    //Microsoft::WRL::ComPtr<ID3DBlob> bloob = CompileShader(L"Shaders/EmitSDFCS.hlsl", nullptr, "main", "cs_5_1");
}
