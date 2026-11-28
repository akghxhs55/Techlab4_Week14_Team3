#include "EnginePCH.h"
#include "LandscapeProxy.h"

#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeRender.h"
#include "Render/Texture2D.h"
#include <algorithm>

ULandscapeInfo* ALandscapeProxy::CreateLandscapeInfo()
{
	if (LandscapeInfo)
	{
		return LandscapeInfo;
	}

	LandscapeInfo = NewObject<ULandscapeInfo>(this);
	LandscapeInfo->LandscapeActor = GetLandscapeActor();
	LandscapeInfo->ComponentSizeQuads = ComponentSizeQuads;
	return LandscapeInfo;
}

void ALandscapeProxy::Import(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, const TArray<uint16>& HeightData)
{
	assert(MinX % ComponentSizeQuads == 0 && MinY % ComponentSizeQuads == 0 && MaxX % ComponentSizeQuads == 0 && MaxY % ComponentSizeQuads == 0);
	assert(MinX < MaxX && MinY < MaxY);
	assert((MaxX - MinX + 1) * (MaxY - MinY + 1) == HeightData.Num());


	ClearLandscapeComponents();

	const int32 Stride = MaxX - MinX + 1;
	for (int32 x = MinX; x < MaxX; x += ComponentSizeQuads)
	{
		for (int32 y = MinY; y < MaxY; y += ComponentSizeQuads)
		{
			const FVector Offset{ static_cast<float>(x) , static_cast<float>(y), 0.0f };

			auto* RenderComponent = Cast<ULandscapeComponent>(AddComponent(ULandscapeComponent::StaticClass(), std::format("LandscapeComponent_{}_{}", x / ComponentSizeQuads, y / ComponentSizeQuads)));
			RenderComponent->SectionBaseX = x;
			RenderComponent->SectionBaseY = y;
			RenderComponent->ComponentSizeQuads = ComponentSizeQuads;
			RenderComponent->SetRelativeLocation(Offset);

			auto* CollisionComponent = Cast<ULandscapeHeightfieldCollisionComponent>(AddComponent(ULandscapeHeightfieldCollisionComponent::StaticClass(), std::format("LandscapeHeightfieldCollisionComponent_{}_{}", x / ComponentSizeQuads, y / ComponentSizeQuads)));
			CollisionComponent->SectionBaseX = x;
			CollisionComponent->SectionBaseY = y;
			CollisionComponent->CollisionSizeQuads = ComponentSizeQuads;
			CollisionComponent->CollisionHeightData.SetNum((ComponentSizeQuads + 1) * (ComponentSizeQuads + 1));
			CollisionComponent->SetRelativeLocation(Offset);
			for (int32 i = 0; i <= ComponentSizeQuads; ++i)
			{
				for (int32 j = 0; j <= ComponentSizeQuads; ++j)
				{
					CollisionComponent->CollisionHeightData[i * (ComponentSizeQuads + 1) + j] = HeightData[(y - MinY + i) * Stride + (x - MinX + j)];
				}
			}
		}
	}

	UnregisterAllComponents();
	RebuildComponentReferences();
	RegisterAllComponents();
}

void ALandscapeProxy::Serialize(FStructuredArchive::FRecord Record)
{
	FArchive& Ar = Record.GetUnderlyingArchive();
	const bool bLoadingFromFile = Ar.IsLoading() && !Ar.HasAnyPortFlags(EPropertyPortFlags::PPF_Duplicate);

	if (bLoadingFromFile)
	{
		DestroyLandscapeInfo();
	}

	Super::Serialize(Record);

	if (bLoadingFromFile)
	{
		UnregisterAllComponents();
		RebuildComponentReferences();
		RegisterAllComponents();
	}
}

void ALandscapeProxy::RegisterAllComponents()
{
	CreateLandscapeInfo();
	BuildDerivedData();
	Super::RegisterAllComponents();
}

void ALandscapeProxy::UnregisterAllComponents()
{
	Super::UnregisterAllComponents();
	DestroyLandscapeInfo();
}

void ALandscapeProxy::DestroyLandscapeInfo()
{
	delete LandscapeInfo;
	LandscapeInfo = nullptr;
}

void ALandscapeProxy::RebuildComponentReferences()
{
	LandscapeComponents.Empty();
	CollisionComponents.Empty();

	TMap<FIntPoint, ULandscapeComponent*> RenderComponents;
	for (auto* Component : GetComponents())
	{
		if (auto* LandscapeComponent = Cast<ULandscapeComponent>(Component))
		{
			LandscapeComponents.Add(LandscapeComponent);
			RenderComponents.Add(LandscapeComponent->GetSectionBase(), LandscapeComponent);
		}
		else if (auto* CollisionComponent = Cast<ULandscapeHeightfieldCollisionComponent>(Component))
		{
			CollisionComponents.Add(CollisionComponent);
		}
	}

	for (auto* Collision : CollisionComponents)
	{
		ULandscapeComponent* Render = RenderComponents.FindRef({ Collision->SectionBaseX, Collision->SectionBaseY });
		Collision->SetRenderComponent(Render);
		if (Render)
		{
			Render->SetCollisionComponent(Collision);
		}
	}
}

void ALandscapeProxy::BuildDerivedData()
{
	bool bNeedsHeightmap = false;
	FIntPoint GridMin{ INT32_MAX, INT32_MAX };
	FIntPoint GridMax{ INT32_MIN, INT32_MIN };
	for (auto* Collision : CollisionComponents)
	{
		if (!Collision || Collision->CollisionHeightData.IsEmpty())
		{
			continue;
		}
		GridMin.X = FMath::Min(GridMin.X, Collision->SectionBaseX);
		GridMin.Y = FMath::Min(GridMin.Y, Collision->SectionBaseY);
		GridMax.X = FMath::Max(GridMax.X, Collision->SectionBaseX + Collision->CollisionSizeQuads);
		GridMax.Y = FMath::Max(GridMax.Y, Collision->SectionBaseY + Collision->CollisionSizeQuads);

		ULandscapeComponent* Render = Collision->GetRenderComponent();
		bNeedsHeightmap |= (Render && !Render->GetHeightmap());
	}

	TArray<uint16> GridHeights;
	const int32 GridSizeX = bNeedsHeightmap ? GridMax.X - GridMin.X + 1 : 0;
	const int32 GridSizeY = bNeedsHeightmap ? GridMax.Y - GridMin.Y + 1 : 0;
	if (bNeedsHeightmap)
	{
		GridHeights.SetNum(GridSizeX * GridSizeY);
		std::ranges::fill(GridHeights, static_cast<uint16>(LandscapeDataAccess::MidValue));

		for (auto* Collision : CollisionComponents)
		{
			if (!Collision || Collision->CollisionHeightData.IsEmpty())
			{
				continue;
			}
			const int32 Vertices = Collision->CollisionSizeQuads + 1;
			const int32 OffsetX = Collision->SectionBaseX - GridMin.X;
			const int32 OffsetY = Collision->SectionBaseY - GridMin.Y;
			for (int32 Y = 0; Y < Vertices; ++Y)
			{
				for (int32 X = 0; X < Vertices; ++X)
				{
					GridHeights[(OffsetY + Y) * GridSizeX + (OffsetX + X)] = Collision->CollisionHeightData[Y * Vertices + X];
				}
			}
		}
	}

	for (auto* Render : LandscapeComponents)
	{
		ULandscapeHeightfieldCollisionComponent* Collision = Render ? Render->GetCollisionComponent() : nullptr;
		if (!Collision || Collision->CollisionHeightData.IsEmpty())
		{
			continue;
		}

		FBox LocalBox{
			.Min = FVector{ 0.0f, 0.0f, FLT_MAX },
			.Max = FVector{ static_cast<float>(Render->ComponentSizeQuads), static_cast<float>(Render->ComponentSizeQuads), -FLT_MAX }
		};
		for (uint16 Height : Collision->CollisionHeightData)
		{
			LocalBox.Min.Z = FMath::Min(LocalBox.Min.Z, LandscapeDataAccess::GetLocalHeight(Height));
			LocalBox.Max.Z = FMath::Max(LocalBox.Max.Z, LandscapeDataAccess::GetLocalHeight(Height));
		}
		Render->CachedLocalBox = LocalBox;
		Collision->CachedLocalBox = LocalBox;

		if (!Render->GetHeightmap() && !GridHeights.IsEmpty())
		{
			Render->SetHeightmapTexture(LandscapeRender::CreateHeightmapTexture(
				GridHeights, GridSizeX, GridSizeY,
				Render->SectionBaseX - GridMin.X, Render->SectionBaseY - GridMin.Y,
				Render->ComponentSizeQuads + 1));
		}
	}
}

void ALandscapeProxy::ClearLandscapeComponents()
{
	DestroyLandscapeInfo();

	const TArray<UActorComponent*> Snapshot = GetComponents();
	for (auto* Component : Snapshot)
	{
		if (Cast<ULandscapeComponent>(Component) || Cast<ULandscapeHeightfieldCollisionComponent>(Component))
		{
			DestroyComponent(Component);
		}
	}

	LandscapeComponents.Empty();
	CollisionComponents.Empty();
}
