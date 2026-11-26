#pragma once

#include "BlendSpace1D.h"
#include "Animation/BonePose.h"
#include "Animation/AnimNodeBase.h"
#include <array>
#include "BlendSpace2D.generated.h"

struct FWeightedBlendSample
{
	const FBlendSample* Sample = nullptr;
	float Weight = 0.0f;
};

struct FBlendTriangle
{
	int32 A = 0;
	int32 B = 0;
	int32 C = 0;
};

UCLASS()
class UBlendSpace2D : public UAnimationAsset
{
	GENERATED_BODY()

public:
	bool AddSample(UAnimSequence* Animation, const FVector& SampleValue);
	bool ReplaceSamples(const TArray<FBlendSample>& InSamples);
	bool RemoveSample(int32 Index);
	void ClearSamples();

	const TArray<FBlendSample>& GetSampleData() const { return BlendSampleData; }
	const TArray<FBlendTriangle>& GetTriangles() const { return Triangles; }

	bool GetBlendSamples(float X, float Y, std::array<FWeightedBlendSample, 3>& Out, int32& OutCount) const;
	float GetEffectiveLength(float X, float Y) const;
	void EvaluatePose(float X, float Y, float Phase, FPoseContext& Output, bool bExtractRootMotion) const;

	using Super::Serialize;
	void Serialize(FStructuredArchive::FRecord Record) override;

private:
	void RebuildTriangles();

	TArray<FBlendSample> BlendSampleData;
	TArray<FBlendTriangle> Triangles;
};
