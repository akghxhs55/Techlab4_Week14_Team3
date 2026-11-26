#include "EnginePCH.h"
#include "SkeletalMeshSceneProxy.h"

#include "Components/SkinnedMeshComponent.h"
#include "Render/SkeletalMeshObject.h"

FSkeletalMeshSceneProxy::FSkeletalMeshSceneProxy(USkinnedMeshComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
	bSkinned = true;
}

void FSkeletalMeshSceneProxy::UpdateRenderState()
{
	RenderState.reset();
	UpdateCommonFlags();

	USkinnedMeshComponent* Skinned = static_cast<USkinnedMeshComponent*>(GetComponent());
	USkeletalMesh* Asset = Skinned->GetSkeletalMesh();
	if (!Asset) return;

	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(Skinned->GetNumMaterials());
	for (int32 Slot = 0; Slot < Skinned->GetNumMaterials(); ++Slot)
		Materials.Add(Skinned->GetMaterial(Slot));
	RenderState = Asset->GetRenderState(Materials);
}

int32 FSkeletalMeshSceneProxy::GetDebugWeightBone() const
{
	USkinnedMeshComponent* Skinned = static_cast<USkinnedMeshComponent*>(GetComponent());
	return Skinned ? Skinned->GetDebugWeightBone() : -1;
}

void FSkeletalMeshSceneProxy::GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex) const
{
	if (!View.bShowSkeletalMesh)
		return;

	USkinnedMeshComponent* Skinned = static_cast<USkinnedMeshComponent*>(GetComponent());
	FSkeletalMeshObject* Object = Skinned ? Skinned->GetMeshObject() : nullptr;
	if (!Object || !RenderState) return;

	const FVertexFactory* VertexFactory = Object->GetVertexFactory(0);
	if (!VertexFactory || !VertexFactory->IsValid()) return;

	const FVector Delta = GetBounds().Center - View.CameraPosition;
	const float DistanceSq = Delta.Dot(Delta);
	const int32 DebugWeightBone = Skinned->GetDebugWeightBone();

	for (uint32 i = 0; i < RenderState->Sections.Num(); ++i)
	{
		const FCachedMeshSection& Section = GetSection(i);
		if (!Section.Material || Section.IndexCount == 0)
			continue;

		FMeshBatch& Batch = Collector.AddMeshDefaulted();
		Batch.VertexFactory = VertexFactory;
		Batch.Material = Section.Material;
		Batch.CameraDistanceSq = DistanceSq;
		Batch.LODIndex = static_cast<uint8>(LODIndex);
		Batch.bCastShadow = bCastsShadow;
		Batch.bUsesGPUScene = true;

		FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
		Element.Proxy = this;
		Element.IndexBuffer = VertexFactory->GetIndexBuffer();
		Element.FirstIndex = Section.StartIndex;
		Element.IndexCount = Section.IndexCount;
		Element.PrimitiveId = GetPrimitiveId();
		Element.bReceiveShadow = bReceiveShadow;
		Element.bReceiveDecal = bReceivesDecals;
		Element.DebugWeightBone = DebugWeightBone;
	}
}
