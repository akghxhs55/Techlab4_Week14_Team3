#include "EnginePCh.h"
#include "Navigation/NavigationSystem.h"
#include "Navigation/NavGeometry.h"
#include "Navigation/RecastNavMesh.h"

#include "Components/CapsuleComponent.h"
#include "Components/CharacterMovementComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor/MyCharacter.h"
#include "GameFramework/Pawn.h"
#include "Math/EngineMath.h"
#include "Render/Mesh.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace
{
	bool ReportNavBuildFailure(const char* Stage)
	{
		KRS_LOG(Warning, "Build Navigation failed at {}", Stage);
		return false;
	}

	bool IsAttachedToPawn(const USceneComponent& Component)
	{
		for (const USceneComponent* Node = &Component; Node; Node = Node->GetAttachParent())
		{
			if (const AActor* Owner = Node->GetOwner(); Owner && Owner->IsA<APawn>())
				return true;
		}

		return false;
	}

	bool CollectGeometry(const UWorld& World, Navigation::FRecastGeometry& Out)
	{
		const ULevel* Level = World.GetCurrentLevel();
		if (!Level)
			return ReportNavBuildFailure("current level");

		for (AActor* Actor : Level->GetActors())
		{
			if (!Actor || World.IsPendingDestroy(Actor))
				continue;

			for (UActorComponent* Component : Actor->GetComponents())
			{
				const auto* StaticMesh = Cast<UStaticMeshComponent>(Component);
				if (!StaticMesh || !StaticMesh->IsRegistered() ||
					StaticMesh->GetWorld() != &World ||
					!StaticMesh->bCanEverAffectNavigation ||
					StaticMesh->IsEditorIcon() ||
					StaticMesh->IsSimulatingPhysics() ||
					IsAttachedToPawn(*StaticMesh))
				{
					continue;
				}

				const UStaticMesh* Mesh = StaticMesh->GetStaticMesh();
				if (!Mesh)
					continue;

				// LOD 0만 사용한다.
				const auto Geometry = Navigation::ConvertStaticMesh(
					Mesh->GetLODMeshData(0), StaticMesh->GetWorldMatrix());

				constexpr int32 MaxCount = (std::numeric_limits<int32>::max)();
				if (Geometry.Vertices.Num() > MaxCount - Out.Vertices.Num() ||
					Geometry.Triangles.Num() > MaxCount - Out.Triangles.Num())
				{
					return ReportNavBuildFailure("geometry size");
				}

				const int32 BaseVertex = Out.Vertices.Num() / 3;
				Out.Vertices.Append(Geometry.Vertices);
				for (int32 Index : Geometry.Triangles)
				{
					Out.Triangles.Add(BaseVertex + Index);
				}
			}
		}
		return !Out.Triangles.IsEmpty() || ReportNavBuildFailure("empty geometry");
	}

	FNavAgentProperties ReadBuildAgent(const ULevel& Level)
	{
		const AMyCharacter* Character = nullptr;
		for (AActor* Actor : Level.GetActors())
		{
			const auto* Candidate = Cast<AMyCharacter>(Actor);
			if (Candidate && Candidate->GetCapsuleComponent() &&
				Candidate->GetCharacterMovement())
			{
				Character = Candidate;
				break;
			}
		}

		if (!Character)
		{
			Character = AMyCharacter::StaticClass()->GetDefaultObject<AMyCharacter>();
			KRS_LOG(Info, "Build Navigation: using AMyCharacter agent");
		}

		const auto* Capsule = Character->GetCapsuleComponent();
		const auto* Movement = Character->GetCharacterMovement();

		FNavAgentProperties Agent;
		Agent.AgentRadius = Capsule->GetScaledCapsuleRadius();
		Agent.AgentHeight = 2.0f * Capsule->GetScaledCapsuleHalfHeight();
		Agent.MaxStepHeight = Movement->MaxStepHeight * CmToWorld;
		return Agent;
	}

	float ToVoxelUnits(float Distance, float CellSize)
	{
		const float Cells = Distance / CellSize;
		const float Nearest = std::round(Cells);

		// 0.45cm / 0.025m 처럼 정수 경계에 있는 설정값의 반올림 오차를 제거한다.
		return std::abs(Cells - Nearest) < 1e-5f ? Nearest : Cells;
	}

	bool BuildTile(const Navigation::FRecastGeometry& Geometry,
		const FNavAgentProperties& Agent,
		const FNavBuildSettings& Settings,
		TArray<uint8>& OutBytes,
		FNavMeshBuildDebugData* Debug = nullptr)
	{
		/* 1. 입력 데이터 처리 */
		// Recast 빌드 설정을 구성하고, 
		// 에이전트의 높이, 반지름, 단차를 복셀 단위로 바꾼다.
		rcContext Context(false);
		rcConfig Config{};
		Config.cs = Settings.CellSize;
		Config.ch = Settings.CellHeight;
		Config.walkableSlopeAngle = Agent.WalkbleFloorAngle;

		const float Height = std::ceil(ToVoxelUnits(Agent.AgentHeight, Config.ch));
		const float Radius = std::ceil(ToVoxelUnits(Agent.AgentRadius, Config.cs));
		const float Climb = std::floor(ToVoxelUnits(Agent.MaxStepHeight, Config.ch));
		if (!(Height >= 3 && Height <= 255 && Radius >= 0 && Radius <= 127 &&
			Climb >= 0 && Climb <= 255))
		{
			return ReportNavBuildFailure("agent voxel dimensions");
		}

		Config.walkableHeight = static_cast<int>(Height);
		Config.walkableRadius = static_cast<int>(Radius);
		Config.walkableClimb = static_cast<int>(Climb);
		Config.maxEdgeLen = static_cast<int>(12.0f / Config.cs);
		Config.maxSimplificationError = 1.4f;
		Config.minRegionArea = 8 * 8;
		Config.mergeRegionArea = 20 * 20;
		Config.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
		Config.detailSampleDist = 6.0f * Config.cs;
		Config.detailSampleMaxError = Config.ch;

		// 입력 지오메트리의 경계에 높이 여유를 더하고,
		// 복셀 격자 크기를 검증한다.
		const float* Vertices = Geometry.Vertices.GetData();
		const int VertexCount = Geometry.Vertices.Num() / 3;
		const int* Triangles = Geometry.Triangles.GetData();
		const int TriangleCount = Geometry.Triangles.Num() / 3;

		rcCalcBounds(Vertices, VertexCount, Config.bmin, Config.bmax);
		// 바닥 복셀이 잘리지 않도록 아래로 한 셀,
		// 위로는 에이전트 높이와 추가 두 셀만큼 확장한다.
		Config.bmin[1] -= Config.ch;
		Config.bmax[1] += (Config.walkableHeight + 2) * Config.ch;

		const float GridX = (Config.bmax[0] - Config.bmin[0]) / Config.cs;
		const float GridZ = (Config.bmax[2] - Config.bmin[2]) / Config.cs;
		const float GridY = (Config.bmax[1] - Config.bmin[1]) / Config.ch;

		constexpr float MaxGridAxis = 2048.0f;
		if (!(GridX >= 1 && GridX <= MaxGridAxis &&
			GridZ >= 1 && GridZ <= MaxGridAxis &&
			GridY > 0 && GridY < RC_SPAN_MAX_HEIGHT))
		{
			return ReportNavBuildFailure("voxel bounds");
		}

		rcCalcGridSize(Config.bmin, Config.bmax, Config.cs,
			&Config.width, &Config.height);

		/* 2. 지오메트리 복셀화 */
		// 경사 기준으로 보행 가능 면을 표시하고,
		// 장애물 면도 포함해 모든 삼각형을 복셀화한다.
		std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)>
			Solid(rcAllocHeightfield(), &rcFreeHeightField);

		if (!Solid || !rcCreateHeightfield(
			&Context, *Solid, Config.width, Config.height,
			Config.bmin, Config.bmax, Config.cs, Config.ch))
		{
			return ReportNavBuildFailure("heightfield");
		}

		std::vector<unsigned char> Areas(TriangleCount, RC_NULL_AREA);
		rcMarkWalkableTriangles(&Context,
			Config.walkableSlopeAngle,
			Vertices, VertexCount,
			Triangles, TriangleCount,
			Areas.data());

		if (!rcRasterizeTriangles(&Context,
			Vertices, VertexCount,
			Triangles,
			Areas.data(), TriangleCount,
			*Solid, Config.walkableClimb))
		{
			return ReportNavBuildFailure("rasterization");
		}
		if (Debug) Debug->CaptureBeforeFilter(*Solid);

		/* 3. 보행 불가능 영역 제거 */
		// 단차가 크거나 천장이 낮은 구간을 걸러낸다.
		// 압축 높이장에서는 에이전트 반지름만큼 통행 여유를 확보한다.
		rcFilterLedgeSpans(&Context,
			Config.walkableHeight, Config.walkableClimb, *Solid);
		rcFilterWalkableLowHeightSpans(&Context, Config.walkableHeight, *Solid);
		if (Debug) Debug->CaptureFiltered(*Solid);


		/* 4. 보행 공간 및 연결 관계 생성 */
		std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)>
			Compact(rcAllocCompactHeightfield(), &rcFreeCompactHeightfield);

		if (!Compact || !rcBuildCompactHeightfield(&Context,
			Config.walkableHeight, Config.walkableClimb, *Solid, *Compact))
		{
			return ReportNavBuildFailure("compact heightfield");
		}
		Solid.reset();

		/* 5. 에이전트 반지름에 따른 보행 영역 침식 */
		if (!rcErodeWalkableArea(&Context, Config.walkableRadius, *Compact))
			return ReportNavBuildFailure("agent radius erosion");

		// 거리장을 계산해 남은 보행 영역을 여러 구역으로 나누고,
		// 작은 구역을 정리한다.
		if (!rcBuildDistanceField(&Context, *Compact) ||
			!rcBuildRegions(&Context, *Compact, Config.borderSize,
				Config.minRegionArea, Config.mergeRegionArea))
		{
			if (Debug) Debug->CaptureCompact(*Compact, false);
			return ReportNavBuildFailure("walkable regions");
		}
		if (Debug) Debug->CaptureCompact(*Compact, true);

		/* 6. Region 외곽선 추출 */
		std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)>
			Contours(rcAllocContourSet(), &rcFreeContourSet);
		if (!Contours || !rcBuildContours(&Context, *Compact,
			Config.maxSimplificationError, Config.maxEdgeLen, *Contours))
		{
			return ReportNavBuildFailure("contours");
		}
		if (Debug) Debug->CaptureContours(*Contours);

		/* 7. Navigation Polygon 생성 */
		// Contour 내부를 삼각형으로 분할하고,
		// 인접한 삼각형들을 가능한 범위에서 병합해 볼록 다각형 생성
		std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)>
			Polys(rcAllocPolyMesh(), &rcFreePolyMesh);
		if (!Polys || !rcBuildPolyMesh(&Context, *Contours, Config.maxVertsPerPoly, *Polys) ||
			Polys->npolys == 0 || Polys->nverts >= 0xffff)
		{
			return ReportNavBuildFailure("polygon mesh");
		}
		if (Debug) Debug->CapturePolygonMesh(*Polys);

		/* 8. 표면 높이 정밀화 */
		std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>
			Detail(rcAllocPolyMeshDetail(), &rcFreePolyMeshDetail);
		if (!Detail || !rcBuildPolyMeshDetail(&Context, *Polys, *Compact,
			Config.detailSampleDist, Config.detailSampleMaxError, *Detail))
		{
			return ReportNavBuildFailure("detail mesh");
		}

		// 단일 보행 영역. Detour 기본 필터가 통과할 수 있도록 플래그를 설정한다.
		// 모든 폴리곤에 보행 플래그를 부여하고, 메시를 Detour 타일 바이트로 변환해 OutBytes에 담는다.
		std::fill_n(Polys->areas, Polys->npolys, static_cast<unsigned char>(0));
		std::fill_n(Polys->flags, Polys->npolys, static_cast<unsigned short>(1));

		/* 9. Recast에서 Detour로 데이터 전달 */
		dtNavMeshCreateParams Params{};
		Params.verts = Polys->verts;
		Params.vertCount = Polys->nverts;
		Params.polys = Polys->polys;
		Params.polyAreas = Polys->areas;
		Params.polyFlags = Polys->flags;
		Params.polyCount = Polys->npolys;
		Params.nvp = Polys->nvp;
		Params.detailMeshes = Detail->meshes;
		Params.detailVerts = Detail->verts;
		Params.detailVertsCount = Detail->nverts;
		Params.detailTris = Detail->tris;
		Params.detailTriCount = Detail->ntris;
		Params.walkableHeight = Agent.AgentHeight;
		Params.walkableRadius = Agent.AgentRadius;
		Params.walkableClimb = Agent.MaxStepHeight;
		Params.cs = Config.cs;
		Params.ch = Config.ch;
		Params.buildBvTree = true;
		std::copy_n(Polys->bmin, 3, Params.bmin);
		std::copy_n(Polys->bmax, 3, Params.bmax);


		/* 10. 최종 바이너리 데이터 생성 */
		uint8* RawBytes = nullptr;
		int ByteCount = 0;
		const bool bCreated = dtCreateNavMeshData(&Params, &RawBytes, &ByteCount);
		std::unique_ptr<uint8, decltype(&dtFree)> Bytes(RawBytes, &dtFree);
		if (!bCreated || !Bytes || ByteCount <= 0)
			return ReportNavBuildFailure("Detour tile creation");

		OutBytes.SetNum(ByteCount);
		std::memcpy(OutBytes.GetData(), Bytes.get(), ByteCount);
		return true;
	}

	void BuildDebugGeometry(const dtMeshTile& Tile, FNavMeshDebugGeometry& Out)
	{
		const auto ToWorld = [](const float* P)
			{
				return Navigation::FromRecast(FVector(P[0], P[1], P[2]));
			};

		for (int PolyIndex = 0; PolyIndex < Tile.header->detailMeshCount; ++PolyIndex)
		{
			const dtPoly& Poly = Tile.polys[PolyIndex];
			const dtPolyDetail& Detail = Tile.detailMeshes[PolyIndex];

			for (int TriIndex = 0; TriIndex < Detail.triCount; ++TriIndex)
			{
				const unsigned char* Tri = Tile.detailTris + 4 * (Detail.triBase + TriIndex);
				const float* Positions[3];

				for (int Corner = 0; Corner < 3; ++Corner)
				{
					const int Index = Tri[Corner];
					Positions[Corner] = Index < Poly.vertCount
						? Tile.verts + 3 * Poly.verts[Index]
						: Tile.detailVerts + 3 * (Detail.vertBase + Index - Poly.vertCount);

					Out.FaceVertices.Add(ToWorld(Positions[Corner]));
				}

				for (int Edge = 0; Edge < 3; ++Edge)
				{
					if (!(dtGetDetailTriEdgeFlags(Tri[3], Edge) & DT_DETAIL_EDGE_BOUNDARY))
						continue;

					const float* A = Positions[Edge];
					const float* B = Positions[(Edge + 1) % 3];

					for (int Side = 0; Side < Poly.vertCount; ++Side)
					{
						if (Poly.neis[Side] != 0)
							continue;

						const float* Start = Tile.verts + 3 * Poly.verts[Side];
						const float* End = Tile.verts + 3 * Poly.verts[(Side + 1) % Poly.vertCount];

						constexpr float EdgeToleranceSquared = 1e-6f;
						float T = 0.0f;
						if (dtDistancePtSegSqr2D(A, Start, End, T) <= EdgeToleranceSquared &&
							dtDistancePtSegSqr2D(B, Start, End, T) <= EdgeToleranceSquared)
						{
							Out.BoundaryVertices.Add(ToWorld(A));
							Out.BoundaryVertices.Add(ToWorld(B));
							break;
						}
					}
				}
			}
		}
	}

	ARecastNavMesh* FindPersistentNavData(const UWorld& World)
	{
		const ULevel* Level = World.GetPersistentLevel();
		if (!Level || World.GetCurrentLevel() != Level)
		{
			return nullptr;
		}

		ARecastNavMesh* Result = nullptr;
		for (AActor* Actor : Level->GetActors())
		{
			auto* NavMesh = Cast<ARecastNavMesh>(Actor);
			if (!NavMesh || World.IsPendingDestroy(NavMesh))
				continue;

			if (Result)
			{
				KRS_LOG(Warning, "Navigation: multiplr RecastNavMesh actors; using {}",
					Result->GetName());
				break;
			}

			Result = NavMesh;
		}

		return Result;
	}
}


bool FNavigationSystem::BuildNavigation(UWorld& World, bool bCaptureBuildDebug)
{
	BuildDebugData.reset();

	const ULevel* Level = World.GetCurrentLevel();
	if (!Level || Level != World.GetPersistentLevel())
		return ReportNavBuildFailure("persistent level");

	TUniquePtr<FNavMeshBuildDebugData> Capture;
	if (bCaptureBuildDebug && World.GetWorldType() == EWorldType::Editor)
		Capture = MakeUnique<FNavMeshBuildDebugData>();

	ARecastNavMesh* ExistingNavMesh = FindPersistentNavData(World);
	Navigation::FRecastGeometry Geometry;
	TArray<uint8> TileBytes;

	ARecastNavMesh* InstalledNavMesh = nullptr;
	if (CollectGeometry(World, Geometry) &&
		BuildTile(Geometry, ReadBuildAgent(*Level), {}, TileBytes, Capture.get()))
	{
		InstalledNavMesh = ARecastNavMesh::InstallBakedTile(
			World, ExistingNavMesh, std::move(TileBytes));
	}

	if (!InstalledNavMesh)
	{
		// 실패 후 씬을 저장해도 이전 베이크가 기록되지 않게 한다.
		if (ExistingNavMesh)
			ExistingNavMesh->ClearNavMeshData();

		ResetNavigation();
		BuildDebugData = std::move(Capture);
		return false;
	}

	MainNavData = InstalledNavMesh;
	++NavMeshVersion;
	BuildDebugData = std::move(Capture);

	KRS_LOG(Info, "Build Navigation: {} input triangles, {} detail triangles, version {}",
		Geometry.Triangles.Num() / 3,
		MainNavData->GetDebugGeometry()->FaceVertices.Num() / 3, 
		NavMeshVersion);

	return true;
}

void FNavigationSystem::RestoreNavigation(const UWorld& World)
{
	ARecastNavMesh* NavMesh = FindPersistentNavData(World);
	if (!NavMesh || !NavMesh->InitializeFromStoredTile())
	{
		ResetNavigation();
		return;
	}

	if (MainNavData != NavMesh)
	{
		MainNavData = NavMesh;
		++NavMeshVersion;
		KRS_LOG(Info, "Navigation: restored baked tile on {}", NavMesh->GetName());
	}
}

void FNavigationSystem::UnregisterNavData(const AActor* Actor)
{
	if (Actor && MainNavData == Actor)
		ResetNavigation();
}

void FNavigationSystem::ResetNavigation()
{
	BuildDebugData.reset();

	if (MainNavData)
	{
		MainNavData = nullptr;
		++NavMeshVersion;
	}
}

const FNavMeshDebugGeometry* FNavigationSystem::GetDebugGeometry() const
{
	return MainNavData ? MainNavData->GetDebugGeometry() : nullptr;
}
