#pragma once

#include "Core/EngineString.h"
#include "Containers/Array.h"

struct FAnimMontageSegment
{
	FString SequencePath;
	float StartTime = 0.0f;
	float EndTime = 0.0f;
	float AnimStartOffset = 0.0f;
	float PlayRate = 1.0f;
};

struct FAnimMontageSection
{
	FString Name;
	float StartTime = 0.0f;
	float EndTime = 0.0f;
	FString NextSection;
};

struct FAnimMontageNotify
{
	FString Name;
	float Time = 0.0f;
	float Duration = 0.0f;
	FString ClassName;
	FString Properties;
};

struct FAnimMontageAsset
{
	FString PreviewMeshPath;
	FString SequencePath;
	FString SlotName = "UpperBody";
	float PlayRate = 1.0f;
	float BlendInTime = 0.1f;
	float BlendOutTime = 0.15f;
	bool bLoop = false;
	TArray<FAnimMontageSegment> Segments;
	TArray<FAnimMontageSection> Sections;
	TArray<FAnimMontageNotify> Notifies;

	bool Validate(FString& OutError) const;
	static bool Load(const FString& Path, FAnimMontageAsset& Out, FString& OutError);
	bool Save(const FString& Path, FString& OutError) const;
};

struct FAimOffsetSample
{
	float Pitch = 0.0f;
	FString SequencePath;
	int32 FrameIndex = 0;
};

struct FAimOffsetAsset
{
	FString PreviewMeshPath;
	FString BaseSequencePath;
	int32 BaseFrameIndex = 0;
	float MinPitch = -90.0f;
	float MaxPitch = 90.0f;
	TArray<FAimOffsetSample> Samples;

	bool Validate(FString& OutError) const;
	static bool Load(const FString& Path, FAimOffsetAsset& Out, FString& OutError);
	bool Save(const FString& Path, FString& OutError) const;
};
