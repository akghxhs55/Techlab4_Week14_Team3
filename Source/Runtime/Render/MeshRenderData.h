#pragma once
#include "Containers/Array.h"
#include "Math/Box.h"
#include "Math/Sphere.h"

class UMaterialInterface;

struct FCachedMeshSection
{
	UMaterialInterface* Material = nullptr;
	uint32 StartIndex = 0;
	uint32 IndexCount = 0;
};

struct FCachedMeshLOD
{
	uint32 FirstSection = 0;
	uint32 NumSections = 0;
};

struct FMeshRenderState
{
	TArray<FCachedMeshSection> Sections;
	FCachedMeshLOD LODs[4]{};
	float LODThresholdSq[3]{};
	uint8 LODCount = 1;
};

struct FLODSelectionInput
{
	FLODSphere Sphere;
	const FMeshRenderState* State = nullptr;
};
