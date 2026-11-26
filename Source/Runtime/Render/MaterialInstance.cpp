#include "EnginePCH.h"
#include "MaterialInstance.h"
#include "Material.h"
#include "MaterialRenderProxy.h"
#include "Asset/AssetManager.h"
#include "Serialization/TypeSerializer.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveSlots.h"
#include "Serialization/MathSerialization.h"

UMaterialInstance::UMaterialInstance()
	: SortID(NextSortID++)
{
}

void UMaterialInstance::SetParent(UMaterialInterface* InParent)
{
	for (UMaterialInterface* Current = InParent; Current;)
	{
		if (Current == this)
		{
			return;
		}

		UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
		Current = Instance ? Instance->GetParent() : nullptr;
	}

	Parent = InParent;
	ParameterOverrides.Reset();
}

const FMaterialParameterOverride* UMaterialInstance::FindAnyParameterOverride(const FName& ParameterName) const
{
	for (const FMaterialParameterOverride& Override : ParameterOverrides)
	{
		if (Override.ParameterName == ParameterName)
		{
			return &Override;
		}
	}

	return nullptr;
}

const FMaterialParameterOverride* UMaterialInstance::GetParameterOverride(const FName& ParameterName, EMaterialParameterType Type) const
{
	for (const FMaterialParameterOverride& Override : ParameterOverrides)
	{
		if (Override.ParameterName == ParameterName && Override.Type == Type)
		{
			return &Override;
		}
	}

	return nullptr;
}

bool UMaterialInstance::HasAnyParameterOverride(const FName& ParameterName) const
{
	return FindAnyParameterOverride(ParameterName) != nullptr;
}

void UMaterialInstance::SetScalarParameterValue(const FName& ParameterName, float Value)
{
	for (FMaterialParameterOverride& Override : ParameterOverrides)
	{
		if (Override.ParameterName == ParameterName && Override.Type == EMaterialParameterType::Scalar)
		{
			Override.ScalarValue = Value;
			return;
		}
	}

	FMaterialParameterOverride Override{};
	Override.ParameterName = ParameterName;
	Override.Type = EMaterialParameterType::Scalar;
	Override.ScalarValue = Value;
	ParameterOverrides.Add(Override);
}

void UMaterialInstance::SetVectorParameterValue(const FName& ParameterName, const FVector4& Value)
{
	for (FMaterialParameterOverride& Override : ParameterOverrides)
	{
		if (Override.ParameterName == ParameterName && Override.Type == EMaterialParameterType::Vector)
		{
			Override.VectorValue = Value;
			return;
		}
	}

	FMaterialParameterOverride Override{};
	Override.ParameterName = ParameterName;
	Override.Type = EMaterialParameterType::Vector;
	Override.VectorValue = Value;
	ParameterOverrides.Add(Override);
}

void UMaterialInstance::SetTextureParameterValue(const FName& ParameterName, UTexture2D* Texture)
{
	for (FMaterialParameterOverride& Override : ParameterOverrides)
	{
		if (Override.ParameterName == ParameterName && Override.Type == EMaterialParameterType::Texture)
		{
			Override.TextureValue = Texture;
			return;
		}
	}

	FMaterialParameterOverride Override{};
	Override.ParameterName = ParameterName;
	Override.Type = EMaterialParameterType::Texture;
	Override.TextureValue = Texture;
	ParameterOverrides.Add(Override);
}

void UMaterialInstance::ClearParameterOverrideByName(const FName& ParameterName)
{
	for (int32 Index = ParameterOverrides.Num() - 1; Index >= 0; --Index)
	{
		if (ParameterOverrides[Index].ParameterName == ParameterName)
		{
			ParameterOverrides.RemoveAt(Index);
		}
	}
}

void UMaterialInstance::ClearParameterOverride(const FName& ParameterName, EMaterialParameterType Type)
{
	for (int32 Index = ParameterOverrides.Num() - 1; Index >= 0; --Index)
	{
		if (ParameterOverrides[Index].ParameterName == ParameterName && ParameterOverrides[Index].Type == Type)
		{
			ParameterOverrides.RemoveAt(Index);
		}
	}
}

bool UMaterialInstance::HasParameterOverride(const FName& ParameterName, EMaterialParameterType Type) const
{
	return GetParameterOverride(ParameterName, Type) != nullptr;
}

UMaterial* UMaterialInstance::GetBaseMaterial() const
{
	return Parent ? Parent->GetBaseMaterial() : nullptr;
}

EPSOType UMaterialInstance::GetPSOType() const
{
	return Parent ? Parent->GetPSOType() : EPSOType::StaticMesh_Opaque;
}

EBlendState UMaterialInstance::GetBlendState() const
{
	return Parent ? Parent->GetBlendState() : EBlendState::Opaque;
}

ESamplerState UMaterialInstance::GetSamplerState() const
{
	return Parent ? Parent->GetSamplerState() : ESamplerState::LinearClamp;
}

namespace
{
	void ApplyMaterialParameter(FMaterialRenderProxy& Proxy, const FMaterialParameterOverride& Parameter)
	{
		const FName& Name = Parameter.ParameterName;

		if (Parameter.Type == EMaterialParameterType::Scalar)
		{
			const float Value = Parameter.ScalarValue;

			if (Name == FName("Shininess"))
			{
				Proxy.Shininess = Value;
			}
			else if (Name == FName("SpecularFactor"))
			{
				Proxy.SpecularFactor = Value;
			}
			else if (Name == FName("EmissiveFactor"))
			{
				Proxy.EmissiveFactor = Value;
			}
			else if (Name == FName("FlipNormalY"))
			{
				Proxy.bFlipNormalY = Value >= 0.5f;
			}
			else if (Name == FName("WorldUVLocalSpace"))
			{
				Proxy.bWorldUVLocalSpace = Value >= 0.5f;
			}
		}
		else if (Parameter.Type == EMaterialParameterType::Vector)
		{
			const FVector4& Value = Parameter.VectorValue;

			if (Name == FName("BaseColor"))
			{
				Proxy.BaseColor = Value;
			}
			else if (Name == FName("SpecularColor"))
			{
				Proxy.SpecularColor = FVector(Value.X, Value.Y, Value.Z);
			}
			else if (Name == FName("EmissiveColor"))
			{
				Proxy.EmissiveColor = FVector(Value.X, Value.Y, Value.Z);
			}
			else if (Name == FName("UVScrollSpeed"))
			{
				Proxy.UVScrollSpeed = FVector2(Value.X, Value.Y);
			}
			else if (Name == FName("UVTiling"))
			{
				Proxy.UVTiling = FVector2(Value.X, Value.Y);
			}
			else if (Name == FName("WorldUVTileSize"))
			{
				Proxy.WorldUVTileSize = FVector2(Value.X, Value.Y);
			}
		}
		else if (Parameter.Type == EMaterialParameterType::Texture)
		{
			int32 TextureIndex = -1;

			if (Name == FName("Texture0"))
			{
				TextureIndex = 0;
			}
			else if (Name == FName("Texture1"))
			{
				TextureIndex = 1;
			}

			if (TextureIndex >= 0)
			{
				while (Proxy.Textures.Num() <= TextureIndex)
				{
					Proxy.Textures.Add(nullptr);
				}

				Proxy.Textures[TextureIndex] = Parameter.TextureValue;
			}
		}
	}
}

void UMaterialInstance::GetRenderProxy(FMaterialRenderProxy& OutProxy) const
{
	if (!Parent)
	{
		OutProxy = FMaterialRenderProxy{};
		return;
	}

	Parent->GetRenderProxy(OutProxy);

	for (const FMaterialParameterOverride& Parameter : ParameterOverrides)
	{
		bool bReplaced = false;

		for (FMaterialParameterOverride& Existing : OutProxy.ParameterOverrides)
		{
			if (Existing.ParameterName == Parameter.ParameterName && Existing.Type == Parameter.Type)
			{
				Existing = Parameter;
				bReplaced = true;
				break;
			}
		}

		if (!bReplaced)
		{
			OutProxy.ParameterOverrides.Add(Parameter);
		}

		ApplyMaterialParameter(OutProxy, Parameter);
	}

	if (!OutProxy.BaseMaterial)
	{
		return;
	}

	for (int32 Index = 0; Index < OutProxy.GraphTextureNodeIds.Num(); ++Index)
	{
		const int32 NodeId = OutProxy.GraphTextureNodeIds[Index];

		for (const FMaterialExpressionNode& Node : OutProxy.BaseMaterial->ExpressionNodes)
		{
			if (Node.Id != NodeId || Node.Type != EMaterialExpressionType::TextureSampleParameter2D)
			{
				continue;
			}

			const FMaterialParameterOverride* Override = nullptr;

			for (const FMaterialParameterOverride& Candidate : OutProxy.ParameterOverrides)
			{
				if (Candidate.ParameterName == FName(Node.ParameterName) && Candidate.Type == EMaterialParameterType::Texture)
				{
					Override = &Candidate;
					break;
				}
			}

			if (Override && OutProxy.GraphTextures.IsValidIndex(Index))
			{
				OutProxy.GraphTextures[Index] = Override->TextureValue;
			}

			break;
		}
	}
}

void UMaterialInstance::Serialize(FStructuredArchive::FRecord Record)
{
	Super::Serialize(Record);
	FArchive& Ar = Record.GetUnderlyingArchive();

	FString ParentPath;
	if (Ar.IsSaving() && Parent)
	{
		ParentPath = Parent->GetPath();
	}

	if (TOptional<FStructuredArchive::FSlot> ParentSlot = Record.TryEnterField("Parent", Ar.IsSaving()))
	{
		ParentSlot.GetValue() << ParentPath;
	}

	if (Ar.IsLoading())
	{
		if (ParentPath.empty())
		{
			Parent = UAssetManager::GetAssetByPath<UMaterial>("DefaultMaterial");
		}
		else
		{
			Parent = UAssetManager::LoadMaterialAsset(ParentPath);
			if (!Parent)
			{
				Parent = UAssetManager::GetAssetByPath<UMaterialInterface>(ParentPath);
			}
		}
	}

	int32 OverrideCount = ParameterOverrides.Num();
	if (TOptional<FStructuredArchive::FSlot> OverrideSlot = Record.TryEnterField("ParameterOverrides", Ar.IsSaving()))
	{
		FStructuredArchive::FArray OverrideArray = OverrideSlot.GetValue().EnterArray(OverrideCount);

		if (Ar.IsLoading())
		{
			ParameterOverrides.Reset();
		}

		for (int32 Index = 0; Index < OverrideCount; ++Index)
		{
			FMaterialParameterOverride Parameter = Ar.IsSaving() ? ParameterOverrides[Index] : FMaterialParameterOverride{};
			FString NameString = Parameter.ParameterName.ToString();
			uint8 Type = static_cast<uint8>(Parameter.Type);
			FString TexturePath = (Ar.IsSaving() && Parameter.TextureValue) ? Parameter.TextureValue->GetPath() : FString();

			FStructuredArchive::FRecord ParameterRecord = OverrideArray.EnterElement().EnterRecord();
			ParameterRecord << SA_VALUE("Name", NameString);
			ParameterRecord << SA_VALUE("Type", Type);
			ParameterRecord << SA_VALUE("ScalarValue", Parameter.ScalarValue);
			ParameterRecord << SA_VALUE("VectorValue", Parameter.VectorValue);
			ParameterRecord << SA_VALUE("TexturePath", TexturePath);

			if (Ar.IsLoading())
			{
				Parameter.ParameterName = FName(NameString);
				Parameter.Type = static_cast<EMaterialParameterType>(Type);
				Parameter.TextureValue = TexturePath.empty() ? nullptr : UAssetManager::Get().LoadTexture(TexturePath);
				ParameterOverrides.Add(Parameter);
			}
		}
	}
}
