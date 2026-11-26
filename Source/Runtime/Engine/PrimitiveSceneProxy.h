#pragma once

#include "Math/Frustum.h"
#include "Render/MeshRenderData.h"
#include "Render/MeshBatch.h"

class FScene;
class UPrimitiveComponent;
class UStaticMesh;
class UMaterial;
struct FViewContext;

class FPrimitiveSceneProxy
{
public:
	explicit FPrimitiveSceneProxy(UPrimitiveComponent* InComponent);

	virtual ~FPrimitiveSceneProxy();
	virtual void UpdateRenderState();

	virtual bool HasStaticRelevance() const { return false; }
	virtual bool HasDynamicRelevance() const { return false; }
	virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) const {}
	virtual void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const {}
	virtual int32 GetDebugWeightBone() const { return -1; }

	bool IsSkinned() const { return bSkinned; }
	bool IsParticleSystem() const { return bParticleSystem; }

	void UpdateTransform();
	void OnMeshRenderDataChanged();
	void OnMeshDestroyed();

	UPrimitiveComponent* GetComponent() const { return Component; }
	const FAABB& GetBounds() const { return Bounds; }
	const FMatrix& GetLocalToWorld() const { return LocalToWorld; }
	const FMatrix& GetWorldToLocal() const { return WorldToLocal; }
	const FLODSphere& GetLODSphere() const { return LODSphere; }
	UStaticMesh* GetMesh() const { return Mesh; }
	bool IsVisible() const { return bVisible; }
	bool IsShown(bool bGameView) const { return bVisible && !(bGameView && bHiddenInGame); }
	const FMeshRenderState* GetRenderState() const { return RenderState.get(); }
	uint32 GetLODCount() const { return RenderState ? RenderState->LODCount : 0u; }
	const FCachedMeshLOD& GetLOD(uint32 LODIndex) const;
	const FCachedMeshSection& GetSection(uint32 Index) const { return RenderState->Sections[Index]; }
	FScene* GetScene() const { return Scene; }
	int32 GetPackedIndex() const { return PackedIndex; }
	uint32 GetPrimitiveId() const { return PrimitiveId; }
	void SetPrimitiveId(uint32 InPrimitiveId) { PrimitiveId = InPrimitiveId; }

	void SetCastShadow(bool NewValue);
	void SetReceiveShadow(bool NewValue);

	bool bCastsShadow = true;
	bool bReceiveShadow = true;
	bool bReceivesDecals = true;
protected:
	bool bSkinned = false;
	bool bParticleSystem = false;
	TSharedPtr<const FMeshRenderState> RenderState;
	void UpdateCommonFlags();
private:
	friend class FScene;
	FScene* Scene = nullptr;
	UPrimitiveComponent* Component = nullptr;
	UStaticMesh* Mesh = nullptr;
	FMatrix LocalToWorld;
	FMatrix WorldToLocal;
	FAABB Bounds;
	FLODSphere LocalLODSphere{};
	FLODSphere LODSphere{};
	void UpdateLODSphere();
	int32 PackedIndex = INDEX_NONE;
	uint32 PrimitiveId = InvalidPrimitiveId;
	bool bQueuedForUpdate = false;
	bool bRenderStateQueued = false;
	bool bPendingStaticCacheRebuild = false;
	bool bVisible = true;
	bool bHiddenInGame = false;
};

class FStaticMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FStaticMeshSceneProxy(UPrimitiveComponent* InComponent);

	bool HasStaticRelevance() const override { return true; }
	bool HasDynamicRelevance() const override { return false; }
	void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) const override;
};

class FBillboardSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FBillboardSceneProxy(UPrimitiveComponent* InComponent);

	void UpdateRenderState() override;
	bool HasStaticRelevance() const override { return false; }
	bool HasDynamicRelevance() const override { return true; }
	void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const override;
};
