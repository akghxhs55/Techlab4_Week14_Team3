#include "EnginePCH.h"
#include "Navigation/NavMeshBuildDebug.h"

#include "Navigation/NavGeometry.h"
#include "Render/Buffer.h"
#include "Render/RenderCommand.h"
#include "Render/Vertex.h"

#include <Recast.h>

#include <unordered_map>

namespace
{
	constexpr size_t RasterCaptureBudget = 2560 * 1024;
	constexpr size_t CompactCaptureBudget = 4 * 1024 * 1024;
	constexpr size_t BitsPerWord = 32;
	constexpr uint8 WalkableBeforeFilter = 1;
	constexpr uint8 WalkableAfterFilter = 2;

	struct FVoxelRowRun
	{
		uint16 StartX;
		uint16 LengthX;
		uint16 Bottom;
		uint16 Top;
		uint16 Region;
		uint8 State;
	};

	class FVoxelRow
	{
	public:
		void Add(int X, uint16 Bottom, uint16 Top, uint16 Region, uint8 State)
		{
			const uint64 Key = static_cast<uint64>(Bottom) | (static_cast<uint64>(Top) << 16) |
				(static_cast<uint64>(Region) << 32) | (static_cast<uint64>(State) << 48);
			auto [It, bInserted] = LastRunByState.try_emplace(Key, 0);
			if (!bInserted)
			{
				FVoxelRowRun& Last = Runs[It->second];
				if (Last.StartX + Last.LengthX == X)
				{
					++Last.LengthX;
					return;
				}
			}

			It->second = Runs.Add(FVoxelRowRun{static_cast<uint16>(X), 1, Bottom, Top, Region, State});
		}

		void Reset()
		{
			Runs.Reset();
			LastRunByState.clear();
		}

		const TArray<FVoxelRowRun>& GetRuns() const { return Runs; }

	private:
		TArray<FVoxelRowRun> Runs;
		std::unordered_map<uint64, uint32> LastRunByState;
	};

	template <class FReadColumn, class FStoreRun>
	bool CaptureVoxelRows(int Width, int Height, FReadColumn&& ReadColumn, FStoreRun&& StoreRun)
	{
		FVoxelRow Row;
		for (int Z = 0; Z < Height; ++Z)
		{
			for (int X = 0; X < Width; ++X)
				ReadColumn(Row, X, Z);

			for (const FVoxelRowRun& Run : Row.GetRuns())
			{
				if (!StoreRun(static_cast<uint16>(Z), Run))
					return false;
			}
			Row.Reset();
		}
		return true;
	}

	enum class EVoxelDisplayState : uint16 { Blocked, Walkable, Filtered, Eroded };

	struct FVoxelDisplayRun
	{
		uint16 StartX;
		uint16 Z;
		uint16 LengthX;
		uint16 Height;
		uint16 State;
	};

	struct FVoxelRectangle
	{
		FVoxelDisplayRun FirstRun;
		uint16 LengthZ = 1;
	};

	template <class FRecord, class FGetRun, class FDrawRectangle>
	void DrawMergedVoxelRows(const TArray<FRecord>& Records, FGetRun&& GetRun, FDrawRectangle&& DrawRectangle)
	{
		std::unordered_map<uint64, FVoxelRectangle> PreviousRow;
		std::unordered_map<uint64, FVoxelRectangle> CurrentRow;
		int32 CurrentZ = -1;

		const auto Flush = [&](const auto& Row)
		{
			for (const auto& Entry : Row)
				DrawRectangle(Entry.second);
		};

		for (const FRecord& Record : Records)
		{
			const FVoxelDisplayRun Run = GetRun(Record);
			if (Run.Z != CurrentZ)
			{
				Flush(PreviousRow);
				PreviousRow.clear();
				if (Run.Z == CurrentZ + 1)
					PreviousRow.swap(CurrentRow);
				else
				{
					Flush(CurrentRow);
					CurrentRow.clear();
				}
				CurrentZ = Run.Z;
			}

			const uint64 Key = static_cast<uint64>(Run.StartX) | (static_cast<uint64>(Run.LengthX) << 16) |
				(static_cast<uint64>(Run.Height) << 32) | (static_cast<uint64>(Run.State) << 48);
			auto Previous = PreviousRow.extract(Key);
			if (Previous.empty())
				CurrentRow.emplace(Key, FVoxelRectangle{Run});
			else
			{
				++Previous.mapped().LengthZ;
				CurrentRow.insert(std::move(Previous));
			}
		}

		Flush(PreviousRow);
		Flush(CurrentRow);
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
	return bVoxelCaptureTruncated;
}

void FNavMeshBuildDebugData::CaptureBeforeFilter(const rcHeightfield& Field)
{
	RasterGrid = {
		FVector(Field.bmin[0], Field.bmin[1], Field.bmin[2]),
		Field.cs, Field.ch
	};

	size_t SpanIndex = 0;
	for (int Cell = 0; Cell < Field.width * Field.height; ++Cell)
	{
		for (const rcSpan* Span = Field.spans[Cell]; Span; Span = Span->next, ++SpanIndex)
		{
			const size_t Bit = SpanIndex % BitsPerWord;
			if (Bit == 0)
				RasterWalkabilityBeforeFilter.Add(0);
			if (Span->area != RC_NULL_AREA)
				RasterWalkabilityBeforeFilter.Last() |= uint32{1} << Bit;
		}
	}
}

void FNavMeshBuildDebugData::CaptureFiltered(const rcHeightfield& Field)
{
	// The filters change area flags in place, so span ordinals still match the bit array.
	size_t SpanIndex = 0;
	size_t StoredBytes = 0;
	const bool bComplete = CaptureVoxelRows(Field.width, Field.height,
		[&](FVoxelRow& Row, int X, int Z)
		{
			for (const rcSpan* Span = Field.spans[X + Z * Field.width];
				Span; Span = Span->next, ++SpanIndex)
			{
				const uint32 Word = RasterWalkabilityBeforeFilter[static_cast<uint32>(SpanIndex / BitsPerWord)];
				const bool bBefore = (Word & (uint32{1} << (SpanIndex % BitsPerWord))) != 0;
				const bool bAfter = Span->area != RC_NULL_AREA;
				const uint8 State = static_cast<uint8>(
					(bBefore ? WalkableBeforeFilter : 0) | (bAfter ? WalkableAfterFilter : 0));
				Row.Add(X, static_cast<uint16>(Span->smin), static_cast<uint16>(Span->smax), 0, State);
			}
		},
		[&](uint16 Z, const FVoxelRowRun& Run)
		{
			const size_t RecordBytes = Run.LengthX == 1 ? sizeof(FRasterSpan) : sizeof(FRasterRun);
			if (RecordBytes > RasterCaptureBudget - StoredBytes)
				return false;

			const FRasterSpan Span{Run.StartX, Z, Run.Bottom, Run.Top,
				(Run.State & WalkableBeforeFilter) != 0, (Run.State & WalkableAfterFilter) != 0};
			if (Run.LengthX == 1)
				RasterSingletons.Add(Span);
			else
				RasterRuns.Add(FRasterRun{Span, Run.LengthX});
			StoredBytes += RecordBytes;
			return true;
		});

	RasterWalkabilityBeforeFilter = TArray<uint32>{};
	bVoxelCaptureTruncated |= !bComplete;
	MarkCaptured(ENavMeshDrawMode::RasterizedVoxels);
	MarkCaptured(ENavMeshDrawMode::WalkabilityFilter);
}

void FNavMeshBuildDebugData::CaptureCompact(const rcCompactHeightfield& Field, bool bHasRegions)
{
	CompactGrid = {
		FVector(Field.bmin[0], Field.bmin[1], Field.bmin[2]),
		Field.cs, Field.ch,
	};

	size_t StoredBytes = 0;
	const bool bComplete = CaptureVoxelRows(Field.width, Field.height,
		[&](FVoxelRow& Row, int X, int Z)
		{
			const rcCompactCell& Cell = Field.cells[X + Z * Field.width];
			const uint32 End = Cell.index + Cell.count;
			for (uint32 Index = Cell.index; Index < End; ++Index)
			{
				const rcCompactSpan& Span = Field.spans[Index];
				Row.Add(X, 0, Span.y, bHasRegions ? Span.reg : uint16{0}, Field.areas[Index] != RC_NULL_AREA);
			}
		},
		[&](uint16 Z, const FVoxelRowRun& Run)
		{
			if (sizeof(FCompactRun) > CompactCaptureBudget - StoredBytes)
				return false;

			CompactRuns.Add(FCompactRun{Run.StartX, Z, Run.LengthX, Run.Top, Run.Region, Run.State != 0});
			StoredBytes += sizeof(FCompactRun);
			return true;
		});

	bVoxelCaptureTruncated |= !bComplete;
	MarkCaptured(ENavMeshDrawMode::AgentRadiusErosion);
	if (bHasRegions)
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

	const auto AddRectangleTop = [&](const FGrid& Grid, const FVoxelRectangle& Rectangle,
		const FVector4& Color)
		{
			const FVoxelDisplayRun& Run = Rectangle.FirstRun;
			const float X0 = Grid.RecastOrigin.X + Run.StartX * Grid.CellSize;
			const float Z0 = Grid.RecastOrigin.Z + Run.Z * Grid.CellSize;
			const float Height = Grid.RecastOrigin.Y + Run.Height * Grid.CellHeight;
			const float X1 = Grid.RecastOrigin.X + (Run.StartX + Run.LengthX) * Grid.CellSize;
			const float Z1 = Grid.RecastOrigin.Z + (Run.Z + Rectangle.LengthZ) * Grid.CellSize;

			const FVector A = Navigation::FromRecast(FVector(X0, Height, Z0));
			const FVector B = Navigation::FromRecast(FVector(X1, Height, Z0));
			const FVector C = Navigation::FromRecast(FVector(X1, Height, Z1));
			const FVector D = Navigation::FromRecast(FVector(X0, Height, Z1));

			AddTriangle(Faces, A, B, C, Color);
			AddTriangle(Faces, A, C, D, Color);
		};

	const auto StateColor = [&](uint16 State) -> const FVector4&
		{
			switch (static_cast<EVoxelDisplayState>(State))
			{
			case EVoxelDisplayState::Walkable: return Walkable;
			case EVoxelDisplayState::Filtered: return Filtered;
			case EVoxelDisplayState::Eroded: return Eroded;
			case EVoxelDisplayState::Blocked: return Blocked;
			}
			return Blocked;
		};

	switch (Mode)
	{
	case ENavMeshDrawMode::RasterizedVoxels:
	case ENavMeshDrawMode::WalkabilityFilter:
	{
		const auto MakeRasterRun = [&](const FRasterSpan& Span, uint16 LengthX)
		{
			EVoxelDisplayState State = Span.bWalkableBeforeFilter
				? EVoxelDisplayState::Walkable : EVoxelDisplayState::Blocked;
			if (Mode == ENavMeshDrawMode::WalkabilityFilter)
			{
				if (Span.bWalkableAfterFilter)
					State = EVoxelDisplayState::Walkable;
				else
					State = Span.bWalkableBeforeFilter ? EVoxelDisplayState::Filtered : EVoxelDisplayState::Blocked;
			}
			return FVoxelDisplayRun{Span.X, Span.Z, LengthX, Span.Top, static_cast<uint16>(State)};
		};
		const auto DrawRasterRectangle = [&](const FVoxelRectangle& Rectangle)
		{
			AddRectangleTop(RasterGrid, Rectangle, StateColor(Rectangle.FirstRun.State));
		};

		// The singleton list has width 1; the run list has width 2 or greater.
		DrawMergedVoxelRows(RasterSingletons, [&](const FRasterSpan& Span) { return MakeRasterRun(Span, 1); },
			DrawRasterRectangle);
		DrawMergedVoxelRows(RasterRuns, [&](const FRasterRun& Run) { return MakeRasterRun(Run.Span, Run.LengthX); },
			DrawRasterRectangle);
		break;
	}
	case ENavMeshDrawMode::AgentRadiusErosion:
	case ENavMeshDrawMode::Regions:
	{
		const bool bDrawRegions = Mode == ENavMeshDrawMode::Regions;
		DrawMergedVoxelRows(CompactRuns, [&](const FCompactRun& Run)
		{
			const uint16 State = bDrawRegions ? Run.Region : static_cast<uint16>(
				Run.bWalkableAfterErosion ? EVoxelDisplayState::Walkable : EVoxelDisplayState::Eroded);
			return FVoxelDisplayRun{Run.StartX, Run.Z, Run.LengthX, Run.Floor, State};
		}, [&](const FVoxelRectangle& Rectangle)
		{
			const uint16 State = Rectangle.FirstRun.State;
			AddRectangleTop(CompactGrid, Rectangle, bDrawRegions ? RegionColor(State) : StateColor(State));
		});
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
