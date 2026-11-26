#include "EnginePCH.h"
#include "AnimNotifyState.h"
#include "AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "AnimSequenceBase.h"
#include "Components/ParticleSystemComponent.h"
#include "Particles/ParticleSystem.h"
#include "Asset/AssetManager.h"
#include "GameFramework/Actor.h"
#include "Math/MathUtility.h"
#include <cmath>

// UAnimNotify::~UAnimNotify와 같은 이유로 시퀀스에 알린다
UAnimNotifyState::~UAnimNotifyState()
{
	if (UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(GetOuter())) Sequence->OnNotifyObjectDestroyed(this);
}

namespace
{
	UAnimInstance* AnimInstanceOf(USkeletalMeshComponent* MeshComp)
	{
		return MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	}
}

void UAnimNotifyState_Named::NotifyBegin(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	if (UAnimInstance* Instance = AnimInstanceOf(MeshComp))
		Instance->HandleNamedNotifyStateBegin(FName(NotifyName), TotalDuration);
}

void UAnimNotifyState_Named::NotifyTick(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	if (UAnimInstance* Instance = AnimInstanceOf(MeshComp))
		Instance->HandleNamedNotifyStateTick(FName(NotifyName), FrameDeltaTime);
}

void UAnimNotifyState_Named::NotifyEnd(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	if (UAnimInstance* Instance = AnimInstanceOf(MeshComp))
		Instance->HandleNamedNotifyStateEnd(FName(NotifyName));
}

namespace
{
	FRotator ExtractRotationFromMatrix(const FMatrix& Matrix)
	{
		FMatrix RotMat = FMatrix::Identity;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			FVector Row(Matrix.M[Axis][0], Matrix.M[Axis][1], Matrix.M[Axis][2]);
			const float LenSq = Row.Dot(Row);
			if (LenSq > 1e-8f)
			{
				Row = Row / std::sqrt(LenSq);
			}
			RotMat.M[Axis][0] = Row.X;
			RotMat.M[Axis][1] = Row.Y;
			RotMat.M[Axis][2] = Row.Z;
		}
		const float Sy = std::sqrt(RotMat.M[0][0] * RotMat.M[0][0] + RotMat.M[0][1] * RotMat.M[0][1]);
		float Pitch = 0.0f;
		float Yaw = 0.0f;
		float Roll = 0.0f;
		if (Sy > 1e-6f)
		{
			Yaw = FMath::RadiansToDegrees(std::atan2(RotMat.M[0][1], RotMat.M[0][0]));
			Pitch = FMath::RadiansToDegrees(std::atan2(-RotMat.M[0][2], Sy));
			Roll = FMath::RadiansToDegrees(std::atan2(RotMat.M[1][2], RotMat.M[2][2]));
		}
		else
		{
			Yaw = FMath::RadiansToDegrees(std::atan2(-RotMat.M[1][0], RotMat.M[1][1]));
			Pitch = FMath::RadiansToDegrees(std::atan2(-RotMat.M[0][2], Sy));
			Roll = 0.0f;
		}
		return FRotator(Pitch, Yaw, Roll);
	}

	void SyncNotifyParticleToBone(USkeletalMeshComponent* Mesh, UParticleSystemComponent* ParticleComp,
		const FString& BoneName, const FString& SecondBoneName,
		const FString& BaseParamName, const FString& TipParamName,
		const FVector& BoneOffset, bool bFollowBoneRotation)
	{
		if (!Mesh || !ParticleComp || !Mesh->GetSkeletalMesh())
		{
			return;
		}
		if (BoneName.empty())
		{
			ParticleComp->SetWorldLocationAndRotation(Mesh->GetWorldLocation(), Mesh->GetWorldRotation(), false);
			return;
		}

		if (!Mesh->DoesSocketExist(BoneName))
		{
			return;
		}

		const FMatrix BoneWorld = Mesh->GetSocketWorldMatrix(BoneName);
		const FVector BonePos = BoneWorld.TransformPosition(BoneOffset);
		if (!SecondBoneName.empty() && Mesh->DoesSocketExist(SecondBoneName))
		{
			const FVector SecondPos = Mesh->GetSocketWorldMatrix(SecondBoneName).TransformPosition(FVector::ZeroVector);
			const FVector MidPos = (BonePos + SecondPos) * 0.5f;
			ParticleComp->SetVectorParameter(BaseParamName.empty() ? FString("RibbonBase") : BaseParamName, BonePos);
			ParticleComp->SetVectorParameter(TipParamName.empty() ? FString("RibbonTip") : TipParamName, SecondPos);
			if (bFollowBoneRotation)
			{
				ParticleComp->SetWorldLocationAndRotation(MidPos, ExtractRotationFromMatrix(BoneWorld), false);
			}
			else if (Mesh->GetOwner())
			{
				ParticleComp->SetWorldLocationAndRotation(MidPos, Mesh->GetOwner()->GetActorRotation(), false);
			}
			else
			{
				ParticleComp->SetWorldLocation(MidPos, false);
			}
			return;
		}

		if (bFollowBoneRotation)
		{
			ParticleComp->SetWorldLocationAndRotation(BonePos, ExtractRotationFromMatrix(BoneWorld), false);
		}
		else if (Mesh->GetOwner())
		{
			FRotator TargetRot = Mesh->GetOwner()->GetActorRotation();
			TargetRot.Roll = ExtractRotationFromMatrix(BoneWorld).Roll;
			ParticleComp->SetWorldLocationAndRotation(BonePos, TargetRot, false);
		}
		else
		{
			ParticleComp->SetWorldLocation(BonePos, false);
		}
	}
}

void UAnimNotifyState_ParticleEffect::NotifyBegin(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	if (!MeshComp || ParticlePath.empty())
	{
		return;
	}
	AActor* Owner = MeshComp->GetOwner();
	if (!Owner)
	{
		return;
	}

	UParticleSystem* LoadedTemplate = UAssetManager::LoadParticleSystem(ParticlePath);
	if (!LoadedTemplate)
	{
		// 구형 에셋 접두사 보정
		FString NormalizedPath = ParticlePath;
		if (NormalizedPath.rfind("Assets/", 0) == 0)
		{
			NormalizedPath = NormalizedPath.substr(7);
			LoadedTemplate = UAssetManager::LoadParticleSystem(NormalizedPath);
		}
		else if (NormalizedPath.rfind("Content/", 0) == 0)
		{
			NormalizedPath = NormalizedPath.substr(8);
			LoadedTemplate = UAssetManager::LoadParticleSystem(NormalizedPath);
		}
	}
	if (!LoadedTemplate)
	{
		KRS_LOG(Warning, "[AnimNotifyState_ParticleEffect] Failed to load particle '{}'", ParticlePath);
		return;
	}

	UParticleSystemComponent* ParticleComp = nullptr;
	for (UActorComponent* Comp : Owner->GetComponents())
	{
		if (UParticleSystemComponent* Candidate = Cast<UParticleSystemComponent>(Comp))
		{
			if (Candidate->GetTemplate() == LoadedTemplate && !Candidate->IsActive())
			{
				ParticleComp = Candidate;
				break;
			}
		}
	}
	if (!ParticleComp)
	{
		ParticleComp = Cast<UParticleSystemComponent>(Owner->AddComponent(UParticleSystemComponent::StaticClass(), FName("AnimNotifyParticle")));
		if (!ParticleComp)
		{
			return;
		}
		ParticleComp->SetupAttachment(MeshComp);
		ParticleComp->SetTemplate(LoadedTemplate);
	}

	ActiveComponents[MeshComp] = ParticleComp;
	SyncNotifyParticleToBone(MeshComp, ParticleComp, BoneName, SecondBoneName, BaseParameterName, TipParameterName, BoneOffset, bFollowBoneRotation);
	ParticleComp->SetVisibility(true);
	ParticleComp->Activate(true);
}

void UAnimNotifyState_ParticleEffect::NotifyTick(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	if (!MeshComp)
	{
		return;
	}
	UParticleSystemComponent** Found = ActiveComponents.Find(MeshComp);
	if (!Found || !IsValid(*Found))
	{
		return;
	}

	UParticleSystemComponent* ParticleComp = *Found;
	SyncNotifyParticleToBone(MeshComp, ParticleComp, BoneName, SecondBoneName, BaseParameterName, TipParameterName, BoneOffset, bFollowBoneRotation);
	if (bLoopDuringState && !ParticleComp->IsActive())
	{
		ParticleComp->Activate(true);
	}
}

void UAnimNotifyState_ParticleEffect::NotifyEnd(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	if (!MeshComp)
	{
		return;
	}
	UParticleSystemComponent** Found = ActiveComponents.Find(MeshComp);
	UParticleSystemComponent* ParticleComp = Found ? *Found : nullptr;
	ActiveComponents.Remove(MeshComp);
	if (!IsValid(ParticleComp))
	{
		return;
	}
	ParticleComp->Deactivate();
}
