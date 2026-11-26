#include "EnginePCH.h"
#include "Mesh.h"
#include "Renderer.h"
#include "Asset/AssetManager.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsCore.h"
#include <PxPhysics.h>
#include <cooking/PxCooking.h>
#include <algorithm>
#include <cmath>
#include <limits>

UStaticMesh::~UStaticMesh()
{
    for (FPrimitiveSceneProxy* Proxy : RenderProxies)
        Proxy->OnMeshDestroyed();
	VertexBuffer = nullptr;
	IndexBuffer = nullptr;
}

void UStaticMesh::ClearBodySetup(const UBodySetup* InBodySetup)
{
	if (BodySetup == InBodySetup)
	{
		BodySetup = nullptr;
	}
}

UBodySetup* UStaticMesh::GetBodySetup()
{
    if (BodySetup)
        return BodySetup;

    BodySetup = NewObject<UBodySetup>(this);

    // RenderBounds는 RebuildRenderData 때만 채워지므로 원본 AABB를 쓴다
    const FBox& Bounds = MeshData.AABB;
    // 평면 메시(바닥 Plane 등)는 두께가 0이라 PhysX가 박스를 만들지 못한다. 최소 두께를 준다
    constexpr float MinThickness = 0.01f;

    FBoxElem& Box = BodySetup->AggGeom.BoxElems.Emplace_GetRef();
    Box.Center = (Bounds.Min + Bounds.Max) * 0.5f;
    Box.X = std::max(Bounds.Max.X - Bounds.Min.X, MinThickness);
    Box.Y = std::max(Bounds.Max.Y - Bounds.Min.Y, MinThickness);
    Box.Z = std::max(Bounds.Max.Z - Bounds.Min.Z, MinThickness);

    return BodySetup;
}

bool UStaticMesh::EnsureTriangleMesh()
{
    UBodySetup* Setup = GetBodySetup();
    if (!Setup)
    {
        return false;
    }
    if (Setup->TriangleMesh)
    {
        return true;
    }
    if (MeshData.Vertices.IsEmpty() || MeshData.Indices.Num() < 3)
    {
        return false;
    }

    physx::PxPhysics* Physics = FPhysicsCore::IsInitialized() ? FPhysicsCore::GetPhysics() : nullptr;
    if (!Physics)
    {
        return false;
    }

    physx::PxTriangleMeshDesc Desc;
    Desc.points.count = static_cast<physx::PxU32>(MeshData.Vertices.Num());
    Desc.points.stride = sizeof(FVertex);
    Desc.points.data = MeshData.Vertices.GetData();
    Desc.triangles.count = static_cast<physx::PxU32>(MeshData.Indices.Num() / 3);
    Desc.triangles.stride = 3 * sizeof(uint32);
    Desc.triangles.data = MeshData.Indices.GetData();

    physx::PxCookingParams CookingParams(Physics->getTolerancesScale());
    CookingParams.suppressTriangleMeshRemapTable = true;

    physx::PxTriangleMeshCookingResult::Enum Result = physx::PxTriangleMeshCookingResult::eSUCCESS;
    Setup->TriangleMesh = PxCreateTriangleMesh(
        CookingParams,
        Desc,
        Physics->getPhysicsInsertionCallback(),
        &Result);

    if (!Setup->TriangleMesh)
    {
        KRS_LOG(Error, "[PhysX] Failed to cook TriangleMesh for '{}' (result={})", GetPath(), static_cast<int32>(Result));
        return false;
    }

    KRS_LOG(Info, "[PhysX] Cooked TriangleMesh for '{}' ({} verts, {} tris)",
        GetPath(), MeshData.Vertices.Num(), MeshData.Indices.Num() / 3);
    return true;
}

UMaterialInterface* UStaticMesh::GetMaterial(uint32 SlotIndex) const
{
	if (SlotIndex < Materials.size() && Materials[SlotIndex]) {
		return Materials[SlotIndex];
	}
	return UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
}

size_t UStaticMesh::FMaterialSlotsHash::operator()(const std::vector<UMaterialInterface*>& Slots) const
{
    size_t Hash = 0;
    for (const UMaterialInterface* Material : Slots)
        Hash ^= std::hash<const UMaterialInterface*>{}(Material) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
    return Hash;
}

void UStaticMesh::RebuildRenderData()
{
	RenderBounds = MeshData.AABB;
	for (const FStaticMeshLODResource& LOD : AdditionalLODs)
	{
		for (int Axis = 0; Axis < 3; ++Axis)
		{
			RenderBounds.Min[Axis] = std::min(RenderBounds.Min[Axis], LOD.Data.AABB.Min[Axis]);
			RenderBounds.Max[Axis] = std::max(RenderBounds.Max[Axis], LOD.Data.AABB.Max[Axis]);
		}
	}
	LocalLODSphere.Center = (RenderBounds.Min + RenderBounds.Max) * 0.5f;
	double RadiusSquared = 0.0;
	for (uint32 LOD = 0; LOD < GetLODCount(); ++LOD)
	{
		for (const FVertex& Vertex : GetLODMeshData(LOD).Vertices)
		{
			const double X = static_cast<double>(Vertex.Position.X) - LocalLODSphere.Center.X;
			const double Y = static_cast<double>(Vertex.Position.Y) - LocalLODSphere.Center.Y;
			const double Z = static_cast<double>(Vertex.Position.Z) - LocalLODSphere.Center.Z;
			RadiusSquared = std::max(RadiusSquared, X * X + Y * Y + Z * Z);
		}
	}
	LocalLODSphere.RadiusSquared = std::nextafter(
		static_cast<float>(RadiusSquared * (1.0 + 2e-5)), std::numeric_limits<float>::infinity());
	RenderStates.clear();
	for (FPrimitiveSceneProxy* Proxy : RenderProxies)
		Proxy->OnMeshRenderDataChanged();
}

TSharedPtr<const FMeshRenderState> UStaticMesh::GetRenderState(const std::vector<UMaterialInterface*>& Materials)
{
	std::weak_ptr<const FMeshRenderState>& WeakState = RenderStates[Materials];
	if (TSharedPtr<const FMeshRenderState> State = WeakState.lock()) return State;
	TSharedPtr<FMeshRenderState> State = MakeShared<FMeshRenderState>();
	State->LODCount = static_cast<uint8>(std::min(GetLODCount(), 4u));
	for (uint32 I = 0; I < 3; ++I)
		State->LODThresholdSq[I] = ScreenThresholds[I] * ScreenThresholds[I];
	for (uint32 LOD = 0; LOD < State->LODCount; ++LOD)
	{
		FCachedMeshLOD& Range = State->LODs[LOD];
		Range.FirstSection = static_cast<uint32>(State->Sections.Num());
		for (const FStaticMeshSection& Section : GetLODMeshData(LOD).Sections)
		{
			UMaterialInterface* Material = Section.MaterialSlotIndex < Materials.size()
				? Materials[Section.MaterialSlotIndex] : GetMaterial(Section.MaterialSlotIndex);
			if (Material) State->Sections.Add({ Material, Section.StartIndex, Section.IndexCount });
		}
		Range.NumSections = static_cast<uint32>(State->Sections.Num()) - Range.FirstSection;
	}
	WeakState = State;
	return State;
}

void UStaticMesh::RegisterProxy(FPrimitiveSceneProxy* Proxy)
{
    RenderProxies.Add(Proxy);
}

void UStaticMesh::UnregisterProxy(FPrimitiveSceneProxy* Proxy)
{
    const auto It = std::find(RenderProxies.begin(), RenderProxies.end(), Proxy);
    if (It != RenderProxies.end())
        RenderProxies.RemoveAtSwap(static_cast<uint32>(It - RenderProxies.begin()));
}
