#pragma once

#include "Asset/RenderAsset.h"
#include "Math/Box.h"
#include "Vertex.h"
#include "SkeletalMeshData.h"
#include "MeshRenderData.h"
#include "Buffer.h"
#include "Material.h"
#include <unordered_map>
#include <vector>
#include "Physics/PhysicsAsset.h"
#include "SkeletalMesh.generated.h"


struct FSkeletalMeshLODRenderData
{
	TArray<FVertex> BindVertices;
	TArray<FSkinWeight> SkinWeights;
	TArray<uint32> Indices;
	TArray<FSkeletalMeshSection> Sections;
	TUniquePtr<FIndexBuffer> IndexBuffer;
	FBox BindAABB;

	// GPU 스키닝 입력. 포즈와 무관하므로 에셋이 갖고 인스턴스끼리 공유한다
	TUniquePtr<FVertexBuffer> GpuSkinVertexBuffer;
	// 본마다 가중치가 있는 정점의 바인드 바운드. 정점이 없는 본은 Min > Max
	TArray<FBox> BoneBounds;
};

struct FSkeletalMeshRenderData
{
	TArray<FSkeletalMeshLODRenderData> LODRenderData;
};

UCLASS()
class USkeletalMesh : public URenderAsset
{
	GENERATED_BODY()
public:
	bool Build(FSkeletalMeshData&& ImportData, FString& OutError);

	const TArray<FBoneInfo>& GetRefSkeleton() const { return RefSkeleton; }
	int32 GetNumBones() const { return RefSkeleton.Num(); }
	int32 FindBoneIndex(const FString& Name) const;
	const FSkeletalMeshRenderData& GetRenderData() const { return RenderData; }
	const TArray<FSkeletalMaterialSlot>& GetMaterialSlots() const { return MaterialSlots; }
	UMaterialInterface* GetMaterial(uint32 SlotIndex) const;

	TSharedPtr<const FMeshRenderState> GetRenderState(const TArray<UMaterialInterface*>& Materials);

	const TArray<FSkeletalMeshSocket>& GetSockets() const { return Sockets; }
	int32 FindSocketIndex(const FString& SocketName) const;
	const FSkeletalMeshSocket* FindSocket(const FString& SocketName) const;
	FSkeletalMeshSocket* FindMutableSocket(const FString& SocketName);
	bool HasSocket(const FString& SocketName) const;
	bool AddSocket(const FSkeletalMeshSocket& NewSocket);
	bool RemoveSocket(const FString& SocketName);
	bool LoadSocketsFromFile(const FString& FilePath);
	bool SaveSocketsToFile(const FString& FilePath) const;

	UPhysicsAsset* GetPhysicsAsset() const { return PhysicsAsset; }
	void SetPhysicsAsset(UPhysicsAsset* NewPhysicsAsset) { PhysicsAsset = NewPhysicsAsset; }
	bool SetPhysicsAssetByPath(const FString& Path, FString& OutError);
	bool LoadPhysicsAssetReference(FString& OutError);
	bool SavePhysicsAssetReference(FString& OutError) const;

private:
	struct FMaterialSlotsHash
	{
		size_t operator()(const TArray<UMaterialInterface*>& Slots) const;
	};

	TArray<FBoneInfo> RefSkeleton;
	TArray<FSkeletalMaterialSlot> MaterialSlots;
	TArray<UMaterialInterface*> Materials;
	FSkeletalMeshRenderData RenderData;
	TArray<FSkeletalMeshSocket> Sockets;
	// UStaticMesh와 같은 캐시: 같은 머티리얼 조합의 컴포넌트끼리 섹션 목록 공유
	TMap<TArray<UMaterialInterface*>, TWeakPtr<const FMeshRenderState>, FMaterialSlotsHash> RenderStates;
	UPROPERTY(EditAnywhere)
	UPhysicsAsset* PhysicsAsset = nullptr;
};
