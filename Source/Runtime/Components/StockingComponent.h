#pragma once

#include "Components/SkinnedMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Render/StockingRegion.h"
#include "Render/StockingMaterial.h"
#include "Render/MaterialInterface.h"
#include "ClothBridge.h"
#include "StockingComponent.generated.h"

class UMaterialInstanceDynamic;
class UTexture2D;

UCLASS()
class UStockingComponent : public USkinnedMeshComponent
{
	GENERATED_BODY()

public:
	UStockingComponent();
	virtual ~UStockingComponent() override;

	virtual void TickComponent(float DeltaTime) override;
	virtual void Serialize(FStructuredArchive::FRecord Record) override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBox CalcLocalBounds() const override;
	virtual void PostEditChangeProperty(const FProperty& Property) override;

	// 입력 연결 및 스타킹 메시 재생성
	void SetSourceSkeletalMeshComponent(USkeletalMeshComponent* InSourceComponent);
	USkeletalMeshComponent* GetSourceSkeletalMeshComponent() const { return SourceSkeletalMeshComponent; }

	void SetStockingRegion(UStockingRegion* InRegion);
	UStockingRegion* GetStockingRegion() const { return StockingRegion; }

	void SetStockingMaterial(UMaterialInterface* InMaterial);
	UMaterialInterface* GetStockingMaterial() const { return StockingMaterial; }
	UMaterialInterface* GetEffectiveStockingMaterial();
	UMaterialInstanceDynamic* GetDefaultStockingMaterialInstance();

	bool RebuildStockingMesh(FString& OutError);
	bool ValidateAgainstSourceMesh(FString& OutError) const;
	void CalibrateParametersForScale(float InScale);
	float GetReferenceScale() const { return ReferenceScale; }
	void ResetClothSimulation();
	void ApplyNvClothParams();
	static bool ValidateSkeletonCompatibility(const USkeletalMesh& SourceMesh, const USkeletalMesh& StockingMesh, FString& OutError);

	// 마우스 집어 당기기 제어
	void BeginPull(uint32 TriangleIndex, const FVector& Barycentric, const FVector& TargetWorldPosition);
	void UpdatePull(const FVector& TargetWorldPosition);
	void EndPull();
	void ResetPullImmediately();

	// 찢어짐 인터랙션 및 원클릭 복구 제어
	void ApplyTearAtSurfaceHit(uint32 TriangleIndex, const FVector& Barycentric, const FVector& DragDirLocal, float Intensity = 1.0f);
	bool ApplyWorldImpactTear(const FVector& WorldLocation, const FVector& WorldImpulse, const FVector& WorldSlideVelocity, int32 HitBoneIndex = -1);
	void RepairAllTears();
	void SyncTearMaskBinding();
	int32 GetActiveTearCount() const { return TearNodes.Num(); }
	bool HasActiveTears() const { return !TearNodes.IsEmpty(); }
	UTexture2D* GetTearMaskTexture() const { return TearMaskTexture; }

	bool IsPulling() const { return bIsPulling; }
	bool HasActiveDeformation() const;
	FVector GetAnchorWorldPosition() const;
	FVector GetPullTargetWorldPosition() const { return PullTargetWorld; }
	uint32 GetGrabbedTriangleIndex() const { return GrabbedTriangleIndex; }
	float GetMaxCurrentStretchRatio() const { return MaxCurrentStretchRatio; }
	const FString& GetLastErrorMessage() const { return LastErrorMessage; }

	UPROPERTY(EditAnywhere)
	USkeletalMeshComponent* SourceSkeletalMeshComponent = nullptr;

	UPROPERTY(EditAnywhere)
	UStockingRegion* StockingRegion = nullptr;

	UPROPERTY(EditAnywhere)
	UMaterialInterface* StockingMaterial = nullptr;

	UPROPERTY(EditAnywhere)
	bool bSyncTransformWithSource = true;

	UPROPERTY(EditAnywhere)
	float SurfaceOffset = 0.0025f;

	UPROPERTY(EditAnywhere)
	float PullRadius = 0.11f;

	UPROPERTY(EditAnywhere)
	float MaxPullDistance = 0.075f;

	UPROPERTY(EditAnywhere)
	float PullFalloffExponent = 2.15f;

	UPROPERTY(EditAnywhere)
	float WrinkleAmplitude = 0.0085f;

	UPROPERTY(EditAnywhere)
	float WrinkleFrequency = 7.0f;

	UPROPERTY(EditAnywhere)
	float SpringStiffness = 160.0f;

	UPROPERTY(EditAnywhere)
	float SpringDamping = 14.0f;

	UPROPERTY(EditAnywhere)
	float RestoreSpeed = 8.0f;

	UPROPERTY(EditAnywhere)
	float BoundsPadding = 0.04f;

	// 관절 주름 시뮬레이션용 NvCloth 파라미터
	UPROPERTY(EditAnywhere)
	bool bUseNvClothSimulation = true;

	UPROPERTY(EditAnywhere)
	float ClothJointWrinkleSlack = 0.0035f;

	UPROPERTY(EditAnywhere)
	float ClothBackstopRadius = 0.025f;

	UPROPERTY(EditAnywhere)
	float ClothStretchStiffness = 0.90f;

	UPROPERTY(EditAnywhere)
	float ClothBendStiffness = 0.10f;

	UPROPERTY(EditAnywhere)
	float ClothShearStiffness = 0.65f;

	UPROPERTY(EditAnywhere)
	float ClothCompressionLimit = 1.0f;

	UPROPERTY(EditAnywhere)
	float ClothStretchLimit = 1.18f;

	UPROPERTY(EditAnywhere)
	float ClothMotionStiffness = 0.85f;

	UPROPERTY(EditAnywhere)
	float ClothDamping = 0.45f;

	UPROPERTY(EditAnywhere)
	float ClothSelfCollisionDistance = 0.0f;

	UPROPERTY(EditAnywhere)
	float ClothSelfCollisionStiffness = 0.45f;

	// 스타킹 찢어짐 및 올 풀림 시뮬레이션 파라미터
	UPROPERTY(EditAnywhere)
	bool bEnableTearing = true;

	UPROPERTY(EditAnywhere)
	float TearStretchThreshold = 1.24f;

	UPROPERTY(EditAnywhere)
	float TearSpreadSpeed = 1.0f;

	UPROPERTY(EditAnywhere)
	float TearRunLength = 0.48f;

	UPROPERTY(EditAnywhere)
	float TearBrushRadiusUV = 0.014f;

	// 래그돌 충돌 충격량 기반 찢어짐 파라미터
	UPROPERTY(EditAnywhere)
	bool bEnableRagdollImpactTearing = true;

	UPROPERTY(EditAnywhere)
	float ImpactTearImpulseThreshold = 1.5f;

	UPROPERTY(EditAnywhere)
	float ImpactScrapeSensitivity = 0.35f;

	UPROPERTY(EditAnywhere)
	float ImpactTearCooldownDuration = 0.14f;

protected:
	virtual void RefreshBoneTransforms() override;
	virtual void OnMeshObjectCreated() override;

private:
	struct FNeighborEdge
	{
		uint32 NeighborVertex = 0;
		float RestLength = 1.0f;
	};

	struct FStockingTearNode
	{
		FVector2 CenterUV = FVector2(0.5f, 0.5f);
		float RadiusU = 0.009f;
		float RadiusV = 0.022f;
		float RunUpV = 0.06f;
		float RunDownV = 0.06f;
		float RunWidthU = 0.011f;
		float OpenStrength = 0.65f;
		float TiltAngle = 0.0f;
		FVector2 PullBiasUV = FVector2(0.0f, 0.0f);
		float Seed = 1.0f;
		float TileMinU = 0.0f;
		float TileWidthU = 1.0f;
		bool bSpawnedUpperSubTear = false;
		bool bSpawnedLowerSubTear = false;
	};

	void BuildRestAdjacency(const USkeletalMesh& Mesh);
	void BuildCoveredBoneMask(const USkeletalMesh& Mesh);
	bool IsCoveredImpactBone(int32 BoneIndex) const;
	void FinalizeStockingSetupAfterLoad();
	void BindSourceImpactListener();
	void UnbindSourceImpactListener();
	void HandleSourcePhysicsImpact(const FSkeletalPhysicsImpact& Impact);
	bool FindClosestTriangleToLocalPoint(
		const FVector& LocalPoint,
		float MaxSearchDist,
		uint32& OutTriangleIndex,
		FVector& OutBarycentric) const;
	void ComputeComponentUVTiles(const USkeletalMesh& Mesh);
	void EnsureTearMaskTexture();
	void RebuildTearMaskTexture();
	bool ComputeTriangleHitUV(
		uint32 TriangleIndex,
		const FVector& Barycentric,
		FVector2& OutUV,
		float& OutTileMinU,
		float& OutTileWidthU) const;
	bool ComputeTriangleUVPullVector(
		uint32 TriangleIndex,
		const FVector& LocalPullVector,
		const TArray<FVector>* OptionalWeldedPositions,
		FVector2& OutPullUVDir,
		float& OutTangentialRatio) const;
	float ComputeWrappedDeltaU(float U, float CenterU, float TileMinU, float TileWidthU) const;
	int32 FindNearestTearIndex(const FVector2& UV, float TileMinU, float TileWidthU, float MaxNormalizedDist) const;
	void AddOrExpandTearAtUV(
		const FVector2& UV,
		float TileMinU,
		float TileWidthU,
		const FVector2& PullDirUV,
		float TangentialRatio,
		float SpreadAmount,
		bool bAllowNewNode);
	void UpdatePullTearing(
		const FSkeletalMeshLODRenderData& LOD,
		const TArray<FVector>& DeformedWeldedPos,
		const TArray<float>& WeldedStretch);
	void RebuildNvClothFromSimMesh();
	void DestroyNvClothResources();
	bool StepNvClothJointWrinkles(
		const TArray<FVector>& SkinnedPos,
		const TArray<FVector>& SkinnedNormal,
		const FVector& AnchorNormal,
		TArray<FVector>& OutSimPos,
		TArray<float>& OutCompression);
	void ComputeGrabGeodesicDistances();
	bool ShouldUsePostSkinDeform() const;
	bool HasPoseOrPullChanged(const TArray<FMatrix>& SourceTransforms, FSkeletalMeshObject* MeshObject) const;
	void RecordSyncedPoseState(const TArray<FMatrix>& SourceTransforms, FSkeletalMeshObject* MeshObject);
	void BindDeformationCallback();
	void StepPullState(float DeltaTime);
	void ApplyStockingDeformation(TArray<FVertex>& InOutVertices, FBox& InOutBounds);
	void RecomputeDeformedNormals(TArray<FVertex>& InOutVertices) const;

	USkeletalMesh* GeneratedStockingMesh = nullptr;
	UMaterialInstanceDynamic* DefaultStockingMaterial = nullptr;

	TArray<uint32> WeldedVertexIds;
	uint32 WeldedVertexCount = 0;
	TArray<FVector> SimBindPositions;
	TArray<uint32> SimTriangleIndices;
	TArray<float> WeldedEdgeMasks;
	TArray<FVector2> WeldedPrimaryUV;
	TArray<float> WeldedTileMinU;
	TArray<float> WeldedTileWidthU;
	TArray<float> WeldedTearFactor;
	TArray<TArray<FNeighborEdge>> WeldedAdjacency;
	TArray<float> GrabGeodesicDistances;
	FString LastErrorMessage;

	// 동적 찢김 마스크 텍스처와 활성 찢김 노드 상태
	UTexture2D* TearMaskTexture = nullptr;
	TArray<uint8> TearMaskPixels;
	TArray<FStockingTearNode> TearNodes;
	bool bTearMaskDirty = false;
	float TearMaskUploadCooldown = 0.0f;
	float PullTearCooldown = 0.0f;
	float ImpactTearCooldownTimer = 0.0f;
	USkeletalMeshComponent* BoundImpactSource = nullptr;
	TArray<uint8> CoveredBoneMask;

	// 용접 시뮬레이션 메쉬용 NvCloth 핸들 및 작업 버퍼
	FClothSolverHandle ClothSolver = nullptr;
	FClothFabricHandle ClothFabric = nullptr;
	FClothInstanceHandle ClothInstance = nullptr;
	TArray<FClothBridgeVec4> ClothParticlesBuffer;
	TArray<FClothBridgeVec4> ClothPrevParticlesBuffer;
	TArray<FClothBridgeVec4> ClothMotionConstraintsBuffer;
	TArray<FClothBridgeVec4> ClothSeparationConstraintsBuffer;
	TArray<FVector> CachedClothSimPos;
	TArray<float> CachedClothCompression;
	bool bNeedsClothTeleport = true;
	bool bNvClothParamsDirty = true;
	bool bClothSteppedThisFrame = false;
	bool bHasActiveClothWrinkles = false;
	bool bHasActiveClothPull = false;
	bool bClothAwakeLastFrame = false;
	bool bPullTargetDirty = false;
	float PullReleaseEnvelope = 0.0f;
	float LastClothPullMag = 0.0f;
	FVector LastClothPullDir = FVector(0.0f, 0.0f, 1.0f);
	float PendingClothDeltaTime = 1.0f / 60.0f;

	bool bIsPulling = false;
	bool bHasGrabAnchor = false;
	uint32 GrabbedTriangleIndex = 0;
	FVector GrabBarycentric = FVector(1.0f, 0.0f, 0.0f);
	FVector PullTargetWorld = FVector::ZeroVector;
	FVector CurrentPullOffsetLocal = FVector::ZeroVector;
	FVector PullVelocityLocal = FVector::ZeroVector;
	FVector LastAnchorLocal = FVector::ZeroVector;
	float MaxAppliedDisplacement = 0.0f;
	float MaxCurrentStretchRatio = 1.0f;
	float CachedBoundsExpansion = 0.0f;
	float ReferenceScale = 1.0f;
	float BakedSurfaceOffset = 0.0f;
	float LastSyncedSurfaceOffset = 0.0f;

	// 직전 프레임 동기화 상태 캐시
	USkeletalMeshComponent* LastSyncedSourceComponent = nullptr;
	FSkeletalMeshObject* LastSyncedMeshObject = nullptr;
	TArray<FMatrix> LastSyncedSourceTransforms;
	bool bHasSyncedPoseState = false;
	bool bLastSyncedPulling = false;
	bool bLastSyncedHasGrabAnchor = false;
	bool bLastSyncedUseNvCloth = true;
	uint32 LastSyncedGrabbedTriangleIndex = 0;
	FVector LastSyncedPullTargetWorld = FVector::ZeroVector;
	FVector LastSyncedPullOffsetLocal = FVector::ZeroVector;
	FVector LastSyncedPullVelocityLocal = FVector::ZeroVector;

	// 변형 루프 재사용 스크래치 버퍼
	TArray<FVector> ScratchWeldedPos;
	TArray<FVector> ScratchWeldedNormal;
	TArray<int32> ScratchWeldedCounts;
	TArray<FVector> ScratchNvSimPos;
	TArray<float> ScratchNvCompression;
	TArray<FVector> ScratchDeformedWeldedPos;
	TArray<float> ScratchWeldedCrease;
	TArray<float> ScratchOutwardBulge;
	TArray<float> ScratchWeldedStretch;
	TArray<float> ScratchRawCompression;
	mutable TArray<FVector> ScratchAccumNormals;
};
