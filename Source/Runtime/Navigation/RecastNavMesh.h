#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "RecastNavMesh.generated.h"

class FNavigationSystem;
class UWorld;
class dtNavMeshQuery;
struct FNavMeshData;
struct FNavMeshDebugGeometry;

UCLASS()
class ARecastNavMesh : public AActor
{
	GENERATED_BODY()

public:
	ARecastNavMesh();
	~ARecastNavMesh() override;

	using Super::Serialize;
	void Serialize(FStructuredArchive::FRecord Record) override;

	const FNavMeshDebugGeometry* GetDebugGeometry() const;
	const dtNavMeshQuery* GetNavMeshQuery() const;

private:
	friend class FNavigationSystem;

	static ARecastNavMesh* InstallBakedTile(
		UWorld& World, ARecastNavMesh* ExistingNavMesh, TArray<uint8> TileBytes);

	static TUniquePtr<FNavMeshData> CreateNavMeshData(TArray<uint8> TileBytes);
	bool InitializeFromStoredTile();

	void ClearNavMeshData();

	TUniquePtr<FNavMeshData> RecastNavMeshImpl;
};
