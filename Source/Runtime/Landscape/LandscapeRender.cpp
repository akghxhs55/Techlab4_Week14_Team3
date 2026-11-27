#include "EnginePCH.h"
#include "LandscapeRender.h"

#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeProxy.h"
#include "Asset/AssetManager.h"
#include "Render/Buffer.h"
#include "Render/Material.h"
#include "Render/RenderCommand.h"
#include "Render/Texture2D.h"
#include <algorithm>
#include <cfloat>
#include <memory>

namespace
{
    TMap<int32, TWeakPtr<FLandscapeSharedBuffers>>& GetSharedBuffersCache()
    {
        static TMap<int32, TWeakPtr<FLandscapeSharedBuffers>> Cache;
        return Cache;
    }

    uint8 PackUnit(float V)
    {
        return static_cast<uint8>(std::clamp(V * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

TSharedPtr<FLandscapeSharedBuffers> FLandscapeSharedBuffers::GetOrCreate(int32 SubsectionSizeVerts)
{
    TWeakPtr<FLandscapeSharedBuffers>& Slot = GetSharedBuffersCache().FindOrAdd(SubsectionSizeVerts);
    if (TSharedPtr<FLandscapeSharedBuffers> Existing = Slot.lock())
    {
        return Existing;
    }
    TSharedPtr<FLandscapeSharedBuffers> Created = std::make_shared<FLandscapeSharedBuffers>(SubsectionSizeVerts);
    Slot = Created;
    return Created;
}

FLandscapeSharedBuffers::FLandscapeSharedBuffers(int32 InSubsectionSizeVerts)
    : SubsectionSizeVerts(InSubsectionSizeVerts)
{
    assert(InSubsectionSizeVerts >= 2 && InSubsectionSizeVerts <= 256);
    assert((InSubsectionSizeVerts & (InSubsectionSizeVerts - 1)) == 0);

    std::vector<FLandscapeVertex> Vertices;
    for (int32 LOD = 0, Verts = SubsectionSizeVerts; Verts >= 2; ++LOD, Verts >>= 1)
    {
        LODVertexOffsets.push_back(static_cast<int32>(Vertices.size()));
        for (int32 Y = 0; Y < Verts; ++Y)
        {
            for (int32 X = 0; X < Verts; ++X)
            {
                Vertices.push_back({ static_cast<uint8>(X), static_cast<uint8>(Y), static_cast<uint8>(LOD), 0 });
            }
        }

        std::vector<uint32> Indices;
        Indices.reserve(static_cast<size_t>((Verts - 1) * (Verts - 1) * 6));
        for (int32 Y = 0; Y < Verts - 1; ++Y)
        {
            for (int32 X = 0; X < Verts - 1; ++X)
            {
                const uint32 I00 = static_cast<uint32>(Y * Verts + X);
                const uint32 I10 = I00 + 1;
                const uint32 I01 = I00 + static_cast<uint32>(Verts);
                const uint32 I11 = I01 + 1;
                Indices.insert(Indices.end(), { I00, I10, I11 });
                Indices.insert(Indices.end(), { I00, I11, I01 });
            }
        }
        IndexBuffers.push_back(RenderCommand::CreateStaticIndexBuffer(Indices.data(), static_cast<uint32>(Indices.size())));
    }

    VertexBuffer = RenderCommand::CreateStaticVertexBuffer(Vertices.data(), static_cast<uint32>(Vertices.size() * sizeof(FLandscapeVertex)), sizeof(FLandscapeVertex));

    KRS_LOG(Info, "[Landscape] SharedBuffers: {} verts/side, {} LODs, {} total verts", SubsectionSizeVerts, GetNumLODs(), Vertices.size());
}

FLandscapeSharedBuffers::~FLandscapeSharedBuffers() = default;

UTexture2D* LandscapeRender::CreateHeightmapTexture(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY, int32 BaseX, int32 BaseY, int32 SubsectionSizeVerts)
{
    auto RawAt = [&](int32 X, int32 Y) -> uint16
        {
            X = std::clamp(X, 0, SizeX - 1);
            Y = std::clamp(Y, 0, SizeY - 1);
            return Heights[Y * SizeX + X];
        };
    auto HeightAt = [&](int32 X, int32 Y) -> float
        {
            return LandscapeDataAccess::GetLocalHeight(RawAt(X, Y));
        };
    
    const int32 Size = SubsectionSizeVerts;
    std::vector<uint8> Pixels(static_cast<size_t>(Size * Size * 4));

    for (int32 Y = 0; Y < Size; ++Y)
    {
        for (int32 X = 0; X < Size; ++X)
        {
            const int32 GX = std::clamp(BaseX + X, 0, SizeX - 1);
            const int32 GY = std::clamp(BaseY + Y, 0, SizeY - 1);
            const uint16 Raw = RawAt(GX, GY);

            // 중앙차분 (경계는 한쪽 차분). 로컬 단위(1쿼드 = 1)라서 액터 스케일은 NormalMatrix가 반영한다.
            const int32 X0 = std::max(GX - 1, 0), X1 = std::min(GX + 1, SizeX - 1);
            const int32 Y0 = std::max(GY - 1, 0), Y1 = std::min(GY + 1, SizeY - 1);
            const float DhDx = (HeightAt(X1, GY) - HeightAt(X0, GY)) / static_cast<float>(std::max(X1 - X0, 1));
            const float DhDy = (HeightAt(GX, Y1) - HeightAt(GX, Y0)) / static_cast<float>(std::max(Y1 - Y0, 1));
            const FVector N = FVector(-DhDx, -DhDy, 1.0f).Normalized();

            uint8* P = &Pixels[static_cast<size_t>((Y * Size + X) * 4)];
            P[0] = static_cast<uint8>(Raw >> 8);   // R: 높이 상위 바이트
            P[1] = static_cast<uint8>(Raw & 0xFF); // G: 높이 하위 바이트
            P[2] = PackUnit(N.X);                  // B: 노멀 X
            P[3] = PackUnit(N.Y);                  // A: 노멀 Y
        }
    }

    return UTexture2D::CreateDynamicTexture(static_cast<uint32>(Size), static_cast<uint32>(Size), Pixels.data());
}

FBox LandscapeRender::CalcSectionLocalBox(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY, int32 BaseX, int32 BaseY, int32 SectionSizeQuads)
{
    float MinZ = FLT_MAX;
    float MaxZ = -FLT_MAX;
    const int32 EndX = std::min(BaseX + SectionSizeQuads, SizeX - 1);
    const int32 EndY = std::min(BaseY + SectionSizeQuads, SizeY - 1);
    for (int32 Y = BaseY; Y <= EndY; ++Y)
    {
        for (int32 X = BaseX; X <= EndX; ++X)
        {
            const float Z = LandscapeDataAccess::GetLocalHeight(Heights[Y * SizeX + X]);
            MinZ = std::min(MinZ, Z);
            MaxZ = std::max(MaxZ, Z);
        }
    }

    FBox Box;
    Box.Min = FVector(0.0f, 0.0f, MinZ);
    Box.Max = FVector(static_cast<float>(SectionSizeQuads), static_cast<float>(SectionSizeQuads), MaxZ);
    return Box;
}

FLandscapeSceneProxy::FLandscapeSceneProxy(ULandscapeComponent* InComponent)
    : FPrimitiveSceneProxy(InComponent)
{
    const int32 SubsectionSizeVerts = InComponent->ComponentSizeQuads + 1;
    SharedBuffers = FLandscapeSharedBuffers::GetOrCreate(SubsectionSizeVerts);

    FLandscapeShaderParams Params{};
    Params.SectionSizeQuads = static_cast<float>(InComponent->ComponentSizeQuads);
    Params.SubsectionSizeVerts = static_cast<float>(SubsectionSizeVerts);
    Params.LODIndex = static_cast<float>(CurrentLOD);
    ParamsBuffer = RenderCommand::CreateConstantBuffer(sizeof(FLandscapeShaderParams));
    RenderCommand::UpdateBufferData(ParamsBuffer.get(), &Params, sizeof(Params));

    VertexFactory.Init(SharedBuffers->GetVertexBuffer(), SharedBuffers->GetIndexBuffer(CurrentLOD), InComponent->GetHeightmap(), ParamsBuffer.get());
    
    ALandscapeProxy* Landscape = InComponent->GetLandscapeProxy();
    Material = (Landscape && Landscape->LandscapeMaterial) ? Landscape->LandscapeMaterial : UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
}

FLandscapeSceneProxy::~FLandscapeSceneProxy() = default;

void FLandscapeSceneProxy::UpdateRenderState()
{
    RenderState.reset();
    UpdateCommonFlags();
}

void FLandscapeSceneProxy::GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex) const
{
    static int32 GDbgCount = 0;
    if (GDbgCount < 3)
    {
        ++GDbgCount;
        KRS_LOG(Info, "[LandscapeDbg] GetDynamicMeshElements: sb={} vfValid={} mat={} idx={} baseVtx={}",
            SharedBuffers ? 1 : 0, VertexFactory.IsValid() ? 1 : 0, Material ? 1 : 0,
            SharedBuffers ? SharedBuffers->GetIndexBuffer(CurrentLOD)->GetIndexCount() : 0,
            SharedBuffers ? SharedBuffers->GetLODVertexOffset(CurrentLOD) : 0);
    }

    if (!SharedBuffers || !VertexFactory.IsValid() || !Material)
    {
        return;
    }

    const int32 LOD = CurrentLOD;
    FIndexBuffer* IB = SharedBuffers->GetIndexBuffer(LOD);

    FMeshBatch& Batch = Collector.AddMeshDefaulted();
    Batch.VertexFactory = &VertexFactory;
    Batch.Material = Material;
    Batch.LODIndex = static_cast<uint8>(LOD);
    Batch.bCastShadow = bCastsShadow;
    Batch.bUsesGPUScene = true;

    FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
    Element.Proxy = this;
    Element.IndexBuffer = IB;
    Element.FirstIndex = 0;
    Element.IndexCount = IB->GetIndexCount();
    Element.BaseVertexLocation = SharedBuffers->GetLODVertexOffset(LOD);
    Element.bReceiveShadow = bReceiveShadow;
    Element.bReceiveDecal = bReceivesDecals;
}
