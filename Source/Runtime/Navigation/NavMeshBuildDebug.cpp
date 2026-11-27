#include "EnginePCH.h"
#include "Navigation/NavMeshBuildDebug.h"

#include "Navigation/NavGeometry.h"
#include "Render/Buffer.h"
#include "Render/RenderCommand.h"
#include "Render/Vertex.h"

#include <Recast.h>

namespace
{
	constexpr int32 MaxCapturedSpans = 262144; // 512 * 512
	constexpr int32 MaxDrawnVoxelSpans = 65536; // 256 * 256

	template <class F>
	void VisitRasterSpans(const rcHeightfield& Field, F&& Visitor)
	{
		for (int Z = 0; Z < Field.height; ++Z)
		{
			for (int X = 0; X < Field.width; ++X)
			{
				for (const rcSpan* Span = Field.spans[X + Z * Field.width];
					Span; Span = Span->next)
				{
					if (!Visitor(X, Z, *Span))
						return;
				}
			}
		}
	}

	FVector GridPointToWorld(
		const float* Origin, float CelSize, float CellHeight,
		int X, int Y, int Z)
	{
		return Navigation::FromRecast(FVector(
			Origin[0] + X * CelSize,
			Origin[1] + Y * CellHeight,
			Origin[2] + Z * CelSize));
	}

	// GPU 렌더링 헬퍼
	FVector4 RegionColor(uint16 Region)
	{
		static const FVector Colors[] = {
			FVector(0.20f, 0.80f, 0.65f),
			FVector(0.40f, 0.65f, 1.00f),
			FVector(1.00f, 0.65f, 0.15f),
			FVector(0.85f, 0.40f, 0.95f),
			FVector(0.60f, 0.85f, 0.25f),
			FVector(1.00f, 0.45f, 0.45f),
		};

		if (Region == 0)
			return FVector4(0.48f, 0.52f, 0.56f, 0.30f);

		const FVector& RGB = Colors[(Region - 1) % 6];
		return FVector4(RGB.X, RGB.Y, RGB.Z, 0.48f);
	}

	void AddVertex(TArray<FLineBatchVertex>& Out, const FVector& Point,
		const FVector4& Color, float Offset = 0.04f)
	{
		Out.Add(FLineBatchVertex{
			Point + FVector(0.0f, 0.0f, Offset), FVector2(), Color
			});
	}

	void AddTriangle(TArray<FLineBatchVertex>& Out, const FVector& A,
		const FVector& B, const FVector& C, const FVector4& Color)
	{
		AddVertex(Out, A, Color);
		AddVertex(Out, B, Color);
		AddVertex(Out, C, Color);
	}

	void AddClosedLoop(TArray<FLineBatchVertex>& Out, const TArray<FVector>& Points,
		const FVector4& Color, float Offset)
	{
		if (Points.Num() < 2)
			return;

		for (int32 I = 0; I < Points.Num(); ++I)
		{
			AddVertex(Out, Points[I], Color, Offset);
			AddVertex(Out, Points[(I + 1) % Points.Num()], Color, Offset);
		}
	}

	TUniquePtr<FVertexBuffer> UploadVertices(const TArray<FLineBatchVertex>& Vertices)
	{
		if (Vertices.IsEmpty())
			return nullptr;

		return RenderCommand::CreateStaticVertexBuffer(
			Vertices.GetData(),
			static_cast<uint32>(Vertices.Num() * sizeof(FLineBatchVertex)),
			sizeof(FLineBatchVertex));
	}
}

FNavMeshBuildDebugOverlay::FNavMeshBuildDebugOverlay() = default;
FNavMeshBuildDebugOverlay::~FNavMeshBuildDebugOverlay() = default;

bool FNavMeshBuildDebugData::HasStage(ENavMeshDrawMode Mode) const
{
	return (CapturedStages & (1u << static_cast<uint32>(Mode))) != 0;
}

void FNavMeshBuildDebugData::MarkCaptured(ENavMeshDrawMode Mode)
{
	CapturedStages |= (1u << static_cast<uint32>(Mode));
}

bool FNavMeshBuildDebugData::IsVoxelPreviewLimited() const
{
	return bVoxelCaptureTruncated ||
		RasterSpans.Num() > MaxDrawnVoxelSpans ||
		CompactSpans.Num() > MaxDrawnVoxelSpans;
}

void FNavMeshBuildDebugData::CaptureRasterized(const rcHeightfield& Field)
{
	RasterGrid = {
		FVector(Field.bmin[0], Field.bmin[1], Field.bmin[2]),
		Field.cs, Field.ch
	};

	VisitRasterSpans(Field, [&](int X, int Z, const rcSpan& Span)
		{
			if (RasterSpans.Num() == MaxCapturedSpans)
			{
				bVoxelCaptureTruncated = true;
				return false;
			}

			const bool bWalkable = Span.area != RC_NULL_AREA;
			RasterSpans.Add(FRasterSpan{
				static_cast<uint16>(X),
				static_cast<uint16>(Z),
				static_cast<uint16>(Span.smin),
				static_cast<uint16>(Span.smax),
				bWalkable, bWalkable
				});
			return true;
		});

	MarkCaptured(ENavMeshDrawMode::RasterizedVoxels);
}

void FNavMeshBuildDebugData::CaptureFiltered(const rcHeightfield& Field)
{
	int32 Index = 0;
	VisitRasterSpans(Field, [&](int, int, const rcSpan& Span)
		{
			if (Index == RasterSpans.Num())
				return false;

			RasterSpans[Index++].bWalkableAfterFilter = Span.area != RC_NULL_AREA;
			return true;
		});

	MarkCaptured(ENavMeshDrawMode::WalkabilityFilter);
}

void FNavMeshBuildDebugData::CaptureBeforeErosion(const rcCompactHeightfield& Field)
{
	CompactGrid = {
		FVector(Field.bmin[0], Field.bmin[1], Field.bmin[2]),
		Field.cs, Field.ch,
	};

	for (int Z = 0; Z < Field.height; ++Z)
	{
		for (int X = 0; X < Field.width; ++X)
		{
			const rcCompactCell& Cell = Field.cells[X + Z * Field.width];
			const uint32 End = Cell.index + Cell.count;

			for (uint32 Index = Cell.index; Index < End; ++Index)
			{
				if (CompactSpans.Num() == MaxCapturedSpans)
				{
					bVoxelCaptureTruncated = true;
					return;
				}

				const bool bWalkable = Field.areas[Index] != RC_NULL_AREA;
				CompactSpans.Add(FCompactSpan{
					Index,
					static_cast<uint16>(X),
					static_cast<uint16>(Z),
					Field.spans[Index].y,
					0,
					bWalkable,
					bWalkable
					});
			}
		}
	}
}

void FNavMeshBuildDebugData::CaptureEroded(const rcCompactHeightfield& Field)
{
	for (FCompactSpan& Span : CompactSpans)
		Span.bWalkableAfterErosion = Field.areas[Span.SourceIndex] != RC_NULL_AREA;

	MarkCaptured(ENavMeshDrawMode::AgentRadiusErosion);
}

void FNavMeshBuildDebugData::CaptureRegions(const rcCompactHeightfield& Field)
{
	for (FCompactSpan& Span : CompactSpans)
		Span.Region = Field.spans[Span.SourceIndex].reg;

	MarkCaptured(ENavMeshDrawMode::Regions);
}

void FNavMeshBuildDebugData::CaptureContours(const rcContourSet& Source)
{
	const auto CopyVertices = [&](const int* Data, int Count, TArray<FVector>& Out)
		{
			Out.Reserve(Count);
			for (int Index = 0; Index < Count; ++Index)
			{
				const int* V = Data + Index * 4;
				Out.Add(GridPointToWorld(
					Source.bmin, Source.cs, Source.ch,
					V[0], V[1], V[2]));
			}
		};

	for (int Index = 0; Index < Source.nconts; ++Index)
	{
		const rcContour& Input = Source.conts[Index];
		FContour Output;
		CopyVertices(Input.rverts, Input.nrverts, Output.Raw);
		CopyVertices(Input.verts, Input.nverts, Output.Simplified);
		Contours.Add(std::move(Output));
	}

	MarkCaptured(ENavMeshDrawMode::Contours);
}

void FNavMeshBuildDebugData::CapturePolygonMesh(const rcPolyMesh& Mesh)
{
	PolygonVertices.Reserve(Mesh.nverts);
	for (int Index = 0; Index < Mesh.nverts; ++Index)
	{
		const unsigned short* V = Mesh.verts + Index * 3;
		PolygonVertices.Add(
			GridPointToWorld(Mesh.bmin, Mesh.cs, Mesh.ch,
				V[0], V[1], V[2]));

	}

	Polygons.Reserve(Mesh.npolys);
	for (int Index = 0; Index < Mesh.npolys; ++Index)
	{
		const unsigned short* Data = Mesh.polys + Index * Mesh.nvp * 2;
		const int32 FirstIndex = PolygonIndices.Num();

		for (int Corner = 0; Corner < Mesh.nvp && Data[Corner] != RC_MESH_NULL_IDX; ++Corner)
		{
			PolygonIndices.Add(Data[Corner]);
		}

		const int32 Count = PolygonIndices.Num() - FirstIndex;
		if (Count >= 3)
			Polygons.Add(FPolygon(FirstIndex, Count, Mesh.regs[Index]));
	}

	MarkCaptured(ENavMeshDrawMode::PolygonMesh);
}

const FNavMeshBuildDebugOverlay& FNavMeshBuildDebugData::GetOverlay(
	ENavMeshDrawMode Mode)
{
	if (CachedOverlay && CachedMode == Mode)
		return *CachedOverlay;

	CachedOverlay.reset();

	TArray<FLineBatchVertex> Faces;
	TArray<FLineBatchVertex> Lines;

	const FVector4 Walkable(0.18f, 0.82f, 0.35f, 0.48f);
	const FVector4 Blocked(0.48f, 0.52f, 0.56f, 0.30f);
	const FVector4 Filtered(1.00f, 0.22f, 0.17f, 0.62f);
	const FVector4 Eroded(1.00f, 0.64f, 0.12f, 0.65f);

	const auto AddCellTop = [&](const FGrid& Grid, int X, int Z, int Y,
		const FVector4& Color)
		{
			const float X0 = Grid.RecastOrigin.X + X * Grid.CellSize;
			const float Z0 = Grid.RecastOrigin.Z + Z * Grid.CellSize;
			const float Height = Grid.RecastOrigin.Y + Y * Grid.CellHeight;
			const float X1 = X0 + Grid.CellSize;
			const float Z1 = Z0 + Grid.CellSize;

			const FVector A = Navigation::FromRecast(FVector(X0, Height, Z0));
			const FVector B = Navigation::FromRecast(FVector(X1, Height, Z0));
			const FVector C = Navigation::FromRecast(FVector(X1, Height, Z1));
			const FVector D = Navigation::FromRecast(FVector(X0, Height, Z1));

			AddTriangle(Faces, A, B, C, Color);
			AddTriangle(Faces, A, C, D, Color);
		};

	const auto PreviewStride = [](int32 Count)
		{
			return Count > MaxDrawnVoxelSpans
				? (Count + MaxDrawnVoxelSpans - 1) / MaxDrawnVoxelSpans
				: 1;
		};

	switch (Mode)
	{
	case ENavMeshDrawMode::RasterizedVoxels:
	case ENavMeshDrawMode::WalkabilityFilter:
	{
		const int32 Step = PreviewStride(RasterSpans.Num());
		for (int32 I = 0; I < RasterSpans.Num(); I += Step)
		{
			const FRasterSpan& Span = RasterSpans[I];
			FVector4 Color = Span.bWalkableBeforeFilter ? Walkable : Blocked;
			if (Mode == ENavMeshDrawMode::WalkabilityFilter)
			{
				Color = Span.bWalkableAfterFilter ? Walkable : Blocked;
				if (Span.bWalkableBeforeFilter && !Span.bWalkableAfterFilter)
					Color = Filtered;
			}

			AddCellTop(RasterGrid, Span.X, Span.Z, Span.Top, Color);
		}
		break;
	}
	case ENavMeshDrawMode::AgentRadiusErosion:
	case ENavMeshDrawMode::Regions:
	{
		const int32 Step = PreviewStride(CompactSpans.Num());
		for (int32 I = 0; I < CompactSpans.Num(); I += Step)
		{
			const FCompactSpan& Span = CompactSpans[I];
			FVector4 Color = Span.bWalkableAfterErosion ? Walkable : Blocked;

			if (Mode == ENavMeshDrawMode::Regions)
				Color = RegionColor(Span.Region);
			else if (Span.bWalkableBeforeErosion && !Span.bWalkableAfterErosion)
				Color = Eroded;

			AddCellTop(CompactGrid, Span.X, Span.Z, Span.Floor, Color);
		}
		break;
	}
	case ENavMeshDrawMode::Contours:
		for (const FContour& Contour : Contours)
		{
			AddClosedLoop(Lines, Contour.Raw, FVector4(0.64f, 0.68f, 0.73f, 0.80f), 0.04f);
			AddClosedLoop(Lines, Contour.Simplified, FVector4(0.20f, 0.95f, 1.00f, 1.00f), 0.06f);
		}
		break;

	case ENavMeshDrawMode::PolygonMesh:
		for (const FPolygon& Poly : Polygons)
		{
			const auto Vertex = [&](int32 Corner) -> const FVector&
				{
					return PolygonVertices[PolygonIndices[Poly.FirstIndex + Corner]];
				};

			const FVector4 Fill = RegionColor(Poly.Region);
			const FVector4 Edge(Fill.X, Fill.Y, Fill.Z, 1.0f);

			// rcPolyMesh의 보행 폴리곤은 볼록하므로 fan으로 그릴 수 있다.
			for (int32 I = 1; I + 1 < Poly.VertexCount; ++I)
				AddTriangle(Faces, Vertex(0), Vertex(I), Vertex(I + 1), Fill);

			for (int32 I = 0; I < Poly.VertexCount; ++I)
			{
				AddVertex(Lines, Vertex(I), Edge, 0.06f);
				AddVertex(Lines, Vertex((I + 1) % Poly.VertexCount), Edge, 0.06f);
			}
		}
		break;

	case ENavMeshDrawMode::FinalNavMesh:
		break; // 최종 표시는 기존 DrawNavigation() 경로에서 처리한다.
	}

	CachedOverlay = MakeUnique<FNavMeshBuildDebugOverlay>();
	CachedOverlay->Faces = UploadVertices(Faces);
	CachedOverlay->Lines = UploadVertices(Lines);
	CachedOverlay->FaceVertexCount = static_cast<uint32>(Faces.Num());
	CachedOverlay->LineVertexCount = static_cast<uint32>(Lines.Num());
	CachedMode = Mode;
	return *CachedOverlay;
}
