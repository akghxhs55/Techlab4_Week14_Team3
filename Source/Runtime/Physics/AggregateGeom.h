#pragma once

#include "AggregateGeom.generated.h"

USTRUCT()
struct FShapeElem
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Center;
	UPROPERTY()
	FRotator Rotation;
};

USTRUCT()
struct FSphereElem : public FShapeElem
{
	GENERATED_BODY()

	UPROPERTY()
	float Radius;
};

USTRUCT()
struct FBoxElem : public FShapeElem
{
	GENERATED_BODY()

	UPROPERTY()
	float X;
	
	UPROPERTY()
	float Y;
	
	UPROPERTY()
	float Z;
};

USTRUCT()
struct FCapsuleElem : public FShapeElem
{
	GENERATED_BODY()

	UPROPERTY()
	float Radius;
	UPROPERTY()
	float Length;
};

USTRUCT()
struct FAggregateGeom
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FSphereElem> SphereElems;
	UPROPERTY()
	TArray<FBoxElem> BoxElems;
	UPROPERTY()
	TArray<FCapsuleElem> CapsuleElems;
	int32 GetElementCount() const { return SphereElems.Num() + BoxElems.Num() + CapsuleElems.Num(); }
	void EmptyElements() {
		SphereElems.Empty();
		BoxElems.Empty();
		CapsuleElems.Empty();
	}
};
