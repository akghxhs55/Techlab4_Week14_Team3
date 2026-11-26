#include "EnginePCH.h"
#include "BoxComponent.h"

#include "Physics/BodySetup.h"
#include "Render/LineBatcher.h"

void UBoxComponent::UpdateBodySetup()
{
	FAggregateGeom& AggGeom = GetBodySetup()->AggGeom;
	AggGeom.EmptyElements();
	
	uint32 Index = AggGeom.BoxElems.Emplace();
	FBoxElem& BoxElem = AggGeom.BoxElems[Index];
	BoxElem.X = BoxExtent.X * 2.0f;
	BoxElem.Y = BoxExtent.Y * 2.0f;
	BoxElem.Z = BoxExtent.Z * 2.0f;
}

FVector UBoxComponent::GetScaledBoxExtent() const
{
	const FVector S = GetWorldScale3D().GetAbs(); // 음수 스케일(뒤집기)도 크기는 양수
	return FVector(BoxExtent.X * S.X, BoxExtent.Y * S.Y, BoxExtent.Z * S.Z);	
}

FBox UBoxComponent::CalcLocalBounds() const
{
	return FBox{ BoxExtent * -1.0f, BoxExtent };
}

void UBoxComponent::DrawDebug(FLineBatcher* LineBatcher, const FVector4& Color) const
{
	// 엔진에 이미 있는 함수. 로컬 박스의 8꼭짓점을 월드 행렬(회전·스케일 포함)로 옮겨 그린다
	LineBatcher->AddOrientedBox(FBox{ BoxExtent * -1.0f, BoxExtent }, GetWorldMatrix(), Color);
}