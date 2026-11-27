#include "EnginePCH.h"
#include "LandscapeProxy.h"

#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "Render/Texture2D.h"

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

		if (!Render->GetHeightmap())
		{
			const uint32 Size = static_cast<uint32>(Render->ComponentSizeQuads + 1);
			Render->SetHeightmapTexture(UTexture2D::CreateDynamicTexture(Size, Size, ETextureFormat::R16_UNorm, Collision->CollisionHeightData.GetData()));
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
