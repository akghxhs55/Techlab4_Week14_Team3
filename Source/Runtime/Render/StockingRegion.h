#pragma once

#include "UObject/Object.h"
#include "Containers/Array.h"
#include "Core/EngineString.h"
#include "Render/SkeletalMesh.h"
#include "Render/StockingMaterial.h"
#include "StockingRegion.generated.h"

UCLASS()
class UStockingRegion : public UObject
{
	GENERATED_BODY()

public:
	UStockingRegion() = default;
	virtual ~UStockingRegion() override = default;
	virtual void Serialize(FStructuredArchive::FRecord Record) override;

	// 원본 메시 기준 메타데이터 갱신
	void BindToSourceMesh(const USkeletalMesh& SourceMesh, const FString& InSourceMeshPath, int32 InLODIndex = 0);

	// 삼각형 선택 편집
	void AddTriangle(uint32 TriangleIndex);
	void RemoveTriangle(uint32 TriangleIndex);
	void SetTriangles(const TArray<uint32>& InTriangles);
	void ClearTriangles();
	bool ContainsTriangle(uint32 TriangleIndex) const;

	// 본 가중치 기반 선택 및 인접 확장 축소
	void SelectTrianglesByBones(const USkeletalMesh& SourceMesh, const TArray<int32>& BoneIndices, float MinWeightThreshold = 0.25f);
	void SelectTrianglesByBoneFilter(
		const USkeletalMesh& SourceMesh,
		const TArray<int32>& BoneIndices,
		float MinWeightThreshold,
		float MaxBoneRadiusRatio,
		float MaxLegHeightRatio);
	void GrowSelection(const USkeletalMesh& SourceMesh);
	void ShrinkSelection(const USkeletalMesh& SourceMesh);

	// 원본 메시 재임포트 및 인덱스 유효성 검사
	bool ValidateAgainstMesh(const USkeletalMesh& SourceMesh, FString& OutError) const;
	static uint64 ComputeMeshTopologyHash(const USkeletalMesh& SourceMesh, int32 LODIndex = 0);
	static float ComputeReferenceScale(const USkeletalMesh& SourceMesh, int32 LODIndex = 0);

	// 선택 영역에서 스키닝 가능한 스타킹 메시 생성
	bool BuildStockingMesh(const USkeletalMesh& SourceMesh, USkeletalMesh& OutStockingMesh, FString& OutError) const;

	// 영역 파일 저장 및 로드
	bool SaveToFile(const FString& FilePath) const;
	bool LoadFromFile(const FString& FilePath, FString& OutError);

	int32 GetSelectedTriangleCount() const { return TriangleIndices.Num(); }
	const TArray<uint32>& GetTriangleIndices() const { return TriangleIndices; }
	const FString& GetRegionFilePath() const { return RegionFilePath; }
	void SetRegionFilePath(const FString& InPath) { RegionFilePath = InPath; }

	UPROPERTY(EditAnywhere)
	FString SourceMeshPath;

	UPROPERTY(EditAnywhere)
	int32 SourceLODIndex = 0;

	UPROPERTY(EditAnywhere)
	uint32 SourceVertexCount = 0;

	UPROPERTY(EditAnywhere)
	uint32 SourceTriangleCount = 0;

	UPROPERTY(EditAnywhere)
	int32 SubdivisionIterations = 1;

	UPROPERTY(EditAnywhere)
	float SurfaceOffset = 0.0025f;

	UPROPERTY(EditAnywhere)
	float BoundaryFeatherDistance = 0.045f;

	UPROPERTY(EditAnywhere)
	TArray<uint32> TriangleIndices;

	// 저장된 머티리얼 당김 주름 다리 필터 프리셋
	bool bHasSavedMaterialParams = false;
	FStockingMaterialParams SavedMaterialParams;

	bool bHasSavedPullParams = false;
	float SavedPullRadius = 0.11f;
	float SavedMaxPullDistance = 0.075f;
	float SavedPullFalloffExponent = 2.15f;
	float SavedWrinkleAmplitude = 0.0085f;
	float SavedWrinkleFrequency = 7.0f;
	float SavedSpringStiffness = 160.0f;
	float SavedSpringDamping = 14.0f;
	float SavedBoundsPadding = 0.04f;

	bool bHasSavedLegFilter = false;
	float SavedLegRadiusRatio = 0.072f;
	float SavedLegHeightRatio = 0.84f;

	bool bHasSavedClothWrinkleParams = false;
	bool SavedUseNvClothSimulation = true;
	float SavedClothJointWrinkleSlack = 0.0035f;
	float SavedClothBackstopRadius = 0.025f;
	float SavedClothStretchStiffness = 0.90f;
	float SavedClothBendStiffness = 0.10f;
	float SavedClothShearStiffness = 0.65f;
	float SavedClothCompressionLimit = 1.0f;
	float SavedClothStretchLimit = 1.18f;
	float SavedClothMotionStiffness = 0.85f;
	float SavedClothDamping = 0.45f;
	float SavedClothSelfCollisionDistance = 0.0f;
	float SavedClothSelfCollisionStiffness = 0.45f;

private:
	uint64 SourceTopologyHash = 0;
	FString RegionFilePath;
};
