#pragma once

#include "PrimitiveSceneProxy.h"

class USkinnedMeshComponent;

class FSkeletalMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FSkeletalMeshSceneProxy(USkinnedMeshComponent* InComponent);

	void UpdateRenderState() override;
	bool HasDynamicRelevance() const override { return true; }

	void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const override;
	int32 GetDebugWeightBone() const override;
private:
	
};