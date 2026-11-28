#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include "Math/Box.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Render/VertexFactory.h"
#include <vector>

class FVertexBuffer;
class FIndexBuffer;
class FConstantBuffer;
class UTexture2D;
class UMaterialInterface;
class ULandscapeComponent;
class FLandscapeSceneProxy;

namespace LandscapeRender
{
	// 전체 지형 높이 배열(SizeX × SizeY)에서 한 섹션(정점 SubsectionSizeVerts², 경계 중복)을 RGBA8 하이트맵으로 만든다.
	// R,G = 16bit 높이 상위·하위 바이트 / B,A = 노멀 XY.
	// 노멀은 전체 배열 기준 중앙차분이라 이웃 섹션과 경계가 일치한다.
	UTexture2D* CreateHeightmapTexture(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY, int32 BaseX, int32 BaseY, int32 SubsectionSizeVerts);

	// VB에 높이가 없으므로 컬링용 바운드는 CPU 높이 min/max로 직접 만든다 (섹션 로컬 좌표)
	FBox CalcSectionLocalBox(const TArray<uint16>& Heights, int32 SizeX, int32 SizeY, int32 BaseX, int32 BaseY, int32 SectionSizeQuads);
}

struct FLandscapeVertex
{
	uint8 X;
	uint8 Y;
	uint8 LOD;
	uint8 Pad;
};
static_assert(sizeof(FLandscapeVertex) == 4);

struct FLandscapeShaderParams
{
	float SectionSizeQuads = 63.0f;
	float SubsectionSizeVerts = 64.0f;
	float SectionLOD = 0.0f;
	float DebugLODColor = 0.0f;
	int32 HeightmapOffsetX = 0;
	int32 HeightmapOffsetY = 0;
	int32 Pad1[2]{};
	float NeighborLODs[4]{};
};
static_assert(sizeof(FLandscapeShaderParams) % 16 == 0);

class FLandscapeSharedBuffers
{
public:
	static TSharedPtr<FLandscapeSharedBuffers> GetOrCreate(int32 SubsectionSizeVerts);

	explicit FLandscapeSharedBuffers(int32 InSubsectionSizeVerts);
	~FLandscapeSharedBuffers();

	int32 GetNumLODs() const { return static_cast<int32>(LODIndexCounts.size()); }
	int32 GetSubsectionSizeVerts() const { return SubsectionSizeVerts; }
	FVertexBuffer* GetVertexBuffer() const { return VertexBuffer.get(); }
	FIndexBuffer* GetIndexBuffer() const { return IndexBuffer.get(); }
	uint32 GetLODFirstIndex(int32 LOD) const { return LODFirstIndices[LOD]; }
	uint32 GetLODIndexCount(int32 LOD) const { return LODIndexCounts[LOD]; }
	int32 GetLODVertexOffset(int32 LOD) const { return LODVertexOffsets[LOD]; }

private:
	int32 SubsectionSizeVerts = 0;
	TUniquePtr<FVertexBuffer> VertexBuffer;
	TUniquePtr<FIndexBuffer> IndexBuffer;
	std::vector<uint32> LODFirstIndices;
	std::vector<uint32> LODIndexCounts;
	std::vector<int32> LODVertexOffsets;
};

class FLandscapeRenderSystem
{
public:
	static TSharedPtr<FLandscapeRenderSystem> GetOrCreate(const void* LandscapeKey);

	void RegisterSection(FIntPoint SectionKey, const FLandscapeSceneProxy* Proxy);
	void UnregisterSection(FIntPoint SectionKey, const FLandscapeSceneProxy* Proxy);

	void UpdateLODs(const FViewContext& View);
	float GetSectionLOD(FIntPoint SectionKey, float Fallback) const;
private:
	TMap<FIntPoint, const FLandscapeSceneProxy*> Sections;
	TMap<FIntPoint, float> SectionLODs;
	FVector LastCameraPosition = FVector::ZeroVector;
	float LastProjectionScaleSquared = -1.0f;
	bool bDirty = true;
};

class FLandscapeSceneProxy : public FPrimitiveSceneProxy
{
public:
	explicit FLandscapeSceneProxy(ULandscapeComponent* InComponent);
	~FLandscapeSceneProxy() override;

	void UpdateRenderState() override;
	bool HasStaticRelevance() const override { return false; }
	bool HasDynamicRelevance() const override { return true; }
	void GetDynamicMeshElements(const FViewContext& View, FMeshElementCollector& Collector, uint32 LODIndex = 0) const override;

	float ComputeFractionalLOD(const FViewContext& View) const;
private:
	TSharedPtr<FLandscapeSharedBuffers> SharedBuffers;
	TUniquePtr<FConstantBuffer> ParamsBuffer;
	FLandscapeVertexFactory VertexFactory;
	UMaterialInterface* Material = nullptr;

	float LOD0ScreenSizeSq = 0.25f;
	float LOD1ScreenSizeSq = 0.16f;
	float LastLODScreenSizeSq = 0.0f;
	float LogLODDistributionSq = 1.0f;
	int32 LastLODIndex = 3;

	TSharedPtr<FLandscapeRenderSystem> RenderSystem;
	FIntPoint SectionKey{ 0, 0 };
	mutable FLandscapeShaderParams CachedParams{};
};