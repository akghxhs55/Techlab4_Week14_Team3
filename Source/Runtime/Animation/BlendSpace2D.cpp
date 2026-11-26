#include "EnginePCH.h"
#include "BlendSpace2D.h"
#include "AnimationRuntime.h"
#include "Serialization/MathSerialization.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
	struct FPoint2D { double X; double Y; };
	struct FEdge2D { int32 A; int32 B; };

	double Cross2D(const FPoint2D& A, const FPoint2D& B, const FPoint2D& C)
	{
		return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
	}

	bool ContainsInCircumcircle(const FPoint2D& A, const FPoint2D& B, const FPoint2D& C, const FPoint2D& P)
	{
		const double AX = A.X - P.X;
		const double AY = A.Y - P.Y;
		const double BX = B.X - P.X;
		const double BY = B.Y - P.Y;
		const double CX = C.X - P.X;
		const double CY = C.Y - P.Y;
		const double Determinant = (AX * AX + AY * AY) * (BX * CY - BY * CX)
			- (BX * BX + BY * BY) * (AX * CY - AY * CX)
			+ (CX * CX + CY * CY) * (AX * BY - AY * BX);
		const double Orientation = Cross2D(A, B, C);
		return Orientation > 0.0 ? Determinant > 0.0 : Determinant < 0.0;
	}
}

bool UBlendSpace2D::AddSample(UAnimSequence* Animation, const FVector& SampleValue)
{
	if (!Animation || !std::isfinite(SampleValue.X) || !std::isfinite(SampleValue.Y))
		return false;

	for (const FBlendSample& Sample : BlendSampleData)
	{
		if (Sample.SampleValue.X == SampleValue.X && Sample.SampleValue.Y == SampleValue.Y)
			return false;
	}

	FBlendSample Sample;
	Sample.Animation = Animation;
	Sample.SampleValue = SampleValue;
	BlendSampleData.Add(Sample);
	RebuildTriangles();
	return true;
}

bool UBlendSpace2D::ReplaceSamples(const TArray<FBlendSample>& InSamples)
{
	UBlendSpace2D Replacement;
	for (const FBlendSample& Sample : InSamples)
	{
		if (!Replacement.AddSample(Sample.Animation, Sample.SampleValue))
			return false;
	}
	if (Replacement.BlendSampleData.IsEmpty())
		return false;
	BlendSampleData = std::move(Replacement.BlendSampleData);
	Triangles = std::move(Replacement.Triangles);
	return true;
}

bool UBlendSpace2D::RemoveSample(int32 Index)
{
	if (Index < 0 || Index >= BlendSampleData.Num())
		return false;
	BlendSampleData.RemoveAt(Index);
	RebuildTriangles();
	return true;
}

void UBlendSpace2D::ClearSamples()
{
	BlendSampleData.Empty();
	Triangles.Empty();
}

void UBlendSpace2D::RebuildTriangles()
{
	Triangles.Reset();
	const int32 Count = BlendSampleData.Num();
	if (Count < 3)
		return;

	std::vector<FPoint2D> Points;
	Points.reserve(static_cast<size_t>(Count) + 3);
	double MinX = BlendSampleData[0].SampleValue.X;
	double MaxX = MinX;
	double MinY = BlendSampleData[0].SampleValue.Y;
	double MaxY = MinY;

	for (const FBlendSample& Sample : BlendSampleData)
	{
		Points.push_back({ Sample.SampleValue.X, Sample.SampleValue.Y });
		MinX = std::min(MinX, static_cast<double>(Sample.SampleValue.X));
		MaxX = std::max(MaxX, static_cast<double>(Sample.SampleValue.X));
		MinY = std::min(MinY, static_cast<double>(Sample.SampleValue.Y));
		MaxY = std::max(MaxY, static_cast<double>(Sample.SampleValue.Y));
	}

	const double Span = std::max(MaxX - MinX, MaxY - MinY);
	if (Span <= 0.0)
		return;

	const double CenterX = (MinX + MaxX) * 0.5;
	const double CenterY = (MinY + MaxY) * 0.5;
	Points.push_back({ CenterX - 20.0 * Span, CenterY - Span });
	Points.push_back({ CenterX, CenterY + 20.0 * Span });
	Points.push_back({ CenterX + 20.0 * Span, CenterY - Span });

	std::vector<FBlendTriangle> Working{ { Count, Count + 1, Count + 2 } };
	for (int32 Index = 0; Index < Count; ++Index)
	{
		std::vector<FEdge2D> Boundary;
		std::vector<FBlendTriangle> Remaining;
		for (const FBlendTriangle& Triangle : Working)
		{
			if (!ContainsInCircumcircle(Points[Triangle.A], Points[Triangle.B], Points[Triangle.C], Points[Index]))
			{
				Remaining.push_back(Triangle);
				continue;
			}
			const FEdge2D Edges[3] = {
				{ Triangle.A, Triangle.B },
				{ Triangle.B, Triangle.C },
				{ Triangle.C, Triangle.A }
			};
			for (FEdge2D Edge : Edges)
			{
				if (Edge.A > Edge.B)
					std::swap(Edge.A, Edge.B);
				std::vector<FEdge2D>::iterator FoundIt = std::find_if(Boundary.begin(), Boundary.end(),
					[&](const FEdge2D& Other) { return Other.A == Edge.A && Other.B == Edge.B; });
				if (FoundIt == Boundary.end())
					Boundary.push_back(Edge);
				else
					Boundary.erase(FoundIt);
			}
		}
		for (const FEdge2D& Edge : Boundary)
		{
			if (Cross2D(Points[Edge.A], Points[Edge.B], Points[Index]) != 0.0)
				Remaining.push_back({ Edge.A, Edge.B, Index });
		}
		Working = std::move(Remaining);
	}

	for (const FBlendTriangle& Triangle : Working)
	{
		if (Triangle.A < Count && Triangle.B < Count && Triangle.C < Count)
			Triangles.Add(Triangle);
	}
}

bool UBlendSpace2D::GetBlendSamples(float X, float Y, std::array<FWeightedBlendSample, 3>& Out, int32& OutCount) const
{
	OutCount = 0;
	if (BlendSampleData.IsEmpty() || !std::isfinite(X) || !std::isfinite(Y))
		return false;

	if (BlendSampleData.Num() == 1)
	{
		Out[0] = { &BlendSampleData[0], 1.0f };
		OutCount = 1;
		return true;
	}

	for (const FBlendSample& Sample : BlendSampleData)
	{
		if (Sample.SampleValue.X == X && Sample.SampleValue.Y == Y)
		{
			Out[0] = { &Sample, 1.0f };
			OutCount = 1;
			return true;
		}
	}

	const FPoint2D P{ X, Y };
	for (const FBlendTriangle& Triangle : Triangles)
	{
		const FBlendSample& A = BlendSampleData[Triangle.A];
		const FBlendSample& B = BlendSampleData[Triangle.B];
		const FBlendSample& C = BlendSampleData[Triangle.C];
		const FPoint2D PA{ A.SampleValue.X, A.SampleValue.Y };
		const FPoint2D PB{ B.SampleValue.X, B.SampleValue.Y };
		const FPoint2D PC{ C.SampleValue.X, C.SampleValue.Y };

		const double Denominator = Cross2D(PA, PB, PC);
		if (Denominator == 0.0)
			continue;

		const double WA = Cross2D(PB, PC, P) / Denominator;
		const double WB = Cross2D(PC, PA, P) / Denominator;
		const double WC = 1.0 - WA - WB;
		if (WA < -1e-6 || WB < -1e-6 || WC < -1e-6)
			continue;

		const double ClampedA = std::max(0.0, WA);
		const double ClampedB = std::max(0.0, WB);
		const double ClampedC = std::max(0.0, WC);
		const double Sum = ClampedA + ClampedB + ClampedC;
		if (Sum <= 0.0)
			continue;

		Out[0] = { &A, static_cast<float>(ClampedA / Sum) };
		Out[1] = { &B, static_cast<float>(ClampedB / Sum) };
		Out[2] = { &C, static_cast<float>(ClampedC / Sum) };
		OutCount = 3;
		return true;
	}

	// 삼각형 밖이나 일직선 배치는 가장 가까운 두 샘플 선분으로 투영
	double BestDistance = std::numeric_limits<double>::infinity();
	double BestLength = std::numeric_limits<double>::infinity();
	for (int32 I = 0; I < BlendSampleData.Num(); ++I)
	{
		for (int32 J = I + 1; J < BlendSampleData.Num(); ++J)
		{
			const double DX = static_cast<double>(BlendSampleData[J].SampleValue.X) - BlendSampleData[I].SampleValue.X;
			const double DY = static_cast<double>(BlendSampleData[J].SampleValue.Y) - BlendSampleData[I].SampleValue.Y;
			const double LengthSquared = DX * DX + DY * DY;
			if (LengthSquared <= 0.0)
				continue;

			const double T = std::clamp(
				((X - BlendSampleData[I].SampleValue.X) * DX + (Y - BlendSampleData[I].SampleValue.Y) * DY) / LengthSquared,
				0.0, 1.0);
			const double ErrorX = X - BlendSampleData[I].SampleValue.X - T * DX;
			const double ErrorY = Y - BlendSampleData[I].SampleValue.Y - T * DY;
			const double Distance = ErrorX * ErrorX + ErrorY * ErrorY;
			if (Distance > BestDistance + 1e-9 ||
				(std::abs(Distance - BestDistance) <= 1e-9 && LengthSquared >= BestLength))
			{
				continue;
			}
			BestDistance = Distance;
			BestLength = LengthSquared;
			Out[0] = { &BlendSampleData[I], static_cast<float>(1.0 - T) };
			Out[1] = { &BlendSampleData[J], static_cast<float>(T) };
			OutCount = 2;
		}
	}
	return OutCount > 0;
}

float UBlendSpace2D::GetEffectiveLength(float X, float Y) const
{
	std::array<FWeightedBlendSample, 3> Blend;
	int32 Count = 0;
	if (!GetBlendSamples(X, Y, Blend, Count))
		return 0.0f;

	float Length = 0.0f;
	for (int32 I = 0; I < Count; ++I)
	{
		const UAnimSequence* Sequence = Blend[I].Sample ? Blend[I].Sample->Animation : nullptr;
		if (Sequence && Sequence->GetDataModel())
		{
			Length += Blend[I].Weight * Sequence->GetDataModel()->GetPlayLength();
		}
	}
	return Length;
}

void UBlendSpace2D::EvaluatePose(float X, float Y, float Phase, FPoseContext& Output, bool bExtractRootMotion) const
{
	std::array<FWeightedBlendSample, 3> Blend;
	int32 Count = 0;
	if (!std::isfinite(Phase) || !GetBlendSamples(X, Y, Blend, Count))
	{
		Output.ResetToRefPose();
		return;
	}

	const float ClampedPhase = FMath::Clamp(Phase, 0.0f, 1.0f);
	float AccumulatedWeight = 0.0f;
	FPoseContext BlendedPose(Output);
	FPoseContext TempPose(Output);
	FPoseContext NextPose(Output);

	for (int32 I = 0; I < Count; ++I)
	{
		if (Blend[I].Weight <= 0.0f || !Blend[I].Sample || !Blend[I].Sample->Animation ||
			!Blend[I].Sample->Animation->GetDataModel())
		{
			continue;
		}

		const UAnimSequence* Sequence = Blend[I].Sample->Animation;
		const float Time = ClampedPhase * Sequence->GetDataModel()->GetPlayLength();
		Sequence->GetAnimationPose(Time, TempPose.Pose, Output.GetRefSkeleton(), bExtractRootMotion);

		if (AccumulatedWeight == 0.0f)
		{
			BlendedPose.Pose = TempPose.Pose;
		}
		else
		{
			const float Alpha = Blend[I].Weight / (AccumulatedWeight + Blend[I].Weight);
			FAnimationRuntime::BlendTwoPosesTogether(
				BlendedPose.Pose, TempPose.Pose,
				BlendedPose.Curve, TempPose.Curve,
				1.0f - Alpha, NextPose.Pose, NextPose.Curve);
			BlendedPose.Pose = NextPose.Pose;
		}
		AccumulatedWeight += Blend[I].Weight;
	}

	if (AccumulatedWeight > 0.0f)
	{
		Output.Pose = BlendedPose.Pose;
	}
	else
	{
		Output.ResetToRefPose();
	}
}

void UBlendSpace2D::Serialize(FStructuredArchive::FRecord Record)
{
	Super::Serialize(Record);

	FArchive& Ar = Record.GetUnderlyingArchive();
	int32 Version = Ar.IsSaving() ? 1 : 0;
	int32 Dimension = 2;
	Record << SA_VALUE("Version", Version);
	if (Version != 1)
	{
		Ar.SetError();
		return;
	}
	Record << SA_VALUE("Dimension", Dimension);

	FString PreviewMeshPath = Ar.IsSaving() ? GetPreviewMeshPath() : FString{};
	Record << SA_VALUE("PreviewMesh", PreviewMeshPath);
	if (Ar.IsLoading())
		SetPreviewMeshPath(std::move(PreviewMeshPath));

	int32 Count = Ar.IsSaving() ? BlendSampleData.Num() : 0;
	FStructuredArchive::FArray Samples = Record.EnterArray("Samples", Count);

	if (Ar.IsLoading())
	{
		BlendSampleData.SetNum(Count);
	}

	for (int32 Index = 0; Index < Count; ++Index)
	{
		FStructuredArchive::FRecord SampleRecord = Samples.EnterElement().EnterRecord();
		FBlendSample& Sample = BlendSampleData[Index];
		UObject* Animation = Ar.IsLoading() ? nullptr : Sample.Animation;

		SampleRecord << SA_VALUE("Animation", Animation);
		SampleRecord << SA_VALUE("SampleValue", Sample.SampleValue);

		if (Ar.IsLoading())
		{
			Sample.Animation = static_cast<UAnimSequence*>(Animation);
		}
	}

	if (Ar.IsLoading())
	{
		RebuildTriangles();
	}
}
