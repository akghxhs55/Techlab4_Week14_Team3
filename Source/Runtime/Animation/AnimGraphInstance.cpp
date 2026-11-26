#include "EnginePCH.h"
#include "AnimGraphInstance.h"
#include "AnimNode_SequencePlayer.h"
#include "AnimNode_BlendSpacePlayer.h"
#include "AnimNode_BlendSpace2DPlayer.h"
#include "AnimNode_TwoWayBlend.h"
#include "AnimNode_Slot.h"
#include "AnimNode_LayeredBoneBlend.h"
#include "AnimNode_AimOffset1D.h"
#include "AnimSequence.h"
#include "AnimSequenceFile.h"
#include "BlendSpace1D.h"
#include "BlendSpace2D.h"
#include "BlendSpaceFile.h"
#include "AnimNotify.h"
#include "AnimNotifyState.h"
#include "AnimationRuntime.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/Formatter/JsonArchiveInputFormatter.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Casts.h"
#include "UObject/Class.h"
#include "Asset/AssetUtils.h"
#include <algorithm>
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include <cmath>
#include <atomic>

namespace
{
    uint32 AllocateAnimGraphGeneration()
    {
        static std::atomic<uint32> NextGeneration = 0;
        return NextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}

UAnimGraphInstance::UAnimGraphInstance()
{
    Generation = AllocateAnimGraphGeneration();
}

void UAnimGraphInstance::ClearNotifyBindings()
{
    EndAllAnimNotifyStates();
    NotifyQueue.Reset();
    NotifyHandlers.Empty();
    NotifyStateHandlers.Empty();
}

UAnimGraphInstance::~UAnimGraphInstance()
{
	// 소멸 시점에는 메시가 이미 없을 수 있으므로 NotifyState End는 보내지 않는다
	ClearGraph();
}

void UAnimGraphInstance::ResetGraph()
{
	EndAllAnimNotifyStates();
	NotifyQueue.Reset();
	ClearGraph();

	FloatVariables.Empty();
	BoolVariables.Empty();
	NotifyHandlers.Empty();
	NotifyStateHandlers.Empty();
	Generation = AllocateAnimGraphGeneration();
}

void UAnimGraphInstance::ClearGraph()
{
	RootNode = nullptr;
	RootNodeIndex = INDEX_NONE;

	ClearMontages();

	// 노드가 시퀀스를 가리키므로 노드부터 지운다. 노드 안의 규칙이 Lua 함수를 잡고 있을 수 있다.
	Nodes.Empty();
	StatePoseNodes.Empty();
	Bindings.Empty();

	for (UBlendSpace1D* BlendSpace : OwnedBlendSpaces)
		delete BlendSpace;
	OwnedBlendSpaces.Empty();

	for (UBlendSpace2D* BlendSpace2D : OwnedBlendSpaces2D)
		delete BlendSpace2D;
	OwnedBlendSpaces2D.Empty();

	// 시퀀스 소멸자가 소유한 Notify 객체도 지운다
	for (UAnimSequence* Sequence : OwnedSequences)
		delete Sequence;
	OwnedSequences.Empty();
}

// ---------------- 에셋 ----------------

UAnimSequence* UAnimGraphInstance::LoadSequence(const FString& Path)
{
	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UAnimSequence* Sequence = NewObject<UAnimSequence>(this);

	FString Error;
	if (!AnimSequenceFile::Read(std::filesystem::path(FilePath.empty() ? Path : FilePath), *Sequence, Error))
	{
		KRS_LOG(Error, "[AnimGraph] Animation load failed: {} ({})", Path, Error);
		delete Sequence;
		return nullptr;
	}

	OwnedSequences.Add(Sequence);
	return Sequence;
}

UBlendSpace1D* UAnimGraphInstance::CreateBlendSpace()
{
	UBlendSpace1D* BlendSpace = NewObject<UBlendSpace1D>(this);
	OwnedBlendSpaces.Add(BlendSpace);
	return BlendSpace;
}

UBlendSpace1D* UAnimGraphInstance::LoadBlendSpace(const FString& Path)
{
	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UBlendSpace1D* BlendSpace = NewObject<UBlendSpace1D>(this);

	FString Error;
	if (!BlendSpaceFile::Read(std::filesystem::path(FilePath.empty() ? Path : FilePath), *BlendSpace, Error))
	{
		KRS_LOG(Error, "[AnimGraph] BlendSpace load failed: {} ({})", Path, Error);
		delete BlendSpace;
		return nullptr;
	}

    // NotifyState objects can contain mutable state. Each graph owns sample sequences.
    const TArray<FBlendSample> Samples = BlendSpace->GetSampleData();

    while (!BlendSpace->GetSampleData().IsEmpty())
    {
        BlendSpace->RemoveSample(0);
    }

    for (const FBlendSample& Sample : Samples)
    {
        UAnimSequence* Sequence = Sample.Animation ? LoadSequence(Sample.Animation->GetAssetPath()) : nullptr;

        if (!Sequence || !BlendSpace->AddSample(Sequence, Sample.SampleValue))
        {
            KRS_LOG(Warning, "[AnimGraph] Failed to create per-instance BlendSpace samples: {}", Path);
            delete BlendSpace;
            return nullptr;
        }
    }

	OwnedBlendSpaces.Add(BlendSpace);
	return BlendSpace;
}

bool UAnimGraphInstance::OwnsSequence(const UAnimSequence* Sequence) const
{
	return Sequence && OwnedSequences.Find(const_cast<UAnimSequence*>(Sequence)) != -1;
}

bool UAnimGraphInstance::OwnsBlendSpace(const UBlendSpace1D* BlendSpace) const
{
	return BlendSpace && OwnedBlendSpaces.Find(const_cast<UBlendSpace1D*>(BlendSpace)) != -1;
}

UBlendSpace2D* UAnimGraphInstance::CreateBlendSpace2D()
{
	UBlendSpace2D* BlendSpace = NewObject<UBlendSpace2D>(this);
	OwnedBlendSpaces2D.Add(BlendSpace);
	return BlendSpace;
}

UBlendSpace2D* UAnimGraphInstance::LoadBlendSpace2D(const FString& Path)
{
	const FString FilePath = FAssetUtils::ResolveAssetFilePath(Path);
	UBlendSpace2D* BlendSpace = NewObject<UBlendSpace2D>(this);

	FString Error;
	if (!BlendSpaceFile::Read2D(std::filesystem::path(FilePath.empty() ? Path : FilePath), *BlendSpace, Error))
	{
		KRS_LOG(Error, "[AnimGraph] BlendSpace2D load failed: {} ({})", Path, Error);
		delete BlendSpace;
		return nullptr;
	}

	const TArray<FBlendSample> Samples = BlendSpace->GetSampleData();
	BlendSpace->ClearSamples();

	for (const FBlendSample& Sample : Samples)
	{
		UAnimSequence* Sequence = Sample.Animation ? LoadSequence(Sample.Animation->GetAssetPath()) : nullptr;
		if (!Sequence || !BlendSpace->AddSample(Sequence, Sample.SampleValue))
		{
			KRS_LOG(Warning, "[AnimGraph] Failed to create per-instance BlendSpace2D samples: {}", Path);
			delete BlendSpace;
			return nullptr;
		}
	}

	OwnedBlendSpaces2D.Add(BlendSpace);
	return BlendSpace;
}

bool UAnimGraphInstance::OwnsBlendSpace2D(const UBlendSpace2D* BlendSpace) const
{
	return BlendSpace && OwnedBlendSpaces2D.Find(const_cast<UBlendSpace2D*>(BlendSpace)) != -1;
}

// ---------------- 노드 ----------------

int32 UAnimGraphInstance::AddNode(std::unique_ptr<FAnimNode_Base> Node)
{
	Nodes.Add(std::move(Node));
	return Nodes.Num() - 1;
}

int32 UAnimGraphInstance::AddSequencePlayer(UAnimSequence* Sequence, bool bLooping, float PlayRate)
{
	auto Node = std::make_unique<FAnimNode_SequencePlayer>();
	Node->Sequence = Sequence;
	Node->bLooping = bLooping;
	Node->PlayRate = PlayRate;
	return AddNode(std::move(Node));
}

int32 UAnimGraphInstance::AddBlendSpacePlayer(UBlendSpace1D* BlendSpace, const FString& XVariable, float PlayRate)
{
	auto Node = std::make_unique<FAnimNode_BlendSpacePlayer>();
	Node->BlendSpace = BlendSpace;
	Node->PlayRate = PlayRate;
	const int32 Index = AddNode(std::move(Node));

	if (!XVariable.empty())
		Bindings.Add({ Index, EBindingTarget::BlendSpaceX, XVariable });
	return Index;
}

int32 UAnimGraphInstance::AddTwoWayBlend(int32 NodeA, int32 NodeB, const FString& AlphaVariable)
{
	if (!IsValidNode(NodeA) || !IsValidNode(NodeB))
		return INDEX_NONE;

	auto Node = std::make_unique<FAnimNode_TwoWayBlend>();
	Node->A.SetLinkNode(Nodes[NodeA].get());
	Node->B.SetLinkNode(Nodes[NodeB].get());
	const int32 Index = AddNode(std::move(Node));

	if (!AlphaVariable.empty())
		Bindings.Add({ Index, EBindingTarget::TwoWayBlendAlpha, AlphaVariable });
	return Index;
}

void UAnimGraphInstance::SetTwoWayBlendSync(int32 NodeIndex, bool bSync)
{
	if (FAnimNode_TwoWayBlend* Node = GetNode<FAnimNode_TwoWayBlend>(NodeIndex))
	{
		Node->bSyncByNormalizedTime = bSync;
	}
}

int32 UAnimGraphInstance::AddBlendSpace2DPlayer(UBlendSpace2D* BlendSpace, const FString& XVariable, const FString& YVariable, float PlayRate)
{
	std::unique_ptr<FAnimNode_BlendSpace2DPlayer> Node = std::make_unique<FAnimNode_BlendSpace2DPlayer>();
	Node->BlendSpace = BlendSpace;
	Node->PlayRate = PlayRate;
	const int32 Index = AddNode(std::move(Node));

	if (!XVariable.empty())
		Bindings.Add({ Index, EBindingTarget::BlendSpace2DX, XVariable });
	if (!YVariable.empty())
		Bindings.Add({ Index, EBindingTarget::BlendSpace2DY, YVariable });
	return Index;
}

int32 UAnimGraphInstance::AddSlotNode(int32 SourceNode, const FString& SlotName)
{
	if (!IsValidNode(SourceNode))
		return INDEX_NONE;

	std::unique_ptr<FAnimNode_Slot> Node = std::make_unique<FAnimNode_Slot>();
	Node->Source.SetLinkNode(Nodes[SourceNode].get());
	Node->SlotName = SlotName.empty() ? FString("UpperBody") : SlotName;
	Node->GraphInstance = this;
	return AddNode(std::move(Node));
}

int32 UAnimGraphInstance::AddLayeredBoneBlend(int32 BaseNode, int32 BlendNode, const FString& StartBoneName,
	const FString& AlphaVariable, bool bMeshSpaceRotationBlend)
{
	if (!IsValidNode(BaseNode) || !IsValidNode(BlendNode))
		return INDEX_NONE;

	std::unique_ptr<FAnimNode_LayeredBoneBlend> Node = std::make_unique<FAnimNode_LayeredBoneBlend>();
	Node->BasePose.SetLinkNode(Nodes[BaseNode].get());
	Node->BlendPose.SetLinkNode(Nodes[BlendNode].get());
	Node->StartBoneName = StartBoneName.empty() ? FString("spine_01") : StartBoneName;
	Node->bMeshSpaceRotationBlend = bMeshSpaceRotationBlend;
	const int32 Index = AddNode(std::move(Node));

	if (!AlphaVariable.empty())
		Bindings.Add({ Index, EBindingTarget::LayeredBlendAlpha, AlphaVariable });
	return Index;
}

int32 UAnimGraphInstance::AddAimOffset1D(int32 BaseNode, const FString& AimOffsetPath,
	const FString& PitchVariable, const FString& AlphaVariable)
{
	if (!IsValidNode(BaseNode))
		return INDEX_NONE;

	std::unique_ptr<FAnimNode_AimOffset1D> Node = std::make_unique<FAnimNode_AimOffset1D>();
	Node->BasePose.SetLinkNode(Nodes[BaseNode].get());

	if (!AimOffsetPath.empty())
	{
		FString AimError;
		if (!FAimOffsetAsset::Load(AimOffsetPath, Node->Asset, AimError))
		{
			KRS_LOG(Warning, "[AnimGraph] AimOffset load failed: {} ({})", AimOffsetPath, AimError);
			return INDEX_NONE;
		}
		if (!Node->Asset.BaseSequencePath.empty())
		{
			Node->BaseSequence = LoadSequence(Node->Asset.BaseSequencePath);
		}
		TArray<FAimOffsetSample> SortedSamples = Node->Asset.Samples;
		std::sort(SortedSamples.begin(), SortedSamples.end(),
			[](const FAimOffsetSample& Left, const FAimOffsetSample& Right) { return Left.Pitch < Right.Pitch; });
		for (const FAimOffsetSample& Sample : SortedSamples)
		{
			UAnimSequence* Seq = LoadSequence(Sample.SequencePath);
			if (Seq && Seq->GetDataModel())
			{
				Node->Samples.Add({ Sample.Pitch, Seq, Sample.FrameIndex });
			}
		}
	}

	const int32 Index = AddNode(std::move(Node));
	if (!PitchVariable.empty())
		Bindings.Add({ Index, EBindingTarget::AimOffsetPitch, PitchVariable });
	if (!AlphaVariable.empty())
		Bindings.Add({ Index, EBindingTarget::AimOffsetAlpha, AlphaVariable });
	return Index;
}

int32 UAnimGraphInstance::AddStateMachine()
{
	const int32 Index = AddNode(std::make_unique<FAnimNode_StateMachine>());
	StatePoseNodes[Index] = TArray<int32>();
	return Index;
}

bool UAnimGraphInstance::SetRoot(int32 NodeIndex)
{
	if (!IsValidNode(NodeIndex))
		return false;

	RootNodeIndex = NodeIndex;
	RootNode = Nodes[NodeIndex].get();

	FAnimationInitializeContext InitContext;
	RootNode->Initialize(InitContext);
	return true;
}

// ---------------- 상태 머신 ----------------

int32 UAnimGraphInstance::AddState(int32 MachineNode, const FName& Name, int32 PoseNode, bool bResetOnEntry,
	FStateCallback OnEnter, FStateCallback OnExit)
{
	FAnimNode_StateMachine* Machine = GetNode<FAnimNode_StateMachine>(MachineNode);
	if (!Machine || !IsValidNode(PoseNode) || PoseNode == MachineNode)
		return INDEX_NONE;

	FAnimStateDesc Desc;
	Desc.Name = Name;
	Desc.Pose.SetLinkNode(Nodes[PoseNode].get());
	Desc.bResetOnEntry = bResetOnEntry;
	Desc.OnEnter = std::move(OnEnter);
	Desc.OnExit = std::move(OnExit);

	const int32 StateIndex = Machine->AddState(Desc);
	StatePoseNodes[MachineNode].Add(PoseNode);
	return StateIndex;
}

int32 UAnimGraphInstance::AddTransition(int32 MachineNode, int32 FromState, int32 ToState, FAnimTransitionRule Rule,
	float CrossfadeDuration, int32 Priority)
{
	FAnimNode_StateMachine* Machine = GetNode<FAnimNode_StateMachine>(MachineNode);
	const TArray<int32>* States = StatePoseNodes.Find(MachineNode);
	if (!Machine || !States)
		return INDEX_NONE;

	const int32 NumStates = States->Num();
	const bool bValidFrom = FromState == INDEX_NONE || (FromState >= 0 && FromState < NumStates);
	if (!bValidFrom || ToState < 0 || ToState >= NumStates || FromState == ToState || !Rule || !std::isfinite(CrossfadeDuration))
		return INDEX_NONE;

	FAnimTransitionDesc Desc;
	Desc.From = FromState;
	Desc.To = ToState;
	Desc.Rule = std::move(Rule);
	Desc.CrossfadeDuration = FMath::Max(CrossfadeDuration, 0.0f);
	Desc.Priority = Priority;
	return Machine->AddTransition(Desc);
}

bool UAnimGraphInstance::SetInitialState(int32 MachineNode, int32 StateIndex)
{
	FAnimNode_StateMachine* Machine = GetNode<FAnimNode_StateMachine>(MachineNode);
	if (!Machine || GetStatePoseNode(MachineNode, StateIndex) == INDEX_NONE)
		return false;

	Machine->SetInitialState(StateIndex);
	return true;
}

int32 UAnimGraphInstance::GetStatePoseNode(int32 MachineNode, int32 StateIndex) const
{
	const TArray<int32>* States = StatePoseNodes.Find(MachineNode);
	if (!States || StateIndex < 0 || StateIndex >= States->Num())
		return INDEX_NONE;
	return (*States)[StateIndex];
}

FName UAnimGraphInstance::GetCurrentStateName(int32 MachineNode) const
{
	const FAnimNode_StateMachine* Machine = GetNode<FAnimNode_StateMachine>(MachineNode);
	return Machine ? Machine->GetCurrentStateName() : FName();
}

bool UAnimGraphInstance::IsStateActiveOrPending(const FString& StateName) const
{
	const FName Target(StateName);
	for (const std::unique_ptr<FAnimNode_Base>& Node : Nodes)
	{
		if (const FAnimNode_StateMachine* Machine = dynamic_cast<const FAnimNode_StateMachine*>(Node.get()))
		{
			if (Machine->IsStateActiveOrBlending(Target))
				return true;
		}
	}
	return false;
}

void UAnimGraphInstance::SetIsAiming(bool bValue)
{
	SetBool(IsAimingVariable, bValue);
	// 블루프린트의 IsAiming이 Float 블렌드 알파로도 바인딩되므로 함께 갱신한다
	SetFloat(IsAimingVariable, bValue ? 1.0f : 0.0f);
}

void UAnimGraphInstance::RequestAttack()
{
	if (GetBool(IsDeadVariable) || IsStateActiveOrPending("Death"))
		return;
	SetBool(IsAttackingVariable, true);
}

void UAnimGraphInstance::RequestHit()
{
	if (GetBool(IsDeadVariable) || IsStateActiveOrPending("Death"))
		return;
	SetBool(IsAttackingVariable, false);
	SetBool(IsHitVariable, true);
}

void UAnimGraphInstance::RequestDeath()
{
	SetBool(IsHitVariable, false);
	SetBool(IsAttackingVariable, false);
	SetBool(IsDeadVariable, true);
}

FAnimTransitionRule UAnimGraphInstance::MakeBoolRule(const FString& Variable, bool bExpected)
{
	return [this, Variable, bExpected](const FAnimTransitionContext&)
		{
			return GetBool(Variable) == bExpected;
		};
}

FAnimTransitionRule UAnimGraphInstance::MakeFloatRule(const FString& Variable, EAnimGraphCompare Compare, float Value)
{
	return [this, Variable, Compare, Value](const FAnimTransitionContext&)
		{
			const float Current = GetFloat(Variable);
			switch (Compare)
			{
			case EAnimGraphCompare::Less:         return Current < Value;
			case EAnimGraphCompare::LessEqual:    return Current <= Value;
			case EAnimGraphCompare::Greater:      return Current > Value;
			case EAnimGraphCompare::GreaterEqual: return Current >= Value;
			case EAnimGraphCompare::Equal:        return Current == Value;
			case EAnimGraphCompare::NotEqual:     return Current != Value;
			}
			return false;
		};
}

FAnimTransitionRule UAnimGraphInstance::MakeFinishedRule(int32 SequencePlayerNode)
{
	return [this, SequencePlayerNode](const FAnimTransitionContext&)
		{
			const FAnimNode_SequencePlayer* Player = GetNode<FAnimNode_SequencePlayer>(SequencePlayerNode);
			return Player && Player->IsFinished();
		};
}

UAnimGraphInstance::FStateCallback UAnimGraphInstance::MakeSetBoolAction(const FString& Variable, bool bValue)
{
	return [this, Variable, bValue](UAnimInstance*)
		{
			SetBool(Variable, bValue);
		};
}

UAnimGraphInstance::FStateCallback UAnimGraphInstance::MakeSetFloatAction(const FString& Variable, float Value)
{
	return [this, Variable, Value](UAnimInstance*)
		{
			SetFloat(Variable, Value);
		};
}

UAnimGraphInstance::FStateCallback UAnimGraphInstance::MakeNamedNotifyAction(const FName& NotifyName)
{
	return [this, NotifyName](UAnimInstance*)
		{
			HandleNamedNotify(NotifyName);
		};
}

UAnimGraphInstance::FStateCallback UAnimGraphInstance::CombineActions(TArray<FStateCallback> Actions)
{
	TArray<FStateCallback> Valid;
	for (FStateCallback& Action : Actions)
	{
		if (Action)
			Valid.Add(std::move(Action));
	}

	if (Valid.IsEmpty())
		return {};
	if (Valid.Num() == 1)
		return std::move(Valid[0]);

	return [Valid = std::move(Valid)](UAnimInstance* Instance)
		{
			for (const FStateCallback& Action : Valid)
				Action(Instance);
		};
}

// ---------------- 변수 ----------------

float UAnimGraphInstance::GetFloat(const FString& Name, float Default) const
{
	const float* Found = FloatVariables.Find(Name);
	return Found ? *Found : Default;
}

bool UAnimGraphInstance::GetBool(const FString& Name, bool bDefault) const
{
	const bool* Found = BoolVariables.Find(Name);
	return Found ? *Found : bDefault;
}

// ---------------- 이름 기반 Notify ----------------

void UAnimGraphInstance::BindNotify(const FName& Name, FNotifyCallback Callback)
{
	if (Callback)
		NotifyHandlers[Name.ToString()] = std::move(Callback);
	else
		NotifyHandlers.Remove(Name.ToString());
}

void UAnimGraphInstance::BindNotifyState(const FName& Name, FNotifyStateCallback OnBegin, FNotifyStateCallback OnTick, FNotifyStateCallback OnEnd)
{
	NotifyStateHandlers[Name.ToString()] = { std::move(OnBegin), std::move(OnTick), std::move(OnEnd) };
}

void UAnimGraphInstance::HandleNamedNotify(const FName& NotifyName)
{
	if (const FNotifyCallback* Callback = NotifyHandlers.Find(NotifyName.ToString()))
		(*Callback)();
}

void UAnimGraphInstance::HandleNamedNotifyStateBegin(const FName& NotifyName, float TotalDuration)
{
	const FNotifyStateHandlers* Handlers = NotifyStateHandlers.Find(NotifyName.ToString());
	if (Handlers && Handlers->OnBegin)
		Handlers->OnBegin(TotalDuration);
}

void UAnimGraphInstance::HandleNamedNotifyStateTick(const FName& NotifyName, float DeltaTime)
{
	const FNotifyStateHandlers* Handlers = NotifyStateHandlers.Find(NotifyName.ToString());
	if (Handlers && Handlers->OnTick)
		Handlers->OnTick(DeltaTime);
}

void UAnimGraphInstance::HandleNamedNotifyStateEnd(const FName& NotifyName)
{
	const FNotifyStateHandlers* Handlers = NotifyStateHandlers.Find(NotifyName.ToString());
	if (Handlers && Handlers->OnEnd)
		Handlers->OnEnd(0.0f);
}

// ---------------- 갱신 ----------------

void UAnimGraphInstance::NativeInitializeAnimation()
{
	// UAnimInstance::InitializeAnimation이 RootNode를 비운 뒤 부르므로, 조립해 둔 그래프를 다시 연결한다
	RootNode = IsValidNode(RootNodeIndex) ? Nodes[RootNodeIndex].get() : nullptr;
}

void UAnimGraphInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	if (APawn* Pawn = TryGetPawnOwner())
	{
		const FVector Velocity = Pawn->GetVelocity();
		const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
		SetFloat(GroundSpeedVariable, HorizontalVelocity.Size() * WorldToCm);

		float Direction = 0.0f;
		if (HorizontalVelocity.SizeSquared() > SMALL_NUMBER)
		{
			const FQuat Facing = Pawn->GetActorQuat();
			const float Forward = HorizontalVelocity.Dot(Facing.GetForwardVector());
			const float Right = HorizontalVelocity.Dot(Facing.GetRightVector());
			Direction = FMath::RadiansToDegrees(std::atan2(Right, Forward));
		}
		SetFloat(DirectionVariable, Direction);

		if (const ACharacter* Character = Cast<ACharacter>(Pawn))
			SetBool(IsFallingVariable, Character->IsFalling());

		float AimPitch = 0.0f;
		const APlayerController* PC = Pawn->GetController();
		for (UActorComponent* Comp : Pawn->GetComponents())
		{
			if (const USpringArmComponent* SpringArm = Cast<USpringArmComponent>(Comp))
			{
				const float Pitch = (SpringArm->bUsePawnControlRotation && PC)
					? PC->ControlRotation.Pitch
					: SpringArm->GetRelativeRotation().Pitch;
				AimPitch = std::clamp(-Pitch, -90.0f, 90.0f);
				break;
			}
		}
		SetFloat(AimPitchVariable, AimPitch);
	}

	UpdateMontages(DeltaSeconds);

	for (const FVariableBinding& Binding : Bindings)
	{
		const float Value = GetFloat(Binding.Variable);
		switch (Binding.Target)
		{
		case EBindingTarget::BlendSpaceX:
			if (FAnimNode_BlendSpacePlayer* Player = GetNode<FAnimNode_BlendSpacePlayer>(Binding.NodeIndex))
				Player->X = Value;
			break;
		case EBindingTarget::BlendSpace2DX:
			if (FAnimNode_BlendSpace2DPlayer* Player2D = GetNode<FAnimNode_BlendSpace2DPlayer>(Binding.NodeIndex))
				Player2D->X = Value;
			break;
		case EBindingTarget::BlendSpace2DY:
			if (FAnimNode_BlendSpace2DPlayer* Player2D = GetNode<FAnimNode_BlendSpace2DPlayer>(Binding.NodeIndex))
				Player2D->Y = Value;
			break;
		case EBindingTarget::TwoWayBlendAlpha:
			if (FAnimNode_TwoWayBlend* Blend = GetNode<FAnimNode_TwoWayBlend>(Binding.NodeIndex))
				Blend->Alpha = Value;
			break;
		case EBindingTarget::LayeredBlendAlpha:
			if (FAnimNode_LayeredBoneBlend* Layered = GetNode<FAnimNode_LayeredBoneBlend>(Binding.NodeIndex))
				Layered->Alpha = Value;
			break;
		case EBindingTarget::AimOffsetAlpha:
			if (FAnimNode_AimOffset1D* Aim = GetNode<FAnimNode_AimOffset1D>(Binding.NodeIndex))
				Aim->Alpha = Value;
			break;
		case EBindingTarget::AimOffsetPitch:
			if (FAnimNode_AimOffset1D* Aim = GetNode<FAnimNode_AimOffset1D>(Binding.NodeIndex))
				Aim->Pitch = Value;
			break;
		}
	}
}

void FAnimNode_Slot::Evaluate(FPoseContext& Output)
{
	Source.Evaluate(Output);
	if (GraphInstance && !SlotName.empty())
	{
		GraphInstance->EvaluateSlotPose(SlotName, Output);
	}
}

float UAnimGraphInstance::ResolveMontageSegmentRate(const FActiveMontageSegment& Segment)
{
	return std::isfinite(Segment.PlayRate) && Segment.PlayRate > 0.0f ? Segment.PlayRate : 1.0f;
}

float UAnimGraphInstance::MapToMontageSegmentLocalTime(const FActiveMontageSegment& Segment, float MontageTime)
{
	const float SeqLength = Segment.Sequence && Segment.Sequence->GetDataModel()
		? Segment.Sequence->GetDataModel()->GetPlayLength() : 0.0f;
	const float LocalTime = Segment.AnimStartOffset +
		std::max(0.0f, MontageTime - Segment.StartTime) * ResolveMontageSegmentRate(Segment);
	return std::clamp(LocalTime, 0.0f, std::max(0.0f, SeqLength));
}

const UAnimGraphInstance::FActiveMontageSegment* UAnimGraphInstance::ResolveActiveMontageSegment(
	const TArray<FActiveMontageSegment>& Segments, float MontageTime, float& OutLocalTime)
{
	if (Segments.IsEmpty())
		return nullptr;
	const FActiveMontageSegment* Match = nullptr;
	const FActiveMontageSegment* NearestPrior = &Segments[0];
	for (const FActiveMontageSegment& Seg : Segments)
	{
		if (!Seg.Sequence || !Seg.Sequence->GetDataModel())
			continue;
		if (MontageTime >= Seg.StartTime && MontageTime <= Seg.EndTime)
			Match = &Seg;
		if (Seg.StartTime <= MontageTime && Seg.StartTime >= NearestPrior->StartTime)
			NearestPrior = &Seg;
	}
	const FActiveMontageSegment* Chosen = Match ? Match : NearestPrior;
	if (!Chosen || !Chosen->Sequence || !Chosen->Sequence->GetDataModel())
		return nullptr;
	OutLocalTime = MapToMontageSegmentLocalTime(*Chosen, MontageTime);
	return Chosen;
}

void UAnimGraphInstance::ClearMontages()
{
	EndAllAnimNotifyStates();
	NotifyQueue.Reset();
	ActiveMontages.Empty();
	for (UObject* Obj : OwnedMontageNotifyObjects)
		delete Obj;
	OwnedMontageNotifyObjects.Empty();
}

float UAnimGraphInstance::PlayMontage(const FString& MontagePath, float InPlayRate, const FString& StartSection)
{
	if (MontagePath.empty() || !std::isfinite(InPlayRate) || InPlayRate <= 0.0f)
		return 0.0f;

	FAnimMontageAsset Asset;
	FString Error;
	if (!FAnimMontageAsset::Load(MontagePath, Asset, Error))
	{
		KRS_LOG(Warning, "[AnimGraph] Montage load failed: {} ({})", MontagePath, Error);
		return 0.0f;
	}

	return PlayMontageFromAsset(MontagePath, std::move(Asset), InPlayRate, StartSection);
}

float UAnimGraphInstance::PlayMontageFromAsset(const FString& MontagePath, FAnimMontageAsset Asset, float InPlayRate, const FString& StartSection)
{
	if (!std::isfinite(InPlayRate) || InPlayRate <= 0.0f)
		return 0.0f;

	TArray<FActiveMontageSegment> LoadedSegments;
	float TotalLength = 0.0f;
	if (!Asset.Segments.IsEmpty())
	{
		for (const FAnimMontageSegment& SegDef : Asset.Segments)
		{
			UAnimSequence* SegSeq = LoadSequence(SegDef.SequencePath);
			if (!SegSeq || !SegSeq->GetDataModel() || SegSeq->GetDataModel()->GetPlayLength() <= 0.0f)
			{
				KRS_LOG(Warning, "[AnimGraph] Montage segment failed to load: {}", SegDef.SequencePath);
				return 0.0f;
			}
			FActiveMontageSegment ActiveSeg;
			ActiveSeg.Sequence = SegSeq;
			ActiveSeg.StartTime = std::max(0.0f, SegDef.StartTime);
			ActiveSeg.AnimStartOffset = std::max(0.0f, SegDef.AnimStartOffset);
			ActiveSeg.PlayRate = SegDef.PlayRate > 0.0f ? SegDef.PlayRate : 1.0f;
			ActiveSeg.EndTime = SegDef.EndTime > ActiveSeg.StartTime
				? SegDef.EndTime
				: (ActiveSeg.StartTime + SegSeq->GetDataModel()->GetPlayLength() / ResolveMontageSegmentRate(ActiveSeg));
			TotalLength = std::max(TotalLength, ActiveSeg.EndTime);
			LoadedSegments.Add(ActiveSeg);
		}
	}
	else
	{
		UAnimSequence* SingleSeq = LoadSequence(Asset.SequencePath);
		if (!SingleSeq || !SingleSeq->GetDataModel() || SingleSeq->GetDataModel()->GetPlayLength() <= 0.0f)
		{
			KRS_LOG(Warning, "[AnimGraph] Montage sequence failed to load: {}", Asset.SequencePath);
			return 0.0f;
		}
		const float SeqLen = SingleSeq->GetDataModel()->GetPlayLength();
		LoadedSegments.Add({ SingleSeq, 0.0f, SeqLen, 0.0f, 1.0f });
		TotalLength = SeqLen;
	}
	if (LoadedSegments.IsEmpty() || TotalLength <= 0.0f)
		return 0.0f;

	int32 SectionIdx = -1;
	float InitialPos = 0.0f;
	if (!Asset.Sections.IsEmpty())
	{
		SectionIdx = 0;
		if (!StartSection.empty())
		{
			for (int32 Idx = 0; Idx < Asset.Sections.Num(); ++Idx)
			{
				if (Asset.Sections[Idx].Name == StartSection)
				{
					SectionIdx = Idx;
					break;
				}
			}
		}
		InitialPos = std::clamp(Asset.Sections[SectionIdx].StartTime, 0.0f, TotalLength);
	}

	FActiveMontage* Target = nullptr;
	for (FActiveMontage& Existing : ActiveMontages)
	{
		if (Existing.Asset.SlotName == Asset.SlotName)
		{
			Target = &Existing;
			break;
		}
	}
	if (!Target)
	{
		ActiveMontages.Add(FActiveMontage{});
		Target = &ActiveMontages[ActiveMontages.Num() - 1];
	}
	else
	{
		// 기존 슬롯 몽타주의 활성 State를 종료한다
		EndAllAnimNotifyStates();
	}

	Target->Path = MontagePath;
	Target->Asset = std::move(Asset);
	Target->Sequence = LoadedSegments[0].Sequence;
	Target->Segments = std::move(LoadedSegments);
	Target->NotifyEvents.SetNum(Target->Asset.Notifies.Num());

	for (int32 Idx = 0; Idx < Target->Asset.Notifies.Num(); ++Idx)
	{
		const FAnimMontageNotify& NotifyDef = Target->Asset.Notifies[Idx];
		FAnimNotifyEvent& Event = Target->NotifyEvents[Idx];
		Event.TriggerTime = NotifyDef.Time;
		Event.Duration = NotifyDef.Duration;
		Event.NotifyName = FName(NotifyDef.Name);
		Event.Notify = nullptr;
		Event.NotifyStateClass = nullptr;

		if (NotifyDef.ClassName.empty())
			continue;

		UClass* NotifyClass = FindClass(NotifyDef.ClassName);
		if (!NotifyClass)
			continue;

		const bool bState = NotifyClass->IsChildOf(UAnimNotifyState::StaticClass());
		UObject* Object = nullptr;
		if (bState && NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
		{
			Object = NewObject<UAnimNotifyState>(this, NotifyClass);
		}
		else if (!bState && NotifyClass->IsChildOf(UAnimNotify::StaticClass()))
		{
			Object = NewObject<UAnimNotify>(this, NotifyClass);
		}
		if (!Object)
			continue;

		if (!NotifyDef.Properties.empty())
		{
			TArray<uint8> Bytes;
			Bytes.SetNum(static_cast<int32>(NotifyDef.Properties.size()));
			std::memcpy(Bytes.GetData(), NotifyDef.Properties.data(), NotifyDef.Properties.size());
			FMemoryReader Reader(Bytes, true);
			FJsonArchiveInputFormatter Formatter(Reader);
			if (!Reader.IsError())
			{
				// 노티파이 속성은 최상위 평면 json 객체로 저장됨
				FStructuredArchive Archive(Formatter);
				Object->GetClass()->SerializeTaggedProperties(Archive.Open(), Object);
			}
		}

		OwnedMontageNotifyObjects.Add(Object);
		if (bState)
			Event.NotifyStateClass = static_cast<UAnimNotifyState*>(Object);
		else
			Event.Notify = static_cast<UAnimNotify*>(Object);
	}

	Target->TotalLength = TotalLength;
	Target->Position = InitialPos;
	Target->PlayRate = InPlayRate;
	Target->ElapsedTime = 0.0f;
	Target->BlendWeight = Target->Asset.BlendInTime <= 0.0f ? 1.0f : 0.0f;
	Target->CurrentSectionIndex = SectionIdx;
	Target->bStopping = false;
	Target->StopElapsed = 0.0f;
	Target->StopDuration = Target->Asset.BlendOutTime;
	Target->StopStartWeight = 1.0f;
	Target->bPlaying = true;
	Target->bPaused = false;
	Target->bNotifyIncludeStart = true;

	return TotalLength / std::max(0.001f, InPlayRate * Target->Asset.PlayRate);
}

void UAnimGraphInstance::StopMontage(const FString& SlotName, float BlendOutOverride)
{
	for (FActiveMontage& Montage : ActiveMontages)
	{
		if (!Montage.bPlaying)
			continue;
		if (!SlotName.empty() && Montage.Asset.SlotName != SlotName)
			continue;

		const float BlendOut = BlendOutOverride >= 0.0f ? BlendOutOverride : Montage.Asset.BlendOutTime;
		if (BlendOut <= 0.0f)
		{
			Montage.bPlaying = false;
			Montage.BlendWeight = 0.0f;
		}
		else if (!Montage.bStopping)
		{
			Montage.bStopping = true;
			Montage.StopElapsed = 0.0f;
			Montage.StopDuration = BlendOut;
			Montage.StopStartWeight = std::max(0.01f, Montage.BlendWeight);
		}
	}
}

bool UAnimGraphInstance::JumpToSection(const FString& SectionName, const FString& SlotName)
{
	if (SectionName.empty())
		return false;

	for (FActiveMontage& Montage : ActiveMontages)
	{
		if (!Montage.bPlaying)
			continue;
		if (!SlotName.empty() && Montage.Asset.SlotName != SlotName)
			continue;

		for (int32 Idx = 0; Idx < Montage.Asset.Sections.Num(); ++Idx)
		{
			if (Montage.Asset.Sections[Idx].Name == SectionName)
			{
				Montage.CurrentSectionIndex = Idx;
				Montage.Position = std::clamp(Montage.Asset.Sections[Idx].StartTime, 0.0f, Montage.TotalLength);
				Montage.bNotifyIncludeStart = true;
				return true;
			}
		}
	}
	return false;
}

bool UAnimGraphInstance::IsPlayingMontage(const FString& SlotName) const
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
			return true;
	}
	return false;
}

float UAnimGraphInstance::GetMontagePosition(const FString& SlotName) const
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
			return Montage.Position;
	}
	return 0.0f;
}

void UAnimGraphInstance::SetMontagePaused(bool bInPaused, const FString& SlotName)
{
	for (FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
		{
			Montage.bPaused = bInPaused;
		}
	}
}

bool UAnimGraphInstance::IsMontagePaused(const FString& SlotName) const
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
			return Montage.bPaused;
	}
	return false;
}

bool UAnimGraphInstance::SetMontagePosition(float InPosition, bool bPauseMontage, const FString& SlotName)
{
	bool bUpdated = false;
	for (FActiveMontage& Montage : ActiveMontages)
	{
		if (!Montage.bPlaying)
			continue;
		if (!SlotName.empty() && Montage.Asset.SlotName != SlotName)
			continue;

		Montage.Position = std::clamp(InPosition, 0.0f, std::max(0.0f, Montage.TotalLength));
		Montage.bPaused = bPauseMontage;
		Montage.bStopping = false;
		Montage.BlendWeight = 1.0f;
		Montage.bNotifyIncludeStart = false;
		bUpdated = true;
	}
	return bUpdated;
}

float UAnimGraphInstance::GetMontageLength(const FString& SlotName) const
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
			return Montage.TotalLength;
	}
	return 0.0f;
}

float UAnimGraphInstance::GetMontageWeight(const FString& SlotName) const
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (Montage.bPlaying && (SlotName.empty() || Montage.Asset.SlotName == SlotName))
			return Montage.BlendWeight;
	}
	return 0.0f;
}

void UAnimGraphInstance::UpdateMontages(float DeltaSeconds)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
		return;

	for (FActiveMontage& Montage : ActiveMontages)
	{
		if (!Montage.bPlaying || Montage.Segments.IsEmpty())
			continue;

		const float SeqLength = Montage.TotalLength;
		if (SeqLength <= 0.0f)
		{
			Montage.bPlaying = false;
			Montage.BlendWeight = 0.0f;
			continue;
		}

		if (Montage.bPaused)
		{
			// 일시정지 상태에서도 활성 노티파이 스테이트 유지
			TArray<FAnimNotifyEventReference> PausedStateRefs;
			for (int32 NotifyIdx = 0; NotifyIdx < Montage.Asset.Notifies.Num(); ++NotifyIdx)
			{
				const FAnimNotifyEvent& Event = Montage.NotifyEvents[NotifyIdx];
				if (Event.NotifyStateClass && Event.Duration > 0.0f)
				{
					const float StateEnd = Event.TriggerTime + Event.Duration;
					if (StateEnd >= Montage.Position && Event.TriggerTime <= Montage.Position)
					{
						PausedStateRefs.Add(FAnimNotifyEventReference(&Event, Montage.Sequence));
					}
				}
			}
			if (!PausedStateRefs.IsEmpty())
			{
				AddNotifies(PausedStateRefs, 1.0f);
			}
			continue;
		}

		Montage.ElapsedTime += DeltaSeconds;
		if (Montage.bStopping)
		{
			Montage.StopElapsed += DeltaSeconds;
			const float Alpha = Montage.StopDuration > 0.0f
				? std::clamp(1.0f - Montage.StopElapsed / Montage.StopDuration, 0.0f, 1.0f) : 0.0f;
			Montage.BlendWeight = Montage.StopStartWeight * Alpha;
			if (Montage.StopElapsed >= Montage.StopDuration)
			{
				Montage.bPlaying = false;
				Montage.BlendWeight = 0.0f;
				continue;
			}
		}

		const float PrevPos = Montage.Position;
		const float EffectiveRate = std::max(0.001f, Montage.PlayRate * Montage.Asset.PlayRate);
		const float NextPos = PrevPos + DeltaSeconds * EffectiveRate;

		float SecStart = 0.0f;
		float SecEnd = SeqLength;
		FString NextSectionName;
		if (Montage.CurrentSectionIndex >= 0 && Montage.CurrentSectionIndex < Montage.Asset.Sections.Num())
		{
			const FAnimMontageSection& Sec = Montage.Asset.Sections[Montage.CurrentSectionIndex];
			SecStart = std::clamp(Sec.StartTime, 0.0f, SeqLength);
			SecEnd = std::clamp(Sec.EndTime, SecStart, SeqLength);
			NextSectionName = Sec.NextSection;
		}

		const float NotifyCheckEnd = std::min(NextPos, SecEnd);
		const bool bFirstStep = Montage.bNotifyIncludeStart;
		Montage.bNotifyIncludeStart = false;

		TArray<FAnimNotifyEventReference> MontageNotifyRefs;
		for (int32 NotifyIdx = 0; NotifyIdx < Montage.Asset.Notifies.Num(); ++NotifyIdx)
		{
			const FAnimMontageNotify& NotifyDef = Montage.Asset.Notifies[NotifyIdx];
			const FAnimNotifyEvent& Event = Montage.NotifyEvents[NotifyIdx];
			if (Event.NotifyStateClass && Event.Duration > 0.0f)
			{
				const float StateEnd = Event.TriggerTime + Event.Duration;
				if (StateEnd > PrevPos && Event.TriggerTime <= NotifyCheckEnd)
				{
					MontageNotifyRefs.Add(FAnimNotifyEventReference(&Event, Montage.Sequence));
				}
				continue;
			}

			const bool bTriggered = bFirstStep
				? (NotifyDef.Time >= PrevPos && NotifyDef.Time <= NotifyCheckEnd)
				: (NotifyDef.Time > PrevPos && NotifyDef.Time <= NotifyCheckEnd);
			if (bTriggered)
			{
				if (OnMontageNotify)
					OnMontageNotify(NotifyDef.Name);
				MontageNotifyRefs.Add(FAnimNotifyEventReference(&Event, Montage.Sequence));
			}
		}
		if (!MontageNotifyRefs.IsEmpty())
		{
			AddNotifies(MontageNotifyRefs, 1.0f);
		}

		const float RangeEnd = NextPos < SecEnd ? NextPos : SecEnd;
		if (!Montage.bStopping && ShouldExtractRootMotion())
		{
			for (const FActiveMontageSegment& Seg : Montage.Segments)
			{
				if (!Seg.Sequence || !Seg.Sequence->GetEnableRootMotion())
					continue;
				const float WinStart = std::max(PrevPos, Seg.StartTime);
				const float WinEnd = std::min(RangeEnd, Seg.EndTime);
				if (WinEnd <= WinStart)
					continue;
				const float LocalStart = MapToMontageSegmentLocalTime(Seg, WinStart);
				const float LocalDelta = MapToMontageSegmentLocalTime(Seg, WinEnd) - LocalStart;
				if (LocalDelta > 0.0f)
				{
					AddRootMotion(Seg.Sequence->ExtractRootMotion(LocalStart, LocalDelta, false));
				}
			}
		}

		if (NextPos < SecEnd)
		{
			Montage.Position = NextPos;
		}
		else
		{
			int32 NextSecIdx = -1;
			if (!NextSectionName.empty())
			{
				for (int32 Idx = 0; Idx < Montage.Asset.Sections.Num(); ++Idx)
				{
					if (Montage.Asset.Sections[Idx].Name == NextSectionName)
					{
						NextSecIdx = Idx;
						break;
					}
				}
			}
			if (NextSecIdx >= 0)
			{
				Montage.CurrentSectionIndex = NextSecIdx;
				Montage.Position = std::clamp(Montage.Asset.Sections[NextSecIdx].StartTime, 0.0f, SeqLength);
				Montage.bNotifyIncludeStart = true;
			}
			else if (Montage.Asset.bLoop)
			{
				const float Span = std::max(0.001f, SecEnd - SecStart);
				Montage.Position = SecStart + std::fmod(NextPos - SecStart, Span);
				Montage.bNotifyIncludeStart = true;
			}
			else
			{
				Montage.Position = SecEnd;
				Montage.bPlaying = false;
				Montage.BlendWeight = 0.0f;
				continue;
			}
		}

		if (!Montage.bStopping)
		{
			const float BlendInAlpha = Montage.Asset.BlendInTime > 0.0f
				? std::clamp(Montage.ElapsedTime / Montage.Asset.BlendInTime, 0.0f, 1.0f) : 1.0f;
			const float RemainingSeconds = std::max(0.0f, SecEnd - Montage.Position) / EffectiveRate;
			const bool bWillEnd = !Montage.Asset.bLoop && NextSectionName.empty();
			const float BlendOutAlpha = (bWillEnd && Montage.Asset.BlendOutTime > 0.0f)
				? std::clamp(RemainingSeconds / Montage.Asset.BlendOutTime, 0.0f, 1.0f) : 1.0f;
			Montage.BlendWeight = std::min(BlendInAlpha, BlendOutAlpha);
		}
	}
}

bool UAnimGraphInstance::EvaluateSlotPose(const FString& SlotName, FPoseContext& InOutPose)
{
	for (const FActiveMontage& Montage : ActiveMontages)
	{
		if (!Montage.bPlaying || Montage.Asset.SlotName != SlotName || Montage.BlendWeight <= 0.0f)
			continue;

		float LocalTime = 0.0f;
		const FActiveMontageSegment* ActiveSeg = ResolveActiveMontageSegment(Montage.Segments, Montage.Position, LocalTime);
		if (!ActiveSeg || !ActiveSeg->Sequence)
			continue;

		FPoseContext MontagePose(InOutPose);
		ActiveSeg->Sequence->GetAnimationPose(LocalTime, MontagePose.Pose, InOutPose.GetRefSkeleton(), ShouldExtractRootMotion());
		if (Montage.BlendWeight >= 1.0f || InOutPose.Pose.GetNumBones() != MontagePose.Pose.GetNumBones())
		{
			InOutPose.Pose = MontagePose.Pose;
			return true;
		}

		FPoseContext Blended(InOutPose);
		FAnimationRuntime::BlendTwoPosesTogether(
			InOutPose.Pose, MontagePose.Pose,
			InOutPose.Curve, MontagePose.Curve,
			1.0f - Montage.BlendWeight, Blended.Pose, Blended.Curve);
		InOutPose.Pose = Blended.Pose;
		return true;
	}
	return false;
}
