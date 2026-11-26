#pragma once

#include "UObject/Object.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "Containers/Map.h"
#include "AnimNotifyState.generated.h"

class USkeletalMeshComponent;
class UParticleSystemComponent;
class UAnimSequenceBase;
struct FAnimNotifyEventReference;   // const 참조로만 받으므로 전방 선언으로 충분

UCLASS()
class UAnimNotifyState : public UObject
{
	GENERATED_BODY()
public:
	// 소유 시퀀스의 Notify 목록에서 자신을 지운다 (엔진 종료 때 시퀀스가 다시 delete하지 않게)
	virtual ~UAnimNotifyState() override;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference){}
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference){}
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference){}
};

// 클래스를 따로 만들지 않고 이름으로 받는 NotifyState.
// Begin/Tick/End를 메시의 AnimInstance::HandleNamedNotifyState*로 넘긴다 (Lua, AnimGraph에서 쓴다).
UCLASS()
class UAnimNotifyState_Named : public UAnimNotifyState
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere)
	FString NotifyName;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
};

UCLASS()
class UAnimNotifyState_ParticleEffect : public UAnimNotifyState
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere)
	FString ParticlePath;

	UPROPERTY(EditAnywhere)
	FString BoneName;

	UPROPERTY(EditAnywhere)
	FString SecondBoneName;

	UPROPERTY(EditAnywhere)
	FString BaseParameterName = "RibbonBase";

	UPROPERTY(EditAnywhere)
	FString TipParameterName = "RibbonTip";

	UPROPERTY(EditAnywhere)
	FVector BoneOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere)
	bool bFollowBoneRotation = true;

	UPROPERTY(EditAnywhere)
	bool bLoopDuringState = true;

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, const UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;

private:
	TMap<USkeletalMeshComponent*, UParticleSystemComponent*> ActiveComponents;
};