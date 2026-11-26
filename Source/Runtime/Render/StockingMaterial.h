#pragma once

#include "Core/Types.h"
#include "Math/EngineMath.h"

class UMaterial;
class UMaterialInstanceDynamic;

struct FStockingMaterialParams
{
	FVector4 FabricColor = FVector4(0.035f, 0.030f, 0.045f, 1.0f);
	FVector4 HemColor = FVector4(0.018f, 0.016f, 0.024f, 1.0f);
	float BaseOpacity = 0.42f;
	float FresnelOpacity = 0.52f;
	float FresnelPower = 2.4f;
	float StretchSensitivity = 2.6f;
	float StretchAlphaDrop = 0.48f;
	float MinOpacity = 0.06f;
	float WeaveScale = 140.0f;
	float WeaveStrength = 0.14f;
	float HemFeatherPower = 0.85f;
	float HemBandStrength = 0.36f;
	float SheenStrength = 0.14f;
	float SpecularIntensity = 0.28f;
	float SpecularPower = 28.0f;
	float WrinkleShadingStrength = 0.65f;
	float TearRimStrength = 0.72f;
	float TearSkinContactStrength = 0.58f;
	float TearRungStrength = 0.85f;
};

// 스타킹 전용 알파 블렌드 머티리얼 그래프 생성 및 파라미터 적용 유틸리티
class FStockingMaterialLibrary
{
public:
	static UMaterial* GetOrCreateStockingMasterMaterial();
	static UMaterialInstanceDynamic* CreateStockingMaterialInstance(const FStockingMaterialParams& Params = FStockingMaterialParams{});
	static void ApplyParameters(UMaterialInstanceDynamic* MaterialInstance, const FStockingMaterialParams& Params);
};
