#pragma once

#include "Asset/RenderAsset.h"
#include "Render/Buffer.h"
#include "Render/StaticMeshData.h"
#include "Render/MeshRenderData.h"
#include "Render/VertexFactory.h"
#include <array>
#include <unordered_map>
#include <vector>
#include "Mesh.generated.h"

class UBodySetup;
class FPrimitiveSceneProxy;

struct FStaticMeshLODResource
{
	FStaticMeshData Data;
	TUniquePtr<FVertexBuffer> VertexBuffer;
	TUniquePtr<FIndexBuffer> IndexBuffer;
	mutable FLocalVertexFactory VertexFactory;
};

UCLASS()
class UStaticMesh : public URenderAsset
{
	GENERATED_BODY()
public:
    UStaticMesh() : SortID(NextSortID++) {}
	virtual ~UStaticMesh() override;

	FStaticMeshData MeshData;

	// Renderer가 실제로 Bind할 런타임 재질 객체
	TArray<UMaterialInterface*> Materials;

	TUniquePtr<FVertexBuffer> VertexBuffer;
	TUniquePtr<FIndexBuffer> IndexBuffer;

	const FStaticMeshData& GetMeshData() const { return MeshData; }
	UMaterialInterface* GetMaterial(uint32 SlotIndex) const;

public:
	TArray<FStaticMeshLODResource> AdditionalLODs;
	std::array<float, 3> ScreenThresholds{ 0.30f, 0.10f, 0.03f };
    FString SourceObjPath;
    FVertexBuffer* GetVertexBuffer() const { return VertexBuffer.get(); }
    FIndexBuffer* GetIndexBuffer() const { return IndexBuffer.get(); }
    // 버퍼를 다시 만들 수 있으므로 꺼낼 때마다 현재 버퍼로 맞춘다
    const FVertexFactory* GetVertexFactory() const
    {
        VertexFactory.Init(VertexBuffer.get(), IndexBuffer.get());
        return &VertexFactory;
    }
	uint32 GetLODCount() const
	{
		return 1u + static_cast<uint32>(AdditionalLODs.Num());
	}
	const FStaticMeshData& GetLODMeshData(uint32 LODIndex) const
	{
		return (LODIndex == 0 || !AdditionalLODs.IsValidIndex(static_cast<int32>(LODIndex - 1)))
			? MeshData : AdditionalLODs[LODIndex - 1].Data;
	}
	FVertexBuffer* GetLODVertexBuffer(uint32 LODIndex) const
	{
		return (LODIndex == 0 || !AdditionalLODs.IsValidIndex(static_cast<int32>(LODIndex - 1)))
			? VertexBuffer.get() : AdditionalLODs[LODIndex - 1].VertexBuffer.get();
	}
	FIndexBuffer* GetLODIndexBuffer(uint32 LODIndex) const
	{
		return (LODIndex == 0 || !AdditionalLODs.IsValidIndex(static_cast<int32>(LODIndex - 1)))
			? IndexBuffer.get() : AdditionalLODs[LODIndex - 1].IndexBuffer.get();
	}
	const FVertexFactory* GetLODVertexFactory(uint32 LODIndex) const
	{
		if (LODIndex == 0 || !AdditionalLODs.IsValidIndex(static_cast<int32>(LODIndex - 1)))
		{
			return GetVertexFactory();
		}
		const FStaticMeshLODResource& Res = AdditionalLODs[LODIndex - 1];
		Res.VertexFactory.Init(Res.VertexBuffer.get(), Res.IndexBuffer.get());
		return &Res.VertexFactory;
	}
    void RebuildRenderData();
	const FLODSphere& GetLocalLODSphere() const { return LocalLODSphere; }
	const FBox& GetRenderBounds() const { return RenderBounds; }
	// 처음 부를 때 메시 AABB로 박스 하나를 만든다 콜리전 편집이 생기면 저장된 형상으로 바꾼다
	UBodySetup* GetBodySetup();
	bool EnsureTriangleMesh();
	void ClearBodySetup(const UBodySetup* InBodySetup);
	TSharedPtr<const FMeshRenderState> GetRenderState(const std::vector<UMaterialInterface*>& Materials);
    void RegisterProxy(FPrimitiveSceneProxy* Proxy);
    void UnregisterProxy(FPrimitiveSceneProxy* Proxy);

    const uint16 SortID;
private:
    static inline uint16 NextSortID = 0;
    struct FMaterialSlotsHash
    {
        size_t operator()(const std::vector<UMaterialInterface*>& Slots) const;
    };
    FBox RenderBounds;
	FLODSphere LocalLODSphere;
    mutable FLocalVertexFactory VertexFactory;
    std::unordered_map<std::vector<UMaterialInterface*>, TWeakPtr<const FMeshRenderState>, FMaterialSlotsHash> RenderStates;
    TArray<FPrimitiveSceneProxy*> RenderProxies;
    UBodySetup* BodySetup = nullptr;
};
