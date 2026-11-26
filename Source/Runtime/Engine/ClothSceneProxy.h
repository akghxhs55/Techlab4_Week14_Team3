#pragma once

#include "Engine/PrimitiveSceneProxy.h"

class FClothSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FClothSceneProxy(UPrimitiveComponent* InComponent);

	void UpdateRenderState() override;
	bool HasStaticRelevance() const override { return false; }
	bool HasDynamicRelevance() const override { return true; }
	void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const override;
};
