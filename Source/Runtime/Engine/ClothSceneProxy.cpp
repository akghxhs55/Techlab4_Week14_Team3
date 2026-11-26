#include "EnginePCH.h"
#include "Engine/ClothSceneProxy.h"
#include "Components/ClothComponent.h"

FClothSceneProxy::FClothSceneProxy(UPrimitiveComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
{
	UpdateCommonFlags();
}

void FClothSceneProxy::UpdateRenderState()
{
	RenderState.reset();
	UpdateCommonFlags();
}

void FClothSceneProxy::GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex) const
{
	UClothComponent* ClothComp = static_cast<UClothComponent*>(GetComponent());
	if (!ClothComp)
	{
		return;
	}

	const FVertexFactory* VF = ClothComp->GetVertexFactory();
	UMaterialInterface* Material = ClothComp->GetMaterial(0);
	if (!VF || !VF->IsValid() || !Material)
	{
		return;
	}

	FIndexBuffer* IB = VF->GetIndexBuffer();
	if (!IB || IB->GetIndexCount() == 0)
	{
		return;
	}

	const FVector Delta = GetBounds().Center - View.CameraPosition;
	const float DistanceSq = Delta.Dot(Delta);

	FMeshBatch& Batch = Collector.AddMeshDefaulted();
	Batch.VertexFactory = VF;
	Batch.Material = Material;
	Batch.CameraDistanceSq = DistanceSq;
	Batch.LODIndex = static_cast<uint8>(LODIndex);
	Batch.bCastShadow = bCastsShadow;
	Batch.bUsesGPUScene = true;

	FMeshBatchElement& Element = Batch.Elements.AddDefaulted_GetRef();
	Element.Proxy = this;
	Element.IndexBuffer = IB;
	Element.FirstIndex = 0;
	Element.IndexCount = IB->GetIndexCount();
	Element.PrimitiveId = GetPrimitiveId();
	Element.bReceiveShadow = bReceiveShadow;
	Element.bReceiveDecal = bReceivesDecals;
}
