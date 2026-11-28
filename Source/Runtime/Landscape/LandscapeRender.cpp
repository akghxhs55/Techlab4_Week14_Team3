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
    std::vector<uint32> Indices;
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

        LODFirstIndices.push_back(static_cast<uint32>(Indices.size()));
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
        LODIndexCounts.push_back(static_cast<uint32>(Indices.size()) - LODFirstIndices.back());
    }

    VertexBuffer = RenderCommand::CreateStaticVertexBuffer(Vertices.data(), static_cast<uint32>(Vertices.size() * sizeof(FLandscapeVertex)), sizeof(FLandscapeVertex));
    IndexBuffer = RenderCommand::CreateStaticIndexBuffer(Indices.data(), static_cast<uint32>(Indices.size()));

    KRS_LOG(Info, "[Landscape] SharedBuffers: {} verts/side, {} LODs, {} total verts, {} total indices", SubsectionSizeVerts, GetNumLODs(), Vertices.size(), Indices.size());
}

FLandscapeSharedBuffers::~FLandscapeSharedBuffers() = default;

UTexture2D* LandscapeRender::CreateHeightmapTexture(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY, int32 BaseX, int32 BaseY, int32 SubsectionSizeVerts)
{
    auto RawAt = [&](int32 X, int32 Y) -> float
        {
            X = std::clamp(X, 0, SizeX - 1);
            Y = std::clamp(Y, 0, SizeY - 1);
            return static_cast<float>(Heights[Y * SizeX + X]);
        };
    auto SampleRaw = [&](float GX, float GY) -> float
        {
            GX = std::clamp(GX, 0.0f, static_cast<float>(SizeX - 1));
            GY = std::clamp(GY, 0.0f, static_cast<float>(SizeY - 1));
            const int32 X0 = static_cast<int32>(std::floor(GX));
            const int32 Y0 = static_cast<int32>(std::floor(GY));
            const float TX = GX - static_cast<float>(X0);
            const float TY = GY - static_cast<float>(Y0);
            const float Top = RawAt(X0, Y0) + (RawAt(X0 + 1, Y0) - RawAt(X0, Y0)) * TX;
            const float Bottom = RawAt(X0, Y0 + 1) + (RawAt(X0 + 1, Y0 + 1) - RawAt(X0, Y0 + 1)) * TX;
            return Top + (Bottom - Top) * TY;
        };
    auto LocalHeight = [&](float GX, float GY) -> float
        {
            return LandscapeDataAccess::GetLocalHeight(static_cast<uint16>(std::lround(SampleRaw(GX, GY))));
        };

    const int32 Size = SubsectionSizeVerts;
    std::vector<std::vector<uint8>> Mips;

    for (int32 Verts = Size; Verts >= 2; Verts >>= 1)
    {
        const float Step = static_cast<float>(Size - 1) / static_cast<float>(Verts - 1);
        std::vector<uint8>& Pixels = Mips.emplace_back(static_cast<size_t>(Verts * Verts * 4));

        for (int32 Y = 0; Y < Verts; ++Y)
        {
            for (int32 X = 0; X < Verts; ++X)
            {
                const float GX = static_cast<float>(BaseX) + static_cast<float>(X) * Step;
                const float GY = static_cast<float>(BaseY) + static_cast<float>(Y) * Step;
                const uint16 Raw = static_cast<uint16>(std::lround(SampleRaw(GX, GY)));

                const float X0 = std::max(GX - 1.0f, 0.0f), X1 = std::min(GX + 1.0f, static_cast<float>(SizeX - 1));
                const float Y0 = std::max(GY - 1.0f, 0.0f), Y1 = std::min(GY + 1.0f, static_cast<float>(SizeY - 1));
                const float DhDx = (LocalHeight(X1, GY) - LocalHeight(X0, GY)) / std::max(X1 - X0, 1.0f);
                const float DhDy = (LocalHeight(GX, Y1) - LocalHeight(GX, Y0)) / std::max(Y1 - Y0, 1.0f);
                const FVector N = FVector(-DhDx, -DhDy, 1.0f).Normalized();

                uint8* P = &Pixels[static_cast<size_t>((Y * Verts + X) * 4)];
                P[0] = static_cast<uint8>(Raw >> 8);
                P[1] = static_cast<uint8>(Raw & 0xFF);
                P[2] = PackUnit(N.X);
                P[3] = PackUnit(N.Y);
            }
        }
    }
    return UTexture2D::CreateDynamicTextureMips(static_cast<uint32>(Size), static_cast<uint32>(Size), Mips);
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
    SectionBaseX = InComponent->SectionBaseX;
    SectionBaseY = InComponent->SectionBaseY;

    ALandscapeProxy* Landscape = InComponent->GetLandscapeProxy();

    LastLODIndex = std::min(SharedBuffers->GetNumLODs() - 1, 3);

    const float LOD0ScreenSize = Landscape ? Landscape->LOD0ScreenSize : 0.5f;
    const float LOD0Distribution = std::max(Landscape ? Landscape->LOD0DistributionSetting : 1.25f, 1.01f);
    const float LODDistribution = std::max(Landscape ? Landscape->LODDistributionSetting : 3.0f, 1.01f);

    const float LOD1ScreenSize = LOD0ScreenSize / LOD0Distribution;
    LOD0ScreenSizeSq = LOD0ScreenSize * LOD0ScreenSize;
    LOD1ScreenSizeSq = LOD1ScreenSize * LOD1ScreenSize;
    LogLODDistributionSq = std::log(LODDistribution * LODDistribution);

    LastLODScreenSizeSq = LOD1ScreenSizeSq / std::pow(LODDistribution * LODDistribution, static_cast<float>(LastLODIndex - 1));

    FLandscapeShaderParams Params{};
    Params.SectionSizeQuads = static_cast<float>(InComponent->ComponentSizeQuads);
    Params.SubsectionSizeVerts = static_cast<float>(SubsectionSizeVerts);
    Params.LODIndex = static_cast<float>(CurrentLOD);
    ParamsBuffer = RenderCommand::CreateConstantBuffer(sizeof(FLandscapeShaderParams));
    RenderCommand::UpdateBufferData(ParamsBuffer.get(), &Params, sizeof(Params));

    VertexFactory.Init(SharedBuffers->GetVertexBuffer(), SharedBuffers->GetIndexBuffer(), InComponent->GetHeightmap(), ParamsBuffer.get());
    
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
            SharedBuffers ? SharedBuffers->GetIndexBuffer()->GetIndexCount() : 0,
            SharedBuffers ? SharedBuffers->GetLODVertexOffset(CurrentLOD) : 0);
    }

    if (!SharedBuffers || !VertexFactory.IsValid() || !Material)
    {
        return;
    }

    const float FractionalLOD = ComputeFractionalLOD(View);
    const int32 DesiredLOD = static_cast<int32>(FractionalLOD);

    if (View.ViewIndex == 0 && DesiredLOD != DebugLastLOD)
    {
        KRS_LOG(Info, "[LandscapeLOD] Section ({}, {}) LOD {} -> {} (fractional {:.2f})",
            SectionBaseX, SectionBaseY, DebugLastLOD, DesiredLOD, FractionalLOD);
        DebugLastLOD = DesiredLOD;
    }

    const int32 LOD = DesiredLOD;

    FMeshBatch& Batch = Collector.AddMeshDefaulted();
    Batch.VertexFactory = &VertexFactory;
    Batch.Material = Material;
    Batch.LODIndex = static_cast<uint8>(LOD);
    Batch.bCastShadow = bCastsShadow;
    Batch.bUsesGPUScene = true;

    FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
    Element.Proxy = this;
    Element.IndexBuffer = SharedBuffers->GetIndexBuffer();
    Element.FirstIndex = SharedBuffers->GetLODFirstIndex(LOD);
    Element.IndexCount = SharedBuffers->GetLODIndexCount(LOD);
    Element.BaseVertexLocation = SharedBuffers->GetLODVertexOffset(LOD);
    Element.bReceiveShadow = bReceiveShadow;
    Element.bReceiveDecal = bReceivesDecals;
}

float FLandscapeSceneProxy::ComputeFractionalLOD(const FViewContext& View) const
{
    const FLODSphere& Sphere = GetLODSphere();

    float DistanceSq = 1.0f;
    if (!View.bOrthographic)
    {
        const FVector ToCenter = Sphere.Center - View.CameraPosition;
        DistanceSq = std::max(ToCenter.Dot(ToCenter), 1.0f);
    }

    const float ScreenSizeSq = Sphere.RadiusSquared * View.ProjectionScaleSquared / DistanceSq;

    if (ScreenSizeSq <= LastLODScreenSizeSq)
    {
        return static_cast<float>(LastLODIndex);
    }
    if (ScreenSizeSq > LOD1ScreenSizeSq)
    {
        // LOD0 ~ LOD1 선형
        return (LOD0ScreenSizeSq - std::min(ScreenSizeSq, LOD0ScreenSizeSq)) / (LOD0ScreenSizeSq - LOD1ScreenSizeSq);
    }

    // LOD1 이후
    return 1.0f + std::log(LOD1ScreenSizeSq / ScreenSizeSq) / LogLODDistributionSq;
}
