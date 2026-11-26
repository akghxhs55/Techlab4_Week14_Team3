#pragma once

#include "MeshComponent.h"
#include "Render/Vertex.h"
#include "Render/VertexFactory.h"
#include "Render/Buffer.h"
#include "ClothBridge.h"
#include "ClothComponent.generated.h"

class UStaticMesh;
class USkeletalMeshComponent;

struct FClothBoneCapsule
{
	FString BoneA;
	FString BoneB;
	float RadiusA = 0.14f;
	float RadiusB = 0.14f;
};

struct FClothBonePin
{
	uint32 VertexIndex = 0;
	FString BoneA;
	FString BoneB;
	float WeightB = 0.0f;
};

UENUM()
enum class EClothPreset : uint8
{
	Grid,
	Cone,
	StaticMesh
};

UCLASS()
class UClothComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UClothComponent();
	virtual ~UClothComponent() override;

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime) override;

	virtual int32 GetNumMaterials() const override { return 1; }
	virtual FString GetMaterialSlotName(int32 SlotIndex) const override;
	virtual UMaterialInterface* GetDefaultMaterial(int32 SlotIndex) const override;
	virtual FBox CalcLocalBounds() const override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual void PostEditChangeProperty(const FProperty& Property) override;
	virtual bool ShouldCreatePhysicsState() const override;

	// 에디터 및 런타임에서 천 구성을 다시 생성
	void RebuildClothMesh();
	void ResetClothPose();

	// 스켈레탈 본 핀과 캡슐 콜라이더 설정
	void SetTargetSkeletalMesh(USkeletalMeshComponent* InMeshComponent);
	void PinVertexToBone(uint32 VertexIndex, const FString& BoneName);
	void PinVertexBetweenBones(uint32 VertexIndex, const FString& BoneA, const FString& BoneB, float WeightB);
	void PinRowToBone(int32 RowIndex, const FString& BoneName);
	void ClearBonePins();
	void AddBoneCapsuleCollider(const FString& InBoneA, const FString& InBoneB, float InRadiusA, float InRadiusB);
	void ClearBoneCapsuleColliders();

	// 물리 씬에서 시뮬레이션 전후로 호출
	void PreClothSimulate();
	void PostClothSimulate();

	const FVertexFactory* GetVertexFactory() const { return &VertexFactory; }
	FClothInstanceHandle GetClothHandle() const { return ClothHandle; }

	UPROPERTY(EditAnywhere)
	EClothPreset Preset = EClothPreset::Grid;

	UPROPERTY(EditAnywhere)
	UStaticMesh* SourceStaticMesh = nullptr;

	UPROPERTY(EditAnywhere)
	float ClothWidth = 1.0f;

	UPROPERTY(EditAnywhere)
	float BottomWidth = 0.0f;

	UPROPERTY(EditAnywhere)
	float ClothHeight = 1.4f;

	UPROPERTY(EditAnywhere)
	float TopRadius = 0.22f;

	UPROPERTY(EditAnywhere)
	float BottomRadius = 0.45f;

	UPROPERTY(EditAnywhere)
	int32 SegmentsX = 12;

	UPROPERTY(EditAnywhere)
	int32 SegmentsY = 16;

	UPROPERTY(EditAnywhere)
	bool bEnableClothSimulation = true;

	UPROPERTY(EditAnywhere)
	bool bDoubleSidedMesh = true;

	UPROPERTY(EditAnywhere)
	bool bPinTopRow = true;

	UPROPERTY(EditAnywhere)
	float PinHeightRatio = 0.92f;

	UPROPERTY(EditAnywhere)
	FVector Gravity = FVector(0.0f, 0.0f, -9.81f);

	UPROPERTY(EditAnywhere)
	FVector WindVelocity = FVector(0.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere)
	FVector Damping = FVector(0.05f, 0.05f, 0.05f);

	UPROPERTY(EditAnywhere)
	float StretchStiffness = 1.0f;

	UPROPERTY(EditAnywhere)
	float BendStiffness = 0.5f;

	UPROPERTY(EditAnywhere)
	float ShearStiffness = 0.8f;

	UPROPERTY(EditAnywhere)
	float TetherScale = 1.0f;

	UPROPERTY(EditAnywhere)
	float TetherStiffness = 1.0f;

	UPROPERTY(EditAnywhere)
	float SolverFrequency = 180.0f;

	UPROPERTY(EditAnywhere)
	float DragCoefficient = 0.25f;

	UPROPERTY(EditAnywhere)
	float LiftCoefficient = 0.08f;

	UPROPERTY(EditAnywhere)
	float LinearInertiaScale = 0.45f;

	UPROPERTY(EditAnywhere)
	float AngularInertiaScale = 0.25f;

	UPROPERTY(EditAnywhere)
	float CentrifugalInertiaScale = 0.15f;

	UPROPERTY(EditAnywhere)
	bool bEnableContinuousCollision = true;

	UPROPERTY(EditAnywhere)
	bool bUseVirtualParticles = true;

	UPROPERTY(EditAnywhere)
	bool bUseBackstop = false;

	UPROPERTY(EditAnywhere)
	float BackstopOffset = 0.04f;

	UPROPERTY(EditAnywhere)
	float BackstopRadius = 0.35f;

	UPROPERTY(EditAnywhere)
	bool bUseMotionConstraints = false;

	UPROPERTY(EditAnywhere)
	float MaxMotionDistance = 0.90f;

protected:
	virtual void OnRegister() override;
	virtual void OnCreatePhysicsState() override;
	virtual void OnDestroyPhysicsState() override;

private:
	void BuildGridData();
	void BuildConeData();
	bool BuildStaticMeshData();
	void CreateGpuBuffers();
	void UpdateDynamicVertexBuffer();
	void RecomputeVertexNormals();
	void UpdateBoundsFromVertices();
	void CreateNvClothInstance();
	void DestroyNvClothInstance();
	void PushClothParameters();
	void PushBoneColliders();
	void UpdateBonePins(bool bInitializeFullMesh);
	FMatrix ComputeBoneSkinMatrix(int32 BoneIndex) const;

	USkeletalMeshComponent* TargetSkeletalMesh = nullptr;
	TArray<FClothBoneCapsule> BoneCapsules;
	TArray<FClothBridgeVec4> SphereScratch;
	TArray<uint32> CapsuleIndexScratch;

	TArray<FClothBonePin> BonePins;
	TArray<uint32> PinnedVertexIndices;
	TArray<FClothBridgeVec3> PinnedLocalScratch;
	TArray<FVector> RestNormals;
	TArray<FClothBridgeVec4> ConstraintScratch;
	bool bInitialDrapeDone = false;

	TArray<FVector> RestPositions;
	TArray<FVector2> RestUVs;
	TArray<float> InvMasses;
	TArray<uint32> SimIndices;
	TArray<FClothBridgeVec4> ParticleScratch;

	TArray<FVertex> RenderVertices;
	TArray<uint32> RenderIndices;
	TUniquePtr<FVertexBuffer> DynamicVertexBuffer;
	TUniquePtr<FIndexBuffer> IndexBuffer;
	FLocalVertexFactory VertexFactory;

	FBox LocalBounds = FBox(FVector(-0.5f, -0.5f, -0.5f), FVector(0.5f, 0.5f, 0.5f));
	FBox CachedProxyBounds = FBox(FVector(-0.5f, -0.5f, -0.5f), FVector(0.5f, 0.5f, 0.5f));

	FClothFabricHandle FabricHandle = nullptr;
	FClothInstanceHandle ClothHandle = nullptr;
	bool bParamsDirty = true;
	bool bTeleportPending = true;
};
