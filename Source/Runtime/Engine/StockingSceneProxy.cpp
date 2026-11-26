#include "EnginePCH.h"
#include "Engine/StockingSceneProxy.h"
#include "Components/StockingComponent.h"
#include "Render/SkeletalMeshObject.h"
#include <algorithm>

FStockingSceneProxy::FStockingSceneProxy(UStockingComponent* InComponent)
	: FSkeletalMeshSceneProxy(InComponent)
{
	bSkinned = true;
}

void FStockingSceneProxy::UpdateRenderState()
{
	RenderState.reset();
	UpdateCommonFlags();

	UStockingComponent* Stocking = static_cast<UStockingComponent*>(GetComponent());
	USkeletalMesh* Asset = Stocking ? Stocking->GetSkeletalMesh() : nullptr;
	if (!Asset)
	{
		return;
	}

	UMaterialInterface* EffectiveMat = Stocking->GetEffectiveStockingMaterial();
	TArray<UMaterialInterface*> Materials;
	const int32 SlotCount = std::max(1, Stocking->GetNumMaterials());
	Materials.Reserve(SlotCount);
	for (int32 Slot = 0; Slot < SlotCount; ++Slot)
	{
		UMaterialInterface* SlotMat = Stocking->GetOverrideMaterial(Slot);
		const bool bValidSlotMat = SlotMat && SlotMat->GetBaseMaterial() != nullptr;
		Materials.Add(bValidSlotMat ? SlotMat : EffectiveMat);
	}
	RenderState = Asset->GetRenderState(Materials);
}

void FStockingSceneProxy::GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex) const
{
	if (!View.bShowSkeletalMesh)
	{
		return;
	}

	UStockingComponent* Stocking = static_cast<UStockingComponent*>(GetComponent());
	FSkeletalMeshObject* Object = Stocking ? Stocking->GetMeshObject() : nullptr;
	USkeletalMesh* Asset = Stocking ? Stocking->GetSkeletalMesh() : nullptr;
	if (!Object || !Asset || !RenderState)
	{
		return;
	}

	const uint32 AvailableLODs = static_cast<uint32>(Asset->GetRenderData().LODRenderData.Num());
	if (AvailableLODs == 0)
	{
		return;
	}

	const uint32 SafeLOD = std::min(LODIndex, AvailableLODs - 1u);
	const FVertexFactory* VertexFactory = Object->GetVertexFactory(SafeLOD);
	if (!VertexFactory || !VertexFactory->IsValid())
	{
		return;
	}

	const FVector Delta = GetBounds().Center - View.CameraPosition;
	const float DistanceSq = Delta.Dot(Delta);
	const int32 DebugWeightBone = Stocking->GetDebugWeightBone();

	for (uint32 i = 0; i < RenderState->Sections.Num(); ++i)
	{
		const FCachedMeshSection& Section = GetSection(i);
		if (!Section.Material || Section.IndexCount == 0)
		{
			continue;
		}

		FMeshBatch& Batch = Collector.AddMeshDefaulted();
		Batch.VertexFactory = VertexFactory;
		Batch.Material = Section.Material;
		Batch.CameraDistanceSq = DistanceSq;
		Batch.LODIndex = static_cast<uint8>(SafeLOD);
		Batch.bCastShadow = bCastsShadow;
		Batch.bUsesGPUScene = true;
		Batch.bTwoSided = true;

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
