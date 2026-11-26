#include "EnginePCH.h"
#include "PrimitiveSceneProxy.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BillboardComponent.h"
#include "Render/Mesh.h"
#include "Engine/Scene.h"

namespace
{
	const FCachedMeshLOD GEmptyMeshLOD{};
}

FPrimitiveSceneProxy::FPrimitiveSceneProxy(UPrimitiveComponent* InComponent)
	: Component(InComponent), bCastsShadow(InComponent->bCastShadow),
	bReceiveShadow(InComponent->bReceiveShadow), bReceivesDecals(InComponent->bReceivesDecals)
{
}

FPrimitiveSceneProxy::~FPrimitiveSceneProxy()
{
	if (Mesh) Mesh->UnregisterProxy(this);
}

void FPrimitiveSceneProxy::OnMeshRenderDataChanged()
{
	if (Mesh)
	{
		LocalLODSphere = Mesh->GetLocalLODSphere();
		UpdateLODSphere();
	}
	if (Scene)
	{
		Scene->MarkRenderStateDirty(this);
		Scene->MarkDirty(this);
	}
}

void FPrimitiveSceneProxy::OnMeshDestroyed()
{
	Mesh = nullptr;
	RenderState.reset();
}

void FPrimitiveSceneProxy::UpdateCommonFlags()
{
	bVisible = Component->IsShown(false);
	bHiddenInGame = !Component->IsShown(true);
	bCastsShadow = Component->bCastShadow;
	bReceiveShadow = Component->bReceiveShadow;
	bReceivesDecals = Component->bReceivesDecals;
}

const FCachedMeshLOD& FPrimitiveSceneProxy::GetLOD(uint32 LODIndex) const
{
	if (!RenderState || LODIndex >= RenderState->LODCount)
	{
		return GEmptyMeshLOD;
	}
	return RenderState->LODs[LODIndex];
}

void FPrimitiveSceneProxy::UpdateLODSphere()
{
	if (Mesh)
	{
		LODSphere = LocalLODSphere.Transform(LocalToWorld);
	}
	else
	{
		LODSphere.Center = Bounds.Center;
		LODSphere.RadiusSquared = Bounds.Extent.Dot(Bounds.Extent);
	}
}

void FPrimitiveSceneProxy::UpdateTransform()
{
	LocalToWorld = Component->GetWorldMatrix();
	WorldToLocal = LocalToWorld.Inverse();
	Bounds = MakeWorldBounds(Mesh ? Mesh->GetRenderBounds().GetWorldAABB(LocalToWorld) : Component->CalcBounds());
	UpdateLODSphere();
}

void FPrimitiveSceneProxy::UpdateRenderState()
{
	UStaticMesh* PreviousMesh = Mesh;
	Mesh = nullptr;
	RenderState.reset();
	UpdateCommonFlags();
	UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component);
	if (StaticMeshComponent) Mesh = StaticMeshComponent->GetStaticMesh();
	if (Mesh != PreviousMesh)
	{
		if (PreviousMesh) PreviousMesh->UnregisterProxy(this);
		if (Mesh) Mesh->RegisterProxy(this);
		if (Scene) Scene->MarkDirty(this);
	}
	if (!Mesh)
	{
		LocalLODSphere = FLODSphere{};
		UpdateLODSphere();
		return;
	}
	LocalLODSphere = Mesh->GetLocalLODSphere();
	UpdateLODSphere();
	std::vector<UMaterialInterface*> Materials;
	Materials.reserve(Mesh->MeshData.MaterialSlots.Num());
	for (uint32 Slot = 0; Slot < static_cast<uint32>(Mesh->MeshData.MaterialSlots.Num()); ++Slot)
		Materials.push_back(StaticMeshComponent->GetMaterial(Slot));
	RenderState = Mesh->GetRenderState(Materials);
}

FStaticMeshSceneProxy::FStaticMeshSceneProxy(UPrimitiveComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
}

void FStaticMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) const
{
	UStaticMesh* StaticMesh = GetMesh();
	if (!PDI || !StaticMesh || !RenderState)
		return;

	for (uint32 LOD = 0; LOD < RenderState->LODCount; ++LOD)
	{
		const FVertexFactory* VF = StaticMesh->GetLODVertexFactory(LOD);
		FIndexBuffer* IB = StaticMesh->GetLODIndexBuffer(LOD);
		if (!VF || !VF->IsValid() || !IB)
		{
			continue;
		}
		const FCachedMeshLOD& CachedLOD = RenderState->LODs[LOD];
		for (uint32 i = 0; i < CachedLOD.NumSections; ++i)
		{
			const FCachedMeshSection& Section = RenderState->Sections[CachedLOD.FirstSection + i];
			if (!Section.Material || Section.IndexCount == 0)
			{
				continue;
			}

			FStaticMeshBatch Batch;
			Batch.PrimitiveSceneProxy = this;
			Batch.PrimitiveIndex = GetPrimitiveId();
			Batch.VertexFactory = VF;
			Batch.Mesh = StaticMesh;
			Batch.Material = Section.Material;
			Batch.MeshSortID = StaticMesh->SortID;
			Batch.LODIndex = static_cast<uint8>(LOD);
			Batch.bCastShadow = bCastsShadow;
			Batch.bUsesGPUScene = true;

			FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
			Element.Proxy = this;
			Element.IndexBuffer = IB;
			Element.FirstIndex = Section.StartIndex;
			Element.IndexCount = Section.IndexCount;
			Element.PrimitiveId = GetPrimitiveId();
			Element.bReceiveShadow = bReceiveShadow;
			Element.bReceiveDecal = bReceivesDecals;

			PDI->DrawMesh(Batch);
		}
	}
}

FBillboardSceneProxy::FBillboardSceneProxy(UPrimitiveComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
	bCastsShadow = false;
	bReceivesDecals = false;
}

void FBillboardSceneProxy::UpdateRenderState()
{
	RenderState.reset();
	UpdateCommonFlags();
	bCastsShadow = false;
	bReceivesDecals = false;
}

void FBillboardSceneProxy::GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex) const
{
	UBillboardComponent* Billboard = static_cast<UBillboardComponent*>(GetComponent());
	if (!Billboard)
		return;

	UStaticMesh* QuadMesh = Billboard->GetQuadMesh();
	UMaterialInterface* Material = Billboard->GetMaterial(0);
	if (!QuadMesh || !Material)
		return;

	const FVertexFactory* VF = QuadMesh->GetVertexFactory();
	if (!VF || !VF->IsValid())
		return;

	const FVector WorldPos = Billboard->GetWorldLocation();
	const FVector WorldScale = Billboard->GetWorldScale3D();
	const float Width = Billboard->IsEditorIcon() ? Billboard->GetIconSize() : WorldScale.Y;
	const float Height = Billboard->IsEditorIcon() ? Billboard->GetIconSize() : WorldScale.Z;
	const FMatrix BillboardWorldMatrix = View.BuildBillboardMatrix(WorldPos, Width, Height);
	const FBillboardConstants& IconParams = Billboard->GetCurrentIconParams();
	const FVector Delta = WorldPos - View.CameraPosition;

	FMeshBatch& Batch = Collector.AddMeshDefaulted();
	Batch.VertexFactory = VF;
	Batch.Mesh = QuadMesh;
	Batch.Material = Material;
	Batch.MaterialParamData = &IconParams;
	Batch.MaterialParamDataSize = sizeof(FBillboardConstants);
	Batch.CameraDistanceSq = Delta.Dot(Delta);
	Batch.MeshSortID = QuadMesh->SortID;
	Batch.bCastShadow = false;
	Batch.bUsesGPUScene = false;

	FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
	Element.Proxy = this;
	Element.IndexBuffer = VF->GetIndexBuffer();
	Element.WorldMatrix = Collector.StoreWorldMatrix(BillboardWorldMatrix);
	Element.FirstIndex = 0;
	Element.IndexCount = VF->GetIndexBuffer()->GetIndexCount();
	Element.bReceiveShadow = false;
	Element.bReceiveDecal = false;
}
