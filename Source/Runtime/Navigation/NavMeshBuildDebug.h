#pragma once

#include "Containers/Array.h"
#include "Core/Types.h"
#include "Math/Vector.h"
#include "Render/RenderEnums.h"

class FVertexBuffer;
struct rcHeightfield;
struct rcCompactHeightfield;
struct rcContourSet;
struct rcPolyMesh;

struct FNavMeshBuildDebugOverlay
{
	FNavMeshBuildDebugOverlay();
	~FNavMeshBuildDebugOverlay();

	TUniquePtr<FVertexBuffer> Faces;
	TUniquePtr<FVertexBuffer> Lines;
	uint32 FaceVertexCount = 0;
	uint32 LineVertexCount = 0;
};

class FNavMeshBuildDebugData
{
public:
	void CaptureBeforeFilter(const rcHeightfield& Field);
	void CaptureFiltered(const rcHeightfield& Field);
	void CaptureCompact(const rcCompactHeightfield& Field, bool bHasRegions);
	void CaptureContours(const rcContourSet& Contours);
	void CapturePolygonMesh(const rcPolyMesh& Polys);

	bool HasStage(ENavMeshDrawMode Mode) const;
	bool IsVoxelPreviewLimited() const;
	const FNavMeshBuildDebugOverlay& GetOverlay(ENavMeshDrawMode Mode);

private:
	struct FGrid
	{
		FVector RecastOrigin;
		float CellSize = 0.0f;
		float CellHeight = 0.0f;
	};

	struct FRasterSpan
	{
		uint16 X;
		uint16 Z;
		uint16 Bottom;
		uint16 Top;
		bool bWalkableBeforeFilter;
		bool bWalkableAfterFilter;
	};

	struct FRasterRun
	{
		FRasterSpan Span;
		uint16 LengthX;
	};

	struct FCompactRun
	{
		uint16 StartX;
		uint16 Z;
		uint16 LengthX;
		uint16 Floor;
		uint16 Region;
		bool bWalkableAfterErosion;
	};

	struct FContour
	{
		TArray<FVector> Raw;
		TArray<FVector> Simplified;
	};

	struct FPolygon
	{
		int32 FirstIndex;
		int32 VertexCount;
		uint16 Region;
	};

	void MarkCaptured(ENavMeshDrawMode Mode);

	FGrid RasterGrid;
	FGrid CompactGrid;
	TArray<uint32> RasterWalkabilityBeforeFilter;
	TArray<FRasterSpan> RasterSingletons;
	TArray<FRasterRun> RasterRuns;
	TArray<FCompactRun> CompactRuns;
	TArray<FContour> Contours;
	TArray<FVector> PolygonVertices;
	TArray<uint16> PolygonIndices;
	TArray<FPolygon> Polygons;

	uint32 CapturedStages = 0;
	bool bVoxelCaptureTruncated = false;

	ENavMeshDrawMode CachedMode = ENavMeshDrawMode::FinalNavMesh;
	TUniquePtr<FNavMeshBuildDebugOverlay> CachedOverlay;
};
