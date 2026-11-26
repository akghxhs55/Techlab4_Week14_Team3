#pragma once

#include "VectorRegister.h"

using FQuatRegister = FVectorRegister;

//template<typename T>
//struct TVector;
struct FVector;
struct FMatrix;
struct FRotator;

struct FQuat
{
	union
	{
		float V[4];
		struct {
			float X;
			float Y;
			float Z;
			float W;
		};
	};

	FQuat();
	FQuat(float InX, float InY, float InZ, float InW = 1.0f);
	explicit FQuat(FRotator InRotator);
	// UE FQuat(const FMatrix&): 스케일이 없는 회전 행렬에서 직접 변환한다 (오일러를 거치지 않음)
	explicit FQuat(const FMatrix& M);

	/* Function */
	FQuat operator * (const FQuat& Other) const;
	FQuat Conjugate() const;
	FQuat UnitInverse() const;
	FQuat Inverse() const;
	FQuat Normalized() const;
	FVector RotateVector(const FVector& V) const;
	FMatrix ToFMatrix() const;
	FRotator ToFRotator() const;

	FVector GetForwardVector() const;
	FVector GetRightVector() const;
	FVector GetUpVector() const;


	/* Static */
	static FQuat Identity();
	static FQuat MakeFromAxisAngle(const FVector& Axis, float AngleRadians);
	static FQuat MakeFromEuler(float Roll, float Pitch, float Yaw);
};


