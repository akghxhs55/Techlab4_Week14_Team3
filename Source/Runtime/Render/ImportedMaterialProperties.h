#pragma once
#include "Math/Vector.h"
#include "Render/RenderEnums.h"

// 임포트된 메시 슬롯과 머티리얼 간 공통 속성 전달 구조체
struct FImportedMaterialProperties
{
	FVector SpecularColor = FVector(1, 1, 1);
	float SpecularFactor = 1.0f;
	FVector EmissiveColor = FVector(0, 0, 0);
	float EmissiveFactor = 1.0f;
	float Shininess = 32.0f;
	ELightingModel ShadingModel = ELightingModel::Phong;
	bool bOverrideShadingModel = false;
	bool bAlphaMasked = false;
};
