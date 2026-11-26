#include "EnginePCH.h"
#include "GameFramework/TestCharacter1d.h"
#include "Components/SkeletalMeshComponent.h"

ATestCharacter1d::ATestCharacter1d()
{
	if (!GetSkeletalMeshComponent()->SetAnimBlueprint(GetAnimationBlueprintPath()))
		KRS_LOG(Error, "Failed to assign TestCharacter1d.animblueprint to ATestCharacter1d");
}
