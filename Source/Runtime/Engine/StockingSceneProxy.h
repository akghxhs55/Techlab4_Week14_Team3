#pragma once

#include "Engine/SkeletalMeshSceneProxy.h"

class UStockingComponent;

class FStockingSceneProxy : public FSkeletalMeshSceneProxy
{
public:
	explicit FStockingSceneProxy(UStockingComponent* InComponent);

	void UpdateRenderState() override;
	bool HasDynamicRelevance() const override { return true; }
	void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const override;
};
