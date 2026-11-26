#include "EnginePCH.h"
#include "PhysicsAssetEditor.h"
#include "PhysicsAssetEditorTab.h"
#include "PhysicsAssetSimulation.h"
#include "Engine/World.h"
#include "Engine/Scene.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Asset/AssetManager.h"
#include "Render/Mesh.h"
#include "Render/SkeletalMesh.h"
#include "Render/LineBatcher.h"
#include "Physics/PhysicsAsset.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsBoneUtils.h"
#include "Physics/PhysXConversion.h"
#include "Asset/AssetUtils.h"
#include "Core/Misc/Paths.h"
#include "Serialization/JsonArchive.h"
#include "Editor/Physics/PhysicsAssetGenerator.h"

#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <limits>
#include <foundation/PxMathUtils.h>

FAggregateGeom& FPhysicsAssetPreviewBody::GetGeometry()
{
	return BodySetup->AggGeom;
}

const FAggregateGeom& FPhysicsAssetPreviewBody::GetGeometry() const
{
	return BodySetup->AggGeom;
}

namespace
{
	// 편집본은 이 에디터만 소유한다. UObject 목록의 일괄 정리 전에 자식도 함께 해제한다.
	void ClearEditingBodies(UPhysicsAsset& Asset)
	{
		Asset.ClearBodies();
	}

	bool ShapePropertiesEqual(const FShapeElem& A, const FVector& SizeA, const FShapeElem& B, const FVector& SizeB)
	{
		return A.Center.X == B.Center.X && A.Center.Y == B.Center.Y && A.Center.Z == B.Center.Z &&
			A.Rotation.Pitch == B.Rotation.Pitch && A.Rotation.Yaw == B.Rotation.Yaw && A.Rotation.Roll == B.Rotation.Roll &&
			SizeA.X == SizeB.X && SizeA.Y == SizeB.Y && SizeA.Z == SizeB.Z;
	}

	int32 GetShapeCount(const FAggregateGeom& Geometry, FPhysicsAssetEditor::EPreviewShapeType Type)
	{
		switch (Type)
		{
		case FPhysicsAssetEditor::EPreviewShapeType::Sphere: return Geometry.SphereElems.Num();
		case FPhysicsAssetEditor::EPreviewShapeType::Box: return Geometry.BoxElems.Num();
		case FPhysicsAssetEditor::EPreviewShapeType::Capsule: return Geometry.CapsuleElems.Num();
		default: return 0;
		}
	}

	// 로컬 레이의 방향을 정규화하지 않아 교차 매개변수 t를 월드 레이와 공유합니다.
	bool RayHitsSphere(const FRay& Ray, const FVector& Center, float Radius, float& OutT)
	{
		const FVector Offset = Ray.Origin - Center;
		const float C = Offset.Dot(Offset) - Radius * Radius;
		if (C <= 0.0f) { OutT = 0.0f; return true; }
		const float A = Ray.Direction.Dot(Ray.Direction);
		const float B = Offset.Dot(Ray.Direction);
		if (A <= 0.0f) return false;
		const float Discriminant = B * B - A * C;
		if (Discriminant < 0.0f) return false;
		OutT = (-B - std::sqrt(Discriminant)) / A;
		return OutT >= 0.0f;
	}

	bool RayHitsBox(const FRay& Ray, const FVector& HalfExtent, float& OutT)
	{
		float Enter = 0.0f;
		float Exit = FLT_MAX;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float Origin = Ray.Origin[Axis];
			const float Direction = Ray.Direction[Axis];
			const float Extent = HalfExtent[Axis];
			if (Direction == 0.0f)
			{
				if (Origin < -Extent || Origin > Extent) return false;
				continue;
			}
			const float T0 = (-Extent - Origin) / Direction;
			const float T1 = (Extent - Origin) / Direction;
			Enter = FMath::Max(Enter, FMath::Min(T0, T1));
			Exit = FMath::Min(Exit, FMath::Max(T0, T1));
			if (Enter > Exit) return false;
		}
		OutT = Enter;
		return true;
	}

	bool RayHitsCapsule(const FRay& Ray, float Radius, float Length, float& OutT)
	{
		const float HalfLength = Length * 0.5f;
		const float ClosestZ = FMath::Clamp(Ray.Origin.Z, -HalfLength, HalfLength);
		const FVector Offset = Ray.Origin - FVector(0.0f, 0.0f, ClosestZ);
		if (Offset.Dot(Offset) <= Radius * Radius) { OutT = 0.0f; return true; }

		float Nearest = FLT_MAX;
		float T;
		for (float CenterZ : { -HalfLength, HalfLength })
		{
			if (RayHitsSphere(Ray, FVector(0.0f, 0.0f, CenterZ), Radius, T))
				Nearest = FMath::Min(Nearest, T);
		}
		const float A = Ray.Direction.X * Ray.Direction.X + Ray.Direction.Y * Ray.Direction.Y;
		const float B = Ray.Origin.X * Ray.Direction.X + Ray.Origin.Y * Ray.Direction.Y;
		const float C = Ray.Origin.X * Ray.Origin.X + Ray.Origin.Y * Ray.Origin.Y - Radius * Radius;
		const float Discriminant = B * B - A * C;
		if (A > 0.0f && Discriminant >= 0.0f)
		{
			const float Root = std::sqrt(Discriminant);
			for (float Candidate : { (-B - Root) / A, (-B + Root) / A })
			{
				const float Z = Ray.Origin.Z + Candidate * Ray.Direction.Z;
				if (Candidate >= 0.0f && Z >= -HalfLength && Z <= HalfLength)
					Nearest = FMath::Min(Nearest, Candidate);
			}
		}
		OutT = Nearest;
		return Nearest < FLT_MAX;
	}

	void DrawWireArc(FLineBatcher& Lines, const FMatrix& ShapeToWorld,
		const FVector& Center, const FVector& AxisA, const FVector& AxisB,
		float Radius, float StartAngle, float EndAngle, const FVector4& Color)
	{
		constexpr int32 Segments = 24;
		const auto PointAt = [&](float Angle)
		{
			return ShapeToWorld.TransformPosition(Center +
				AxisA * (std::cos(Angle) * Radius) + AxisB * (std::sin(Angle) * Radius));
		};

		FVector Previous = PointAt(StartAngle);
		for (int32 Index = 1; Index <= Segments; ++Index)
		{
			const float Angle = StartAngle + (EndAngle - StartAngle) * static_cast<float>(Index) / Segments;
			const FVector Current = PointAt(Angle);
			Lines.AddLine(Previous, Current, Color);
			Previous = Current;
		}
	}

	bool GetConstraintWorldFrames(const USkeletalMeshComponent& Component, const FPhysicsAssetConstraint& Constraint,
		int32 Parent, int32 Child, physx::PxTransform& ParentFrame, physx::PxTransform& ChildFrame)
	{
		const auto& Bones = Component.GetBones();
		const FVector Scale = Component.GetWorldScale3D();
		if (Parent < 0 || Child < 0 || Parent >= Bones.Num() || Child >= Bones.Num() ||
			!std::isfinite(Constraint.FrameOffset.Pitch) || !std::isfinite(Constraint.FrameOffset.Yaw) ||
			!std::isfinite(Constraint.FrameOffset.Roll)) return false;
		for (int32 Axis = 0; Axis < 3; ++Axis)
			if (!std::isfinite(Scale[Axis]) || Scale[Axis] <= 0.0f) return false;
		TArray<FMatrix> BindMatrices;
		TArray<physx::PxTransform> BindPoses;
		BindMatrices.SetNum(Bones.Num());
		BindPoses.SetNum(Bones.Num());
		// InitConstraints와 같은 바인드 포즈/스케일/축 정렬 규칙으로 로컬 프레임을 복원합니다.
		for (int32 Index = 0; Index < Bones.Num(); ++Index)
		{
			const int32 BoneParent = Bones[Index].ParentIndex;
			if (BoneParent >= Index || BoneParent < -1) return false;
			const FMatrix Local = Bones[Index].LocalBindPose.GetLocalMatrix();
			BindMatrices[Index] = BoneParent < 0 ? Local : Local * BindMatrices[BoneParent];
			const FTransform Bind(BindMatrices[Index]);
			BindPoses[Index] = K2PTransform(FTransform(Bind.Rotation, Bind.Location * Scale));
		}
		const physx::PxVec3 BoneAxis = K2PVector(FPhysicsBoneUtils::FindBoneAxis(Bones, Child));
		const physx::PxQuat Alignment = physx::PxShortestRotation(physx::PxVec3(1.0f, 0.0f, 0.0f), BoneAxis);
		const physx::PxQuat Offset = K2PQuat(Constraint.FrameOffset.Quaternion().Normalized());
		const physx::PxTransform JointBind(BindPoses[Child].p, (BindPoses[Child].q * Alignment * Offset).getNormalized());
		const FTransform LocalToWorld(Component.GetWorldRotation(), Component.GetWorldLocation(), Scale);
		ParentFrame = K2PTransform(Component.GetBoneTransform(Parent, LocalToWorld)) * (BindPoses[Parent].getInverse() * JointBind);
		ChildFrame = K2PTransform(Component.GetBoneTransform(Child, LocalToWorld)) * (BindPoses[Child].getInverse() * JointBind);
		return ParentFrame.isValid() && ChildFrame.isValid();
	}

	void DrawConstraintArc(FLineBatcher& Lines, const physx::PxTransform& Frame, const physx::PxVec3& Center,
		const physx::PxVec3& AxisA, const physx::PxVec3& AxisB, float Radius, float Start, float End, const FVector4& Color)
	{
		constexpr int32 Segments = 64;
		if (End <= Start) return;
		const auto PointAt = [&](float Angle)
		{
			return P2KVector(Frame.transform(Center + (AxisA * std::cos(Angle) + AxisB * std::sin(Angle)) * Radius));
		};
		FVector Previous = PointAt(Start);
		for (int32 Index = 1; Index <= Segments; ++Index)
		{
			const FVector Current = PointAt(Start + (End - Start) * static_cast<float>(Index) / Segments);
			Lines.AddLine(Previous, Current, Color);
			Previous = Current;
		}
	}

	void DrawConstraintLimits(FLineBatcher& Lines, const physx::PxTransform& ParentFrame,
		const physx::PxTransform& ChildFrame, const FPhysicsAssetConstraint& Constraint, float Radius)
	{
		if (!std::isfinite(Radius) || !std::isfinite(Constraint.Swing1LimitDeg) || !std::isfinite(Constraint.Swing2LimitDeg) ||
			!std::isfinite(Constraint.TwistMinDeg) || !std::isfinite(Constraint.TwistMaxDeg)) return;
		const physx::PxVec3 X(1.0f, 0.0f, 0.0f), Y(0.0f, 1.0f, 0.0f), Z(0.0f, 0.0f, 1.0f), Zero(0.0f);
		const FVector4 SwingColor(0.25f, 1.0f, 0.4f, 1.0f);
		const FVector4 TwistColor(0.25f, 0.7f, 1.0f, 1.0f);
		const FVector4 CurrentColor(1.0f, 0.85f, 0.1f, 1.0f);
		const FVector4 FreeColor(0.5f, 0.5f, 0.55f, 1.0f);
		const FVector4 LockedColor(1.0f, 0.3f, 0.3f, 1.0f);
		const FVector Origin = P2KVector(ParentFrame.p);
		const bool bLimited1 = Constraint.Swing1Motion == EConstraintMotion::Limited;
		const bool bLimited2 = Constraint.Swing2Motion == EConstraintMotion::Limited;
		const bool bLocked1 = Constraint.Swing1Motion == EConstraintMotion::Locked;
		const bool bLocked2 = Constraint.Swing2Motion == EConstraintMotion::Locked;
		const float Angle1 = FMath::Clamp(FMath::DegreesToRadians(Constraint.Swing1LimitDeg), 0.001f, PI - 0.001f);
		const float Angle2 = FMath::Clamp(FMath::DegreesToRadians(Constraint.Swing2LimitDeg), 0.001f, PI - 0.001f);
		const auto DrawFreeSwing = [&]()
		{
			DrawConstraintArc(Lines, ParentFrame, Zero, X, Y, Radius, 0.0f, 2.0f * PI, FreeColor);
			DrawConstraintArc(Lines, ParentFrame, Zero, X, Z, Radius, 0.0f, 2.0f * PI, FreeColor);
			DrawConstraintArc(Lines, ParentFrame, Zero, Y, Z, Radius, 0.0f, 2.0f * PI, FreeColor);
		};
		Lines.AddLine(Origin, P2KVector(ParentFrame.transform(X * Radius)), FreeColor);
		if (bLimited1 && bLimited2)
		{
			// tan(각도/4) 공간의 타원을 쿼터니언으로 옮겨 90도 이상의 제한도 유한하게 그립니다.
			const float TanY = std::tan(Angle1 * 0.25f), TanZ = std::tan(Angle2 * 0.25f);
			const auto Boundary = [&](float Angle)
			{
				const float A = TanY * std::cos(Angle), B = TanZ * std::sin(Angle);
				const float Squared = A * A + B * B;
				const float Inv = 1.0f / (1.0f + Squared);
				const physx::PxQuat Swing(0.0f, 2.0f * A * Inv, 2.0f * B * Inv, (1.0f - Squared) * Inv);
				return P2KVector(ParentFrame.transform(Swing.rotate(X) * Radius));
			};
			FVector Previous = Boundary(0.0f);
			for (int32 Index = 1; Index <= 64; ++Index)
			{
				const FVector Current = Boundary(2.0f * PI * static_cast<float>(Index) / 64.0f);
				Lines.AddLine(Previous, Current, SwingColor);
				if (Index % 8 == 0) Lines.AddLine(Origin, Current, SwingColor);
				Previous = Current;
			}
		}
		else if ((bLimited1 && !bLocked2) || (bLimited2 && !bLocked1))
		{
			// 한 Swing만 제한하고 다른 축은 Free이면 허용 띠의 양쪽 경계가 두 원뿔이 됩니다.
			const float Limit = FMath::Min(bLimited1 ? Angle1 : Angle2, PI * 0.5f);
			const physx::PxVec3 Axis = bLimited1 ? Z : Y;
			const physx::PxVec3 RingB = bLimited1 ? Y : Z;
			const float Height = Radius * std::sin(Limit), RingRadius = Radius * std::cos(Limit);
			// 단일 Swing 제한은 asin 범위이므로 90도 이상이면 방향 제한이 없습니다.
			if (Limit >= PI * 0.5f) DrawFreeSwing();
			else for (float Sign : { -1.0f, 1.0f })
			{
				const physx::PxVec3 Center = Axis * (Sign * Height);
				DrawConstraintArc(Lines, ParentFrame, Center, X, RingB, RingRadius, 0.0f, 2.0f * PI, SwingColor);
				for (const physx::PxVec3& Side : { X, -X, RingB, -RingB })
					Lines.AddLine(Origin, P2KVector(ParentFrame.transform(Center + Side * RingRadius)), SwingColor);
			}
		}
		else if (bLocked1 && bLocked2)
		{
			Lines.AddLine(Origin, P2KVector(ParentFrame.transform(X * Radius)), LockedColor);
		}
		else if (bLocked1 || bLocked2)
		{
			const float Limit = bLimited1 ? Angle1 : (bLimited2 ? Angle2 : PI);
			const physx::PxVec3 OtherAxis = bLocked2 ? -Z : Y;
			const FVector4& Color = bLimited1 || bLimited2 ? SwingColor : FreeColor;
			DrawConstraintArc(Lines, ParentFrame, Zero, X, OtherAxis, Radius, -Limit, Limit, Color);
			for (float Sign : { -1.0f, 1.0f })
				Lines.AddLine(Origin, P2KVector(ParentFrame.transform((X * std::cos(Limit) + OtherAxis * (Sign * std::sin(Limit))) * Radius)), Color);
		}
		else
		{
			DrawFreeSwing();
		}
		// 실제 자식 프레임의 X축은 부모 프레임의 허용 범위와 비교합니다.
		const FVector ChildOrigin = P2KVector(ChildFrame.p);
		Lines.AddLine(Origin, ChildOrigin, CurrentColor);
		Lines.AddLine(ChildOrigin, P2KVector(ChildFrame.transform(X * (Radius * 1.1f))), CurrentColor);

		// Swing과 겹치지 않게 Twist 다이얼은 기준 X축의 뒤쪽에 배치합니다.
		const physx::PxVec3 DialCenter = -X * (Radius * 0.3f);
		const FVector DialOrigin = P2KVector(ParentFrame.transform(DialCenter));
		const float DialRadius = Radius * 0.65f;
		const auto DialPoint = [&](float Angle)
		{
			return P2KVector(ParentFrame.transform(DialCenter + (Y * std::cos(Angle) + Z * std::sin(Angle)) * DialRadius));
		};
		float Lower = 0.0f, Upper = 0.0f;
		if (Constraint.TwistMotion == EConstraintMotion::Limited)
		{
			Lower = FMath::Clamp(FMath::DegreesToRadians(FMath::Min(Constraint.TwistMinDeg, Constraint.TwistMaxDeg)), -2.0f * PI + 0.001f, 2.0f * PI - 0.001f);
			Upper = FMath::Clamp(FMath::DegreesToRadians(FMath::Max(Constraint.TwistMinDeg, Constraint.TwistMaxDeg)), -2.0f * PI + 0.001f, 2.0f * PI - 0.001f);
		}
		else if (Constraint.TwistMotion == EConstraintMotion::Free) { Lower = -PI; Upper = PI; }
		const FVector4& DialColor = Constraint.TwistMotion == EConstraintMotion::Limited ? TwistColor :
			(Constraint.TwistMotion == EConstraintMotion::Locked ? LockedColor : FreeColor);
		DrawConstraintArc(Lines, ParentFrame, DialCenter, Y, Z, DialRadius, Lower, Upper, DialColor);
		Lines.AddLine(DialOrigin, DialPoint(Lower), DialColor);
		Lines.AddLine(DialOrigin, DialPoint(Upper), DialColor);
		Lines.AddLine(DialOrigin, DialPoint(0.0f), FreeColor);
		// 상대 회전을 Swing * Twist로 분리합니다. 180도 Swing 특이점에서는 Twist를 표시하지 않습니다.
		physx::PxQuat Relative = ParentFrame.q.getConjugate() * ChildFrame.q;
		if (Relative.w < 0.0f) Relative = -Relative;
		if (Relative.x * Relative.x + Relative.w * Relative.w > 1e-10f)
			Lines.AddLine(DialOrigin, DialPoint(2.0f * std::atan2(Relative.x, Relative.w)), CurrentColor);
	}

	void DrawAggregateGeometry(FLineBatcher& Lines, const FAggregateGeom& Geometry,
		const FMatrix& BoneToWorld, const FVector4& Color, const FShapeElem* SelectedShape, const FVector4& SelectionColor)
	{
		const FVector X(1.0f, 0.0f, 0.0f);
		const FVector Y(0.0f, 1.0f, 0.0f);
		const FVector Z(0.0f, 0.0f, 1.0f);

		for (const FSphereElem& Sphere : Geometry.SphereElems)
		{
			const FVector4 ShapeColor = &Sphere == SelectedShape ? SelectionColor : Color;
			if (!std::isfinite(Sphere.Radius) || Sphere.Radius <= 0.0f) continue;
			const FMatrix ShapeToWorld = FTransform(Sphere.Rotation, Sphere.Center).GetLocalMatrix() * BoneToWorld;
			DrawWireArc(Lines, ShapeToWorld, FVector::ZeroVector, X, Y, Sphere.Radius, 0.0f, 2.0f * PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, FVector::ZeroVector, Y, Z, Sphere.Radius, 0.0f, 2.0f * PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, FVector::ZeroVector, Z, X, Sphere.Radius, 0.0f, 2.0f * PI, ShapeColor);
		}

		for (const FBoxElem& Box : Geometry.BoxElems)
		{
			if (!std::isfinite(Box.X) || !std::isfinite(Box.Y) || !std::isfinite(Box.Z) ||
				Box.X <= 0.0f || Box.Y <= 0.0f || Box.Z <= 0.0f) continue;
			// FBoxElem의 X/Y/Z는 전체 길이입니다.
			const FVector HalfExtent(Box.X * 0.5f, Box.Y * 0.5f, Box.Z * 0.5f);
			const FMatrix ShapeToWorld = FTransform(Box.Rotation, Box.Center).GetLocalMatrix() * BoneToWorld;
			Lines.AddOrientedBox(FBox{ HalfExtent * -1.0f, HalfExtent }, ShapeToWorld,
				&Box == SelectedShape ? SelectionColor : Color);
		}

		for (const FCapsuleElem& Capsule : Geometry.CapsuleElems)
		{
			const FVector4 ShapeColor = &Capsule == SelectedShape ? SelectionColor : Color;
			if (!std::isfinite(Capsule.Radius) || !std::isfinite(Capsule.Length) ||
				Capsule.Radius <= 0.0f || Capsule.Length < 0.0f) continue;
			const FMatrix ShapeToWorld = FTransform(Capsule.Rotation, Capsule.Center).GetLocalMatrix() * BoneToWorld;
			// 캡슐은 로컬 Z축 방향이며 Length는 양쪽 반구 중심 사이의 거리입니다.
			const FVector Top = Z * (Capsule.Length * 0.5f);
			const FVector Bottom = Top * -1.0f;
			DrawWireArc(Lines, ShapeToWorld, Top, X, Y, Capsule.Radius, 0.0f, 2.0f * PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, Bottom, X, Y, Capsule.Radius, 0.0f, 2.0f * PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, Top, X, Z, Capsule.Radius, 0.0f, PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, Top, Y, Z, Capsule.Radius, 0.0f, PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, Bottom, X, Z, Capsule.Radius, 0.0f, -PI, ShapeColor);
			DrawWireArc(Lines, ShapeToWorld, Bottom, Y, Z, Capsule.Radius, 0.0f, -PI, ShapeColor);
			for (const FVector& Side : { X, X * -1.0f, Y, Y * -1.0f })
			{
				Lines.AddLine(ShapeToWorld.TransformPosition(Top + Side * Capsule.Radius),
					ShapeToWorld.TransformPosition(Bottom + Side * Capsule.Radius), ShapeColor);
			}
		}
	}
}

FPhysicsAssetEditor::FPhysicsAssetEditor(
	UEditorEngine& InEditorEngine,
	FEditorManager& InEditorManager,
	const FString& InAssetPath,
	USkeletalMesh& InPreviewMesh
)
	: FSkeletalPreviewEditor(InEditorEngine, InEditorManager, InAssetPath)
{
	Mesh = &InPreviewMesh;
}

FPhysicsAssetEditor::~FPhysicsAssetEditor() = default;

void FPhysicsAssetEditor::MarkModified()
{
	bDirty = true;
	bSaveFailed = false;
	SaveMessage.clear();
}

bool FPhysicsAssetEditor::Save()
{
	const auto Fail = [this](const FString& Message)
	{
		SaveMessage = Message;
		bSaveFailed = true;
		KRS_LOG(Warning, "[PhysicsAssetEditor] {}", Message);
		return false;
	};
	if (!PhysicsAsset) return Fail("There is no physics asset to save.");
	const FString PhysicsPath = FAssetUtils::ResolveAssetFilePath(GetAssetPath());
	if (PhysicsPath.empty()) return Fail("Cannot resolve the physics asset file path.");
	for (const UBodySetup* Body : PhysicsAsset->BodySetups)
		if (!Body) return Fail("Cannot save a physics asset containing a null body.");

	// 저장할 때는 화면에 보이는 기즈모 편집을 확정합니다. 취소나 시뮬레이션 포즈는 저장하지 않습니다.
	ShapeGizmo.EndDrag();
	if (PhysicsAsset->PreviewMeshPath.empty() && Mesh)
		PhysicsAsset->PreviewMeshPath = FAssetUtils::GetAssetKey(Mesh->GetPath());
	TArray<uint8> Bytes;
	if (!FJsonArchive::SaveObjectToBytes(PhysicsAsset, Bytes)) return Fail("Could not serialize the physics asset.");
	const std::filesystem::path Target = FPaths::FromUtf8(PhysicsPath);
	std::filesystem::path Temporary = Target;
	Temporary += std::wstring(L".tmp.") + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
	const auto RemoveTemporary = [&]()
	{
		std::error_code Error;
		std::filesystem::remove(Temporary, Error);
	};
	// 기존 로더와 같은 JSON을 사용하며, 임시 파일을 완전히 쓴 다음 교체합니다.
	std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
	if (!File) return Fail(FString("Cannot create the save file. Check write access: ") + PhysicsPath);
	File.write(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<std::streamsize>(Bytes.Num()));
	File.flush();
	File.close();
	if (!File)
	{
		RemoveTemporary();
		return Fail(FString("Writing the physics asset failed: ") + PhysicsPath);
	}
	if (!MoveFileExW(Temporary.c_str(), Target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD Error = GetLastError();
		RemoveTemporary();
		KRS_LOG(Warning, "[PhysicsAssetEditor] Could not replace save file. Windows error: {}", Error);
		return Fail(FString("Cannot replace the physics asset file. Check write access: ") + PhysicsPath);
	}
	SavedAssetBytes = std::move(Bytes);
	bDirty = false;
	bSaveFailed = false;
	SaveMessage = FString("Saved: ") + PhysicsPath;
	KRS_LOG(Info, "[PhysicsAssetEditor] Saved {}", PhysicsPath);
	return true;
}

bool FPhysicsAssetEditor::DiscardChanges()
{
	if (!PhysicsAsset || SavedAssetBytes.IsEmpty())
	{
		SaveMessage = "The saved physics asset snapshot is unavailable.";
		bSaveFailed = true;
		return false;
	}
	StopSimulation();
	SelectBone(-1);
	// 로드가 기존 BodySetup을 해제하므로 편집 뷰의 참조를 먼저 비웁니다.
	PreviewBodies.Empty();
	ClearEditingBodies(*PhysicsAsset);
	const bool bRestored = FJsonArchive::LoadObjectFromBytes(PhysicsAsset, SavedAssetBytes);
	RebuildBodyViews();
	if (!bRestored)
	{
		SaveMessage = "Could not restore the last saved physics asset. The editor will remain open.";
		bSaveFailed = true;
		return false;
	}
	bDirty = false;
	bSaveFailed = false;
	SaveMessage.clear();
	return true;
}

bool FPhysicsAssetEditor::ConfirmClose()
{
	ShapeGizmo.EndDrag();
	if (!IsDirty()) return true;
	const int Choice = MessageBoxW(GetActiveWindow(),
		L"Save changes to this physics asset before closing?\n\nYes: Save and close\nNo: Discard edits and close\nCancel: Keep editing",
		L"Unsaved Physics Asset", MB_YESNOCANCEL | MB_ICONQUESTION);
	if (Choice == IDCANCEL || Choice == 0) return false;
	if (Choice == IDYES && Save()) return true;
	if (Choice == IDNO && DiscardChanges()) return true;
	const std::wstring Message = FPaths::FromUtf8(SaveMessage).wstring();
	MessageBoxW(GetActiveWindow(), Message.c_str(), L"Physics Asset", MB_OK | MB_ICONERROR);
	return false;
}

void FPhysicsAssetEditor::RebuildBodyViews()
{
	PreviewBodies.Empty();
	if (!PhysicsAsset || !Mesh) return;
	for (UBodySetup* Setup : PhysicsAsset->BodySetups)
	{
		if (!Setup)
		{
			KRS_LOG(Warning, "[PhysicsAssetEditor] Skipped a null body setup.");
			continue;
		}
		const int32 BoneIndex = Mesh->FindBoneIndex(Setup->BoneName);
		if (BoneIndex < 0 || FindBodyByBone(BoneIndex) >= 0)
		{
			KRS_LOG(Warning, "[PhysicsAssetEditor] Skipped missing or duplicate body bone: {}", Setup->BoneName);
			continue;
		}
		PreviewBodies.Emplace(BoneIndex, Setup);
	}
	++TopologyRevision;
}

bool FPhysicsAssetEditor::CanStartSimulation() const
{
	return Simulation && MeshComponent && PreviewFloor && PhysicsAsset && !PreviewBodies.IsEmpty()
		&& (SimulationMode == ESimulationMode::All || GetSelectedBody() != nullptr);
}

void FPhysicsAssetEditor::SetSimulationMode(ESimulationMode Mode)
{
	if (IsSimulationActive() || (Mode != ESimulationMode::All && Mode != ESimulationMode::SelectedAndBelow)) return;
	SimulationMode = Mode;
	SimulationError.clear();
}

bool FPhysicsAssetEditor::StartSimulation()
{
	if (IsSimulationActive()) return true;
	CancelViewportInteraction();
	if (!Simulation || !MeshComponent || !PreviewFloor || !PhysicsAsset)
	{
		SimulationError = "The preview mesh or physics asset is unavailable.";
		return false;
	}
	int32 RootBoneIndex = INDEX_NONE;
	if (SimulationMode == ESimulationMode::SelectedAndBelow)
	{
		const FPhysicsAssetPreviewBody* SelectedBody = GetSelectedBody();
		if (!SelectedBody)
		{
			SimulationError = "Select a physics body to simulate it and its descendants.";
			return false;
		}
		RootBoneIndex = SelectedBody->BoneIndex;
	}
	const bool bStarted = Simulation->Start(*MeshComponent, *PreviewFloor, *PhysicsAsset, SimulationError, RootBoneIndex);
	SyncShapeGizmo();
	return bStarted;
}

void FPhysicsAssetEditor::StopSimulation()
{
	if (Simulation) Simulation->Stop();
	SimulationError.clear();
	SyncShapeGizmo();
	if (UWorld* World = PreviewScene.GetWorld()) World->GetScene().UpdateAllTransforms();
}

void FPhysicsAssetEditor::ToggleSimulation()
{
	if (IsSimulationActive()) StopSimulation();
	else StartSimulation();
}

bool FPhysicsAssetEditor::IsSimulationActive() const
{
	return Simulation && Simulation->IsActive();
}

void FPhysicsAssetEditor::OnRenderLine(FLineBatcher& LineBatcher)
{
	FSkeletalPreviewEditor::OnRenderLine(LineBatcher);
}

void FPhysicsAssetEditor::OnRenderDebugOverlay(const FSceneView* View)
{
	if (!View || !Mesh || !MeshComponent || !PhysicsBodyLines) return;
	PhysicsBodyLines->BeginFrame();
	// 물리 형상 전용 배치에서 깊이를 검사하고, 장면 깊이에는 쓰지 않습니다.
	PhysicsBodyLines->SetDepthTest(bBodyDepthTest, false);
	const FShapeElem* SelectedShape = GetSelectedShape();
	for (int32 Index = 0; Index < PreviewBodies.Num(); ++Index)
	{
		if (!IsBodyVisible(Index)) continue;
		const FPhysicsAssetPreviewBody& Body = PreviewBodies[Index];
		if (Body.BoneIndex < 0 || Body.BoneIndex >= Mesh->GetNumBones()) continue;
		const FVector4& Color = Index == SelectedBodyIndex ? SelectedBodyColor : BodyColor;
		DrawAggregateGeometry(*PhysicsBodyLines, Body.GetGeometry(), MeshComponent->GetBoneWorldMatrix(Body.BoneIndex),
			Color, SelectedShape, SelectedShapeColor);
	}
	if (PhysicsAsset)
	{
		for (int32 Index = 0; Index < PhysicsAsset->Constraints.Num(); ++Index)
		{
			const FPhysicsAssetConstraint& Constraint = PhysicsAsset->Constraints[Index];
			const int32 Parent = Mesh->FindBoneIndex(Constraint.ParentBone);
			const int32 Child = Mesh->FindBoneIndex(Constraint.ChildBone);
			if (Parent < 0 || Child < 0 || FindBodyByBone(Parent) < 0 || FindBodyByBone(Child) < 0) continue;
			const bool bSelected = Index == SelectedConstraintIndex;
			if (BodyDrawMode == EBodyDrawMode::None ||
				(BodyDrawMode == EBodyDrawMode::SelectedOnly && !bSelected &&
					SelectedBodyIndex != FindBodyByBone(Parent) && SelectedBodyIndex != FindBodyByBone(Child))) continue;
			const FVector ParentPosition = MeshComponent->GetBoneWorldMatrix(Parent).TransformPosition(FVector::ZeroVector);
			const FVector ChildPosition = MeshComponent->GetBoneWorldMatrix(Child).TransformPosition(FVector::ZeroVector);
			const FVector4 Color = bSelected ? SelectedShapeColor : FVector4(0.4f, 0.7f, 1.0f, 1.0f);
			// 전체 연결은 간단히 표시하고, 선택한 조인트에만 각도 제한을 표시합니다.
			PhysicsBodyLines->AddLine(ParentPosition, ChildPosition, Color);
			PhysicsBodyLines->AddSphere(ChildPosition, FMath::Max(BoneSphereSize, 0.005f), Color);
			if (bSelected)
			{
				physx::PxTransform ParentFrame(physx::PxIdentity), ChildFrame(physx::PxIdentity);
				if (GetConstraintWorldFrames(*MeshComponent, Constraint, Parent, Child, ParentFrame, ChildFrame))
				{
					const float Radius = FMath::Max(0.05f, FMath::Max(BoneSphereSize * 8.0f, (ChildPosition - ParentPosition).Size() * 0.6f));
					DrawConstraintLimits(*PhysicsBodyLines, ParentFrame, ChildFrame, Constraint, Radius);
				}
			}
		}
	}
	PhysicsBodyLines->OnRender(View->ViewProjectionMatrix);
}

bool FPhysicsAssetEditor::IsBodyVisible(int32 BodyIndex) const
{
	if (BodyIndex < 0 || BodyIndex >= PreviewBodies.Num() || BodyDrawMode == EBodyDrawMode::None) return false;
	if (BodyDrawMode == EBodyDrawMode::All || BodyIndex == SelectedBodyIndex) return true;
	if (const FPhysicsAssetConstraint* Constraint = GetSelectedConstraint())
	{
		const FString& BoneName = PreviewBodies[BodyIndex].BodySetup->BoneName;
		return BoneName == Constraint->ParentBone || BoneName == Constraint->ChildBone;
	}
	return false;
}

void FPhysicsAssetEditor::SetBodyDrawMode(EBodyDrawMode Mode)
{
	if (Mode == BodyDrawMode) return;
	CancelViewportInteraction();
	BodyDrawMode = Mode;
	SyncShapeGizmo();
}

TUniquePtr<IEditorTab> FPhysicsAssetEditor::CreateTab()
{
	return MakeUnique<FPhysicsAssetEditorTab>(*this);
}

bool FPhysicsAssetEditor::InitializeResources()
{
	if (!InitializePreview())
	{
		return false;
	}

	PreviewBodies.Empty();
	SelectedBodyIndex = -1;
	SelectedConstraintIndex = -1;
	PhysicsAsset = NewObject<UPhysicsAsset>();
	if (!PhysicsAsset) return false;
	const FString PhysicsFile = FAssetUtils::ResolveAssetFilePath(GetAssetPath());
	if (PhysicsFile.empty()) return false;
	std::error_code FileError;
	const bool bExistingFile = std::filesystem::exists(FPaths::FromUtf8(PhysicsFile), FileError);
	if (FileError) return false;
	if (bExistingFile)
	{
		std::ifstream File(FPaths::FromUtf8(PhysicsFile), std::ios::binary | std::ios::ate);
		if (!File) return false;
		const std::streamsize Size = File.tellg();
		if (Size <= 0 || Size > std::numeric_limits<int32>::max()) return false;
		TArray<uint8> Bytes;
		Bytes.SetNum(static_cast<int32>(Size));
		File.seekg(0);
		if (!File.read(reinterpret_cast<char*>(Bytes.GetData()), Size)) return false;
		if (!FJsonArchive::LoadObjectFromBytes(PhysicsAsset, Bytes)) return false;
	}
	PhysicsAsset->PreviewMeshPath = FAssetUtils::GetAssetKey(Mesh->GetPath());
	if (PhysicsAsset->PreviewMeshPath.empty()) return false;
	for (const UBodySetup* Body : PhysicsAsset->BodySetups)
		if (!Body) return false;
	if (!FJsonArchive::SaveObjectToBytes(PhysicsAsset, SavedAssetBytes)) return false;
	bDirty = !bExistingFile;
	bSaveFailed = false;
	RebuildBodyViews();
	PhysicsBodyLines = MakeUnique<FLineBatcher>();
	if (!PhysicsBodyLines->Init(nullptr, PreviewScene.GetWorld())) return false;
	ShapeGizmoTarget = Cast<USceneComponent>(PreviewScene.AddComponent(USceneComponent::StaticClass()));
	if (!ShapeGizmoTarget) return false;
	ShapeGizmoTarget->DetachFromParent();
	if (!InitializePreviewFloor()) return false;
	Simulation = MakeUnique<FPhysicsAssetSimulation>();
	return true;
}

bool FPhysicsAssetEditor::InitializePreviewFloor()
{
	UStaticMesh* FloorMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Cube");
	if (!FloorMesh || !MeshComponent) return false;
	const FBox Bounds = MeshComponent->CalcBounds();
	const FBox& FloorBounds = FloorMesh->GetMeshData().AABB;
	const FVector NativeSize = FloorBounds.Max - FloorBounds.Min;
	const FVector MeshSize = Bounds.Max - Bounds.Min;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (!std::isfinite(Bounds.Min[Axis]) || !std::isfinite(Bounds.Max[Axis]) ||
			!std::isfinite(NativeSize[Axis]) || !std::isfinite(MeshSize[Axis]) ||
			NativeSize[Axis] <= 0.0f || MeshSize[Axis] < 0.0f) return false;
	}
	// 바닥 윗면의 중심을 월드 원점에 맞추고, 캐릭터가 쓰러질 공간을 확보합니다.
	const float HalfExtent = FMath::Max(1.0f, FMath::Max(MeshSize.Z, FMath::Max(MeshSize.X, MeshSize.Y))) * 2.0f;
	const float Thickness = FMath::Max(0.05f, MeshSize.Z * 0.025f);
	const FVector FloorScale(HalfExtent * 2.0f / NativeSize.X, HalfExtent * 2.0f / NativeSize.Y, Thickness / NativeSize.Z);
	const FVector Center(0.0f, 0.0f, -Thickness * 0.5f);
	const FVector NativeCenter = (FloorBounds.Min + FloorBounds.Max) * 0.5f;
	PreviewFloor = Cast<UStaticMeshComponent>(PreviewScene.AddComponent(UStaticMeshComponent::StaticClass()));
	if (!PreviewFloor) return false;
	PreviewFloor->SetStaticMesh(FloorMesh);
	PreviewFloor->DetachFromParent();
	PreviewFloor->SetTransform(FTransform(FRotator(0.0f, 0.0f, 0.0f), Center - NativeCenter * FloorScale, FloorScale));
	PreviewFloor->bGenerateOverlapEvents = false;
	PreviewFloor->bBlockComponent = true;
	PreviewFloor->BodyInstance.bSimulatePhysics = false;
	PreviewFloor->BodyInstance.bEnableGravity = false;
	PreviewFloor->BodyInstance.CollisionEnabled = ECollisionEnabled::QueryAndPhysics;
	return true;
}

void FPhysicsAssetEditor::ShutdownResources()
{
	StopSimulation();
	Simulation.reset();
	PreviewFloor = nullptr;
	CancelViewportInteraction();
	ShapeGizmo.SetTarget(nullptr);
	ShapeGizmoTarget = nullptr;
	SelectedShapeType = EPreviewShapeType::None;
	SelectedShapeIndex = -1;
	SelectedBodyIndex = -1;
	PreviewBodies.Empty();
	SelectedConstraintIndex = -1;
	PhysicsBodyLines.reset();
	FSkeletalPreviewEditor::ShutdownResources();
	if (PhysicsAsset) ClearEditingBodies(*PhysicsAsset);
	delete PhysicsAsset;
	PhysicsAsset = nullptr;
}

int32 FPhysicsAssetEditor::FindBodyByBone(int32 BoneIndex) const
{
	for (int32 Index = 0; Index < PreviewBodies.Num(); ++Index)
		if (PreviewBodies[Index].BoneIndex == BoneIndex) return Index;
	return -1;
}

const FPhysicsAssetPreviewBody* FPhysicsAssetEditor::GetSelectedBody() const
{
	return SelectedBodyIndex >= 0 && SelectedBodyIndex < PreviewBodies.Num()
		? &PreviewBodies[SelectedBodyIndex] : nullptr;
}

int32 FPhysicsAssetEditor::GetNearestParentBody(int32 ChildBodyIndex) const
{
	if (!Mesh || ChildBodyIndex < 0 || ChildBodyIndex >= PreviewBodies.Num()) return -1;
	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	const int32 ChildBone = PreviewBodies[ChildBodyIndex].BoneIndex;
	if (ChildBone < 0 || ChildBone >= Bones.Num()) return -1;
	int32 Bone = Bones[ChildBone].ParentIndex;
	for (int32 Step = 0; Bone >= 0 && Bone < Bones.Num() && Step < Bones.Num(); ++Step)
	{
		const int32 BodyIndex = FindBodyByBone(Bone);
		if (BodyIndex >= 0) return BodyIndex;
		Bone = Bones[Bone].ParentIndex;
	}
	return -1;
}

int32 FPhysicsAssetEditor::FindConstraintForBody(int32 ChildBodyIndex) const
{
	if (!PhysicsAsset || ChildBodyIndex < 0 || ChildBodyIndex >= PreviewBodies.Num()) return -1;
	const FString& ChildName = PreviewBodies[ChildBodyIndex].BodySetup->BoneName;
	for (int32 Index = 0; Index < PhysicsAsset->Constraints.Num(); ++Index)
		if (PhysicsAsset->Constraints[Index].ChildBone == ChildName) return Index;
	return -1;
}

bool FPhysicsAssetEditor::CanAddConstraint(int32 ParentBodyIndex, int32 ChildBodyIndex) const
{
	return !IsSimulationActive() && PhysicsAsset && ParentBodyIndex >= 0 && ParentBodyIndex < PreviewBodies.Num() &&
		ChildBodyIndex >= 0 && ChildBodyIndex < PreviewBodies.Num() && ParentBodyIndex != ChildBodyIndex &&
		GetNearestParentBody(ChildBodyIndex) == ParentBodyIndex && FindConstraintForBody(ChildBodyIndex) < 0 &&
		PreviewBodies[ParentBodyIndex].GetGeometry().GetElementCount() > 0 &&
		PreviewBodies[ChildBodyIndex].GetGeometry().GetElementCount() > 0;
}

int32 FPhysicsAssetEditor::AddConstraint(int32 ParentBodyIndex, int32 ChildBodyIndex)
{
	if (!CanAddConstraint(ParentBodyIndex, ChildBodyIndex)) return -1;
	CancelViewportInteraction();
	const int32 Index = PhysicsAsset->Constraints.Num();
	FPhysicsAssetConstraint& Constraint = PhysicsAsset->Constraints.Emplace_GetRef();
	Constraint.ParentBone = PreviewBodies[ParentBodyIndex].BodySetup->BoneName;
	Constraint.ChildBone = PreviewBodies[ChildBodyIndex].BodySetup->BoneName;
	++TopologyRevision;
	MarkModified();
	SelectConstraint(Index);
	return Index;
}

bool FPhysicsAssetEditor::RemoveConstraint(int32 ConstraintIndex)
{
	if (IsSimulationActive() || !PhysicsAsset || ConstraintIndex < 0 || ConstraintIndex >= PhysicsAsset->Constraints.Num()) return false;
	if (SelectedConstraintIndex == ConstraintIndex) SelectedConstraintIndex = -1;
	else if (SelectedConstraintIndex > ConstraintIndex) --SelectedConstraintIndex;
	PhysicsAsset->Constraints.RemoveAt(static_cast<uint32>(ConstraintIndex));
	++TopologyRevision;
	MarkModified();
	return true;
}

void FPhysicsAssetEditor::SelectConstraint(int32 ConstraintIndex)
{
	CancelViewportInteraction();
	SelectedConstraintIndex = -1;
	SelectedBodyIndex = -1;
	SelectedShapeType = EPreviewShapeType::None;
	SelectedShapeIndex = -1;
	ShapeGizmo.SetTarget(nullptr);
	if (!PhysicsAsset || !Mesh || ConstraintIndex < 0 || ConstraintIndex >= PhysicsAsset->Constraints.Num()) return;
	SelectBone(Mesh->FindBoneIndex(PhysicsAsset->Constraints[ConstraintIndex].ChildBone));
	SelectedConstraintIndex = ConstraintIndex;
}

const FPhysicsAssetConstraint* FPhysicsAssetEditor::GetSelectedConstraint() const
{
	return PhysicsAsset && SelectedConstraintIndex >= 0 && SelectedConstraintIndex < PhysicsAsset->Constraints.Num()
		? &PhysicsAsset->Constraints[SelectedConstraintIndex] : nullptr;
}

bool FPhysicsAssetEditor::SetSelectedConstraintProperties(const FPhysicsAssetConstraint& Properties)
{
	if (IsSimulationActive() || !GetSelectedConstraint()) return false;
	const auto ValidMotion = [](EConstraintMotion Motion)
	{
		return Motion == EConstraintMotion::Locked || Motion == EConstraintMotion::Limited || Motion == EConstraintMotion::Free;
	};
	if (!ValidMotion(Properties.Swing1Motion) || !ValidMotion(Properties.Swing2Motion) || !ValidMotion(Properties.TwistMotion) ||
		!std::isfinite(Properties.Swing1LimitDeg) || !std::isfinite(Properties.Swing2LimitDeg) ||
		!std::isfinite(Properties.TwistMinDeg) || !std::isfinite(Properties.TwistMaxDeg) ||
		!std::isfinite(Properties.FrameOffset.Pitch) || !std::isfinite(Properties.FrameOffset.Yaw) ||
		!std::isfinite(Properties.FrameOffset.Roll)) return false;
	// 연결 대상 변경은 생성/삭제를 거치게 하고 선택한 제약의 설정만 갱신합니다.
	FPhysicsAssetConstraint& Constraint = PhysicsAsset->Constraints[SelectedConstraintIndex];
	const FPhysicsAssetConstraint Previous = Constraint;
	Constraint.Swing1Motion = Properties.Swing1Motion;
	Constraint.Swing2Motion = Properties.Swing2Motion;
	Constraint.TwistMotion = Properties.TwistMotion;
	Constraint.Swing1LimitDeg = FMath::Clamp(Properties.Swing1LimitDeg, 0.1f, 179.0f);
	Constraint.Swing2LimitDeg = FMath::Clamp(Properties.Swing2LimitDeg, 0.1f, 179.0f);
	Constraint.TwistMinDeg = FMath::Clamp(FMath::Min(Properties.TwistMinDeg, Properties.TwistMaxDeg), -179.0f, 179.0f);
	Constraint.TwistMaxDeg = FMath::Clamp(FMath::Max(Properties.TwistMinDeg, Properties.TwistMaxDeg), -179.0f, 179.0f);
	Constraint.FrameOffset = Properties.FrameOffset;
	if (Previous.Swing1Motion != Constraint.Swing1Motion || Previous.Swing2Motion != Constraint.Swing2Motion ||
		Previous.TwistMotion != Constraint.TwistMotion || Previous.Swing1LimitDeg != Constraint.Swing1LimitDeg ||
		Previous.Swing2LimitDeg != Constraint.Swing2LimitDeg || Previous.TwistMinDeg != Constraint.TwistMinDeg ||
		Previous.TwistMaxDeg != Constraint.TwistMaxDeg || Previous.FrameOffset.Pitch != Constraint.FrameOffset.Pitch ||
		Previous.FrameOffset.Yaw != Constraint.FrameOffset.Yaw || Previous.FrameOffset.Roll != Constraint.FrameOffset.Roll)
		MarkModified();
	return true;
}

int32 FPhysicsAssetEditor::AddBody(int32 BoneIndex, EPreviewShapeType ShapeType)
{
	if (IsSimulationActive() || !PhysicsAsset || !Mesh || !MeshComponent || BoneIndex < 0 || BoneIndex >= Mesh->GetNumBones() ||
		(ShapeType != EPreviewShapeType::Sphere && ShapeType != EPreviewShapeType::Box && ShapeType != EPreviewShapeType::Capsule))
		return -1;
	const int32 ExistingIndex = FindBodyByBone(BoneIndex);
	if (ExistingIndex >= 0)
	{
		SelectBody(ExistingIndex);
		return ExistingIndex;
	}

	// 재할당 전에 진행 중인 조작을 정리합니다. 선택은 포인터 대신 인덱스로 보관합니다.
	CancelViewportInteraction();
	UBodySetup* Setup = NewObject<UBodySetup>(PhysicsAsset);
	if (!Setup) return -1;
	Setup->BoneName = Mesh->GetRefSkeleton()[BoneIndex].Name;
	PhysicsAsset->BodySetups.Add(Setup);
	const int32 BodyIndex = PreviewBodies.Num();
	PreviewBodies.Emplace(BoneIndex, Setup);
	++TopologyRevision;
	MarkModified();
	AddShape(BodyIndex, ShapeType);
	return BodyIndex;
}

bool FPhysicsAssetEditor::RemoveBody(int32 BodyIndex)
{
	if (IsSimulationActive() || !PhysicsAsset || BodyIndex < 0 || BodyIndex >= PreviewBodies.Num()) return false;
	UBodySetup* Setup = PreviewBodies[BodyIndex].BodySetup;
	const int32 AssetIndex = PhysicsAsset->BodySetups.Find(Setup);
	if (AssetIndex < 0) return false;
	const FString BoneName = Setup->BoneName;
	CancelViewportInteraction();
	ShapeGizmo.SetTarget(nullptr);
	if (SelectedBodyIndex == BodyIndex)
	{
		SelectedBodyIndex = -1;
		SelectedShapeType = EPreviewShapeType::None;
		SelectedShapeIndex = -1;
	}
	else if (SelectedBodyIndex > BodyIndex)
	{
		--SelectedBodyIndex;
	}
	PreviewBodies.RemoveAt(static_cast<uint32>(BodyIndex));
	PhysicsAsset->BodySetups.RemoveAt(static_cast<uint32>(AssetIndex));
	for (int32 Index = PhysicsAsset->Constraints.Num() - 1; Index >= 0; --Index)
	{
		const FPhysicsAssetConstraint& Constraint = PhysicsAsset->Constraints[Index];
		if (Constraint.ChildBone == BoneName || Constraint.ParentBone == BoneName)
			RemoveConstraint(Index);
	}
	delete Setup;
	++TopologyRevision;
	MarkModified();
	SyncShapeGizmo();
	return true;
}

bool FPhysicsAssetEditor::GenerateBodies()
{
	if (IsSimulationActive() || !PhysicsAsset || !Mesh) return false;
	if (!PreviewBodies.IsEmpty())
	{
		const int Choice = MessageBoxW(GetActiveWindow(),
			L"Replace all existing bodies and constraints with generated ones?\n\nUnsaved edits can still be discarded when closing the editor.",
			L"Generate Physics Asset", MB_OKCANCEL | MB_ICONQUESTION);
		if (Choice != IDOK) return false;
	}

	UPhysicsAsset* Generated = NewObject<UPhysicsAsset>();
	// 생성 에셋 직렬화 시 프리뷰 메시 경로 유지
	if (Generated)
	{
		Generated->PreviewMeshPath = PhysicsAsset->PreviewMeshPath;
	}
	TArray<FString> Warnings;
	TArray<uint8> Bytes;
	const bool bGenerated = Generated &&
		FPhysicsAssetGenerator::Generate(*Generated, FPhysicsAssetGenInput::FromMesh(*Mesh), FPhysicsAssetGenSettings{}, Warnings) &&
		FJsonArchive::SaveObjectToBytes(Generated, Bytes);
	delete Generated;
	for (const FString& Warning : Warnings) KRS_LOG(Warning, "[PhysicsAssetEditor] {}", Warning);
	if (!bGenerated)
	{
		SaveMessage = Warnings.IsEmpty() ? FString("Could not generate the physics asset.")
			: FString("Could not generate the physics asset: ") + Warnings.Last();
		bSaveFailed = true;
		return false;
	}

	CancelViewportInteraction();
	SelectBone(-1);
	OnBoneSelected();
	PreviewBodies.Empty();
	const bool bLoaded = FJsonArchive::LoadObjectFromBytes(PhysicsAsset, Bytes);
	RebuildBodyViews();
	MarkModified();
	SyncShapeGizmo();
	if (!bLoaded)
	{
		SaveMessage = "Could not apply the generated physics asset.";
		bSaveFailed = true;
		return false;
	}
	KRS_LOG(Info, "[PhysicsAssetEditor] Generated {} bodies and {} constraints",
		PhysicsAsset->BodySetups.Num(), PhysicsAsset->Constraints.Num());
	return true;
}

int32 FPhysicsAssetEditor::AddShape(int32 BodyIndex, EPreviewShapeType ShapeType)
{
	if (IsSimulationActive() || BodyIndex < 0 || BodyIndex >= PreviewBodies.Num() ||
		(ShapeType != EPreviewShapeType::Sphere && ShapeType != EPreviewShapeType::Box && ShapeType != EPreviewShapeType::Capsule))
		return -1;
	CancelViewportInteraction();
	ShapeGizmo.SetTarget(nullptr);
	FAggregateGeom& Geometry = PreviewBodies[BodyIndex].GetGeometry();
	const int32 ShapeIndex = GetShapeCount(Geometry, ShapeType);
	const float Radius = FMath::Max(BoneSphereSize * 2.0f, 0.01f);
	FShapeElem* Shape = nullptr;
	if (ShapeType == EPreviewShapeType::Sphere)
	{
		FSphereElem& Sphere = Geometry.SphereElems.Emplace_GetRef();
		Sphere.Radius = Radius;
		Shape = &Sphere;
	}
	else if (ShapeType == EPreviewShapeType::Box)
	{
		FBoxElem& Box = Geometry.BoxElems.Emplace_GetRef();
		Box.X = Box.Y = Box.Z = Radius * 2.0f;
		Shape = &Box;
	}
	else
	{
		FCapsuleElem& Capsule = Geometry.CapsuleElems.Emplace_GetRef();
		Capsule.Radius = Radius;
		Capsule.Length = Radius * 4.0f;
		Shape = &Capsule;
	}
	Shape->Center = FVector::ZeroVector;
	Shape->Rotation = FRotator(0.0f, 0.0f, 0.0f);
	++TopologyRevision;
	MarkModified();
	SelectShape(BodyIndex, ShapeType, ShapeIndex);
	return ShapeIndex;
}

bool FPhysicsAssetEditor::RemoveShape(int32 BodyIndex, EPreviewShapeType ShapeType, int32 ShapeIndex)
{
	if (IsSimulationActive() || BodyIndex < 0 || BodyIndex >= PreviewBodies.Num() || ShapeIndex < 0 ||
		ShapeIndex >= GetShapeCount(PreviewBodies[BodyIndex].GetGeometry(), ShapeType)) return false;
	CancelViewportInteraction();
	ShapeGizmo.SetTarget(nullptr);
	FAggregateGeom& Geometry = PreviewBodies[BodyIndex].GetGeometry();
	switch (ShapeType)
	{
	case EPreviewShapeType::Sphere: Geometry.SphereElems.RemoveAt(static_cast<uint32>(ShapeIndex)); break;
	case EPreviewShapeType::Box: Geometry.BoxElems.RemoveAt(static_cast<uint32>(ShapeIndex)); break;
	case EPreviewShapeType::Capsule: Geometry.CapsuleElems.RemoveAt(static_cast<uint32>(ShapeIndex)); break;
	default: return false;
	}
	if (SelectedBodyIndex == BodyIndex && SelectedShapeType == ShapeType)
	{
		if (SelectedShapeIndex == ShapeIndex)
		{
			SelectedShapeType = EPreviewShapeType::None;
			SelectedShapeIndex = -1;
		}
		else if (SelectedShapeIndex > ShapeIndex)
		{
			--SelectedShapeIndex;
		}
	}
	++TopologyRevision;
	MarkModified();
	SyncShapeGizmo();
	return true;
}

bool FPhysicsAssetEditor::SelectShape(int32 BodyIndex, EPreviewShapeType ShapeType, int32 ShapeIndex)
{
	if (BodyIndex < 0 || BodyIndex >= PreviewBodies.Num() || ShapeIndex < 0 ||
		ShapeIndex >= GetShapeCount(PreviewBodies[BodyIndex].GetGeometry(), ShapeType)) return false;
	SelectBody(BodyIndex);
	SelectedShapeType = ShapeType;
	SelectedShapeIndex = ShapeIndex;
	SyncShapeGizmo();
	return true;
}

void FPhysicsAssetEditor::SelectBody(int32 BodyIndex)
{
	CancelViewportInteraction();
	SelectedConstraintIndex = -1;
	SelectedBodyIndex = -1;
	SelectedShapeType = EPreviewShapeType::None;
	SelectedShapeIndex = -1;
	ShapeGizmo.SetTarget(nullptr);
	if (BodyIndex < 0 || BodyIndex >= PreviewBodies.Num()) return;
	const FPhysicsAssetPreviewBody& Body = PreviewBodies[BodyIndex];
	// SelectBone의 훅에서 이전 선택을 정리한 후 새 바디를 선택합니다.
	SelectBone(Body.BoneIndex);
	SelectedBodyIndex = BodyIndex;
	if (!Body.GetGeometry().CapsuleElems.IsEmpty()) SelectedShapeType = EPreviewShapeType::Capsule;
	else if (!Body.GetGeometry().BoxElems.IsEmpty()) SelectedShapeType = EPreviewShapeType::Box;
	else if (!Body.GetGeometry().SphereElems.IsEmpty()) SelectedShapeType = EPreviewShapeType::Sphere;
	if (SelectedShapeType != EPreviewShapeType::None) SelectedShapeIndex = 0;
	SyncShapeGizmo();
}

FShapeElem* FPhysicsAssetEditor::GetSelectedShape()
{
	if (!GetSelectedBody() || SelectedShapeIndex < 0) return nullptr;
	FAggregateGeom& Geometry = PreviewBodies[SelectedBodyIndex].GetGeometry();
	switch (SelectedShapeType)
	{
	case EPreviewShapeType::Sphere:
		return SelectedShapeIndex < Geometry.SphereElems.Num() ? &Geometry.SphereElems[SelectedShapeIndex] : nullptr;
	case EPreviewShapeType::Box:
		return SelectedShapeIndex < Geometry.BoxElems.Num() ? &Geometry.BoxElems[SelectedShapeIndex] : nullptr;
	case EPreviewShapeType::Capsule:
		return SelectedShapeIndex < Geometry.CapsuleElems.Num() ? &Geometry.CapsuleElems[SelectedShapeIndex] : nullptr;
	default: return nullptr;
	}
}

const FShapeElem* FPhysicsAssetEditor::GetSelectedShape() const
{
	const FPhysicsAssetPreviewBody* Body = GetSelectedBody();
	if (!Body || SelectedShapeIndex < 0) return nullptr;
	const FAggregateGeom& Geometry = Body->GetGeometry();
	switch (SelectedShapeType)
	{
	case EPreviewShapeType::Sphere:
		return SelectedShapeIndex < Geometry.SphereElems.Num() ? &Geometry.SphereElems[SelectedShapeIndex] : nullptr;
	case EPreviewShapeType::Box:
		return SelectedShapeIndex < Geometry.BoxElems.Num() ? &Geometry.BoxElems[SelectedShapeIndex] : nullptr;
	case EPreviewShapeType::Capsule:
		return SelectedShapeIndex < Geometry.CapsuleElems.Num() ? &Geometry.CapsuleElems[SelectedShapeIndex] : nullptr;
	default: return nullptr;
	}
}

FVector FPhysicsAssetEditor::GetSelectedShapeSize() const
{
	if (!GetSelectedShape()) return FVector::ZeroVector;
	const FAggregateGeom& Geometry = PreviewBodies[SelectedBodyIndex].GetGeometry();
	switch (SelectedShapeType)
	{
	case EPreviewShapeType::Sphere:
		return FVector(Geometry.SphereElems[SelectedShapeIndex].Radius, 0.0f, 0.0f);
	case EPreviewShapeType::Box:
	{
		const FBoxElem& Box = Geometry.BoxElems[SelectedShapeIndex];
		return FVector(Box.X, Box.Y, Box.Z);
	}
	case EPreviewShapeType::Capsule:
	{
		const FCapsuleElem& Capsule = Geometry.CapsuleElems[SelectedShapeIndex];
		return FVector(Capsule.Radius, Capsule.Length, 0.0f);
	}
	default: return FVector::ZeroVector;
	}
}

void FPhysicsAssetEditor::SetSelectedShapeSize(const FVector& Size)
{
	if (!GetSelectedShape()) return;
	FAggregateGeom& Geometry = PreviewBodies[SelectedBodyIndex].GetGeometry();
	switch (SelectedShapeType)
	{
	case EPreviewShapeType::Sphere:
		Geometry.SphereElems[SelectedShapeIndex].Radius = Size.X;
		break;
	case EPreviewShapeType::Box:
	{
		FBoxElem& Box = Geometry.BoxElems[SelectedShapeIndex];
		Box.X = Size.X; Box.Y = Size.Y; Box.Z = Size.Z;
		break;
	}
	case EPreviewShapeType::Capsule:
		Geometry.CapsuleElems[SelectedShapeIndex].Radius = Size.X;
		Geometry.CapsuleElems[SelectedShapeIndex].Length = Size.Y;
		break;
	default: break;
	}
}

bool FPhysicsAssetEditor::GetSelectedShapeProperties(FShapeElem& OutShape, FVector& OutSize, EPreviewShapeType& OutType) const
{
	const FShapeElem* Shape = GetSelectedShape();
	if (!Shape) return false;
	OutShape = *Shape;
	OutSize = GetSelectedShapeSize();
	OutType = SelectedShapeType;
	return true;
}

bool FPhysicsAssetEditor::SetSelectedShapeProperties(const FShapeElem& Shape, const FVector& Size)
{
	if (IsSimulationActive() || !GetSelectedShape() ||
		!std::isfinite(Shape.Center.X) || !std::isfinite(Shape.Center.Y) || !std::isfinite(Shape.Center.Z) ||
		!std::isfinite(Shape.Rotation.Pitch) || !std::isfinite(Shape.Rotation.Yaw) || !std::isfinite(Shape.Rotation.Roll) ||
		!std::isfinite(Size.X) || !std::isfinite(Size.Y) || !std::isfinite(Size.Z))
		return false;

	FVector ValidSize = Size;
	constexpr float MinDimension = 0.001f;
	switch (SelectedShapeType)
	{
	case EPreviewShapeType::Sphere:
		ValidSize = FVector(FMath::Max(Size.X, MinDimension), 0.0f, 0.0f);
		break;
	case EPreviewShapeType::Box:
		ValidSize = FVector(FMath::Max(Size.X, MinDimension), FMath::Max(Size.Y, MinDimension), FMath::Max(Size.Z, MinDimension));
		break;
	case EPreviewShapeType::Capsule:
		ValidSize = FVector(FMath::Max(Size.X, MinDimension), FMath::Max(Size.Y, 0.0f), 0.0f);
		break;
	default: return false;
	}

	// 디테일과 기즈모가 같은 형상을 수정하므로 이전 드래그 상태를 먼저 정리합니다.
	CancelViewportInteraction();
	const bool bChanged = !ShapePropertiesEqual(*GetSelectedShape(), GetSelectedShapeSize(), Shape, ValidSize);
	*GetSelectedShape() = Shape;
	SetSelectedShapeSize(ValidSize);
	if (bChanged) MarkModified();
	SyncShapeGizmo();
	return true;
}

void FPhysicsAssetEditor::SetGizmoMode(EGizmoMode Mode)
{
	if (IsSimulationActive() || ShapeGizmo.GetMode() == Mode) return;
	CancelViewportInteraction();
	ShapeGizmo.SetMode(Mode);
	SyncShapeGizmo();
}

void FPhysicsAssetEditor::CancelViewportInteraction()
{
	if (ShapeGizmo.IsUsing())
	{
		if (FShapeElem* Shape = GetSelectedShape())
		{
			*Shape = DragStartShape;
			SetSelectedShapeSize(DragStartSize);
			bDirty = bDirtyBeforeGizmo;
		}
	}
	ShapeGizmo.EndDrag();
}

void FPhysicsAssetEditor::OnBoneSelected()
{
	CancelViewportInteraction();
	SelectedConstraintIndex = -1;
	SelectedBodyIndex = -1;
	SelectedShapeType = EPreviewShapeType::None;
	SelectedShapeIndex = -1;
	ShapeGizmo.SetTarget(nullptr);
}

void FPhysicsAssetEditor::TickAfterWorld(float DeltaTime)
{
	if (IsSimulationActive())
	{
		Simulation->Tick(DeltaTime);
		if (UWorld* World = PreviewScene.GetWorld()) World->GetScene().UpdateAllTransforms();
		if (!UpdateShapePicking()) UpdateBonePicking();
		return;
	}
	if (!UpdateShapeGizmo() && !UpdateShapePicking())
		UpdateBonePicking();
	SyncShapeGizmo();
}

bool FPhysicsAssetEditor::UpdateShapePicking()
{
	if (!CanProcessViewportInput() || !Mesh || !MeshComponent || !PointerInput.bHovered ||
		!PointerInput.bMousePressed || PointerInput.bCancel || CameraInput.bCaptured) return false;

	const FVector2 MousePosition(PointerInput.MouseUV.X * ViewportWidth, PointerInput.MouseUV.Y * ViewportHeight);
	const FRay WorldRay = ViewportClient->DeprojectScreenToWorld(MousePosition);
	float Nearest = FLT_MAX;
	int32 HitBodyIndex = -1;
	EPreviewShapeType HitShapeType = EPreviewShapeType::None;
	int32 HitShapeIndex = -1;
	for (int32 BodyIndex = 0; BodyIndex < PreviewBodies.Num(); ++BodyIndex)
	{
		if (!IsBodyVisible(BodyIndex)) continue;
		const FPhysicsAssetPreviewBody& Body = PreviewBodies[BodyIndex];
		if (Body.BoneIndex < 0 || Body.BoneIndex >= Mesh->GetNumBones()) continue;
		const FMatrix BoneToWorld = MeshComponent->GetBoneWorldMatrix(Body.BoneIndex);
		const auto TestShape = [&](const FShapeElem& Shape, EPreviewShapeType Type, int32 Index, const auto& Trace)
		{
			const FMatrix ShapeToWorld = FTransform(Shape.Rotation, Shape.Center).GetLocalMatrix() * BoneToWorld;
			const float Determinant = ShapeToWorld.Determinant();
			if (!std::isfinite(Determinant) || std::abs(Determinant) < 1e-12f) return;
			float T;
			if (Trace(ToLocalRay(WorldRay, ShapeToWorld), T) && std::isfinite(T) && T >= 0.0f && T < Nearest)
			{
				Nearest = T;
				HitBodyIndex = BodyIndex;
				HitShapeType = Type;
				HitShapeIndex = Index;
			}
		};
		for (int32 Index = 0; Index < Body.GetGeometry().SphereElems.Num(); ++Index)
		{
			const FSphereElem& Sphere = Body.GetGeometry().SphereElems[Index];
			if (!std::isfinite(Sphere.Radius) || Sphere.Radius <= 0.0f) continue;
			TestShape(Sphere, EPreviewShapeType::Sphere, Index, [&](const FRay& Ray, float& T)
			{ return RayHitsSphere(Ray, FVector::ZeroVector, Sphere.Radius, T); });
		}
		for (int32 Index = 0; Index < Body.GetGeometry().BoxElems.Num(); ++Index)
		{
			const FBoxElem& Box = Body.GetGeometry().BoxElems[Index];
			if (!std::isfinite(Box.X) || !std::isfinite(Box.Y) || !std::isfinite(Box.Z) ||
				Box.X <= 0.0f || Box.Y <= 0.0f || Box.Z <= 0.0f) continue;
			TestShape(Box, EPreviewShapeType::Box, Index, [&](const FRay& Ray, float& T)
			{ return RayHitsBox(Ray, FVector(Box.X, Box.Y, Box.Z) * 0.5f, T); });
		}
		for (int32 Index = 0; Index < Body.GetGeometry().CapsuleElems.Num(); ++Index)
		{
			const FCapsuleElem& Capsule = Body.GetGeometry().CapsuleElems[Index];
			if (!std::isfinite(Capsule.Radius) || !std::isfinite(Capsule.Length) ||
				Capsule.Radius <= 0.0f || Capsule.Length < 0.0f) continue;
			TestShape(Capsule, EPreviewShapeType::Capsule, Index, [&](const FRay& Ray, float& T)
			{ return RayHitsCapsule(Ray, Capsule.Radius, Capsule.Length, T); });
		}
	}
	if (HitBodyIndex >= 0) return SelectShape(HitBodyIndex, HitShapeType, HitShapeIndex);
	SelectBody(-1);
	return false;
}

FQuat FPhysicsAssetEditor::GetPreviewBoneWorldRotation() const
{
	FQuat Rotation = FQuat::Identity();
	const FPhysicsAssetPreviewBody* Body = GetSelectedBody();
	if (!Body || !Mesh || !MeshComponent) return Rotation;
	const TArray<FBoneInfo>& Bones = Mesh->GetRefSkeleton();
	for (int32 Index = Body->BoneIndex; Index >= 0 && Index < Bones.Num(); Index = Bones[Index].ParentIndex)
		Rotation = MeshComponent->GetBoneLocalTransform(Index).Rotation * Rotation;
	return (MeshComponent->GetWorldRotation().Quaternion() * Rotation).Normalized();
}

void FPhysicsAssetEditor::SyncShapeGizmo()
{
	if (IsSimulationActive())
	{
		ShapeGizmo.SetTarget(nullptr);
		return;
	}
	const FShapeElem* Shape = GetSelectedShape();
	const FPhysicsAssetPreviewBody* Body = GetSelectedBody();
	if (!Shape || !Body || !IsBodyVisible(SelectedBodyIndex) || !ShapeGizmoTarget || !Mesh || !MeshComponent ||
		Body->BoneIndex < 0 || Body->BoneIndex >= Mesh->GetNumBones())
	{
		ShapeGizmo.SetTarget(nullptr);
		return;
	}
	if (ShapeGizmo.IsUsing()) return;
	const FVector Location = MeshComponent->GetBoneWorldMatrix(Body->BoneIndex).TransformPosition(Shape->Center);
	const FQuat Rotation = (GetPreviewBoneWorldRotation() * Shape->Rotation.Quaternion()).Normalized();
	// 기즈모 스케일은 드래그 배율이며, 실제 크기는 형상의 Radius/Length/X/Y/Z에 저장합니다.
	ShapeGizmoTarget->SetTransform(FTransform(Rotation, Location));
	ShapeGizmo.SetTarget(ShapeGizmoTarget);
}

bool FPhysicsAssetEditor::UpdateShapeGizmo()
{
	if (!CanProcessViewportInput() || PointerInput.bCancel)
	{
		const bool bWasUsing = ShapeGizmo.IsUsing();
		CancelViewportInteraction();
		return bWasUsing;
	}
	SyncShapeGizmo();
	if (!ShapeGizmo.GetTarget()) return false;
	const bool bWasUsing = ShapeGizmo.IsUsing();
	if ((!PointerInput.bHovered && !bWasUsing) || CameraInput.bCaptured) return false;

	const FVector2 MousePosition(PointerInput.MouseUV.X * ViewportWidth, PointerInput.MouseUV.Y * ViewportHeight);
	ShapeGizmo.Update(ViewportClient->DeprojectScreenToWorld(MousePosition), MousePosition,
		ViewportClient->GetViewProjectionMatrix(), static_cast<int>(ViewportWidth), static_cast<int>(ViewportHeight),
		PointerInput.bMouseDown, ViewportClient->GetViewLocation(), !ViewportClient->IsPerspective(), PointerInput.bMousePressed);
	if (!bWasUsing && ShapeGizmo.IsUsing())
	{
		const FMatrix BoneToWorld = MeshComponent->GetBoneWorldMatrix(GetSelectedBody()->BoneIndex);
		if (!std::isfinite(BoneToWorld.Determinant()) || std::abs(BoneToWorld.Determinant()) < 1e-12f)
		{
			ShapeGizmo.EndDrag();
			return true;
		}
		DragStartShape = *GetSelectedShape();
		DragStartSize = GetSelectedShapeSize();
		bDirtyBeforeGizmo = bDirty;
		DragScaleAxis = ShapeGizmo.GetHoveredAxis();
		DragBoneInverse = BoneToWorld.Inverse();
		DragBoneRotation = GetPreviewBoneWorldRotation();
	}
	// Update()가 버튼을 놓는 프레임에 드래그를 끝내므로 마지막 변환도 반영합니다.
	if (bWasUsing || ShapeGizmo.IsUsing()) ApplyShapeGizmoTransform();
	return bWasUsing || ShapeGizmo.IsUsing() || ShapeGizmo.GetHoveredAxis() >= 0;
}

void FPhysicsAssetEditor::ApplyShapeGizmoTransform()
{
	FShapeElem* Shape = GetSelectedShape();
	if (!Shape || !ShapeGizmoTarget) return;
	const FShapeElem Before = *Shape;
	const FVector SizeBefore = GetSelectedShapeSize();
	switch (ShapeGizmo.GetMode())
	{
	case EGizmoMode::Location:
		Shape->Center = DragBoneInverse.TransformPosition(ShapeGizmoTarget->GetWorldLocation());
		break;
	case EGizmoMode::Rotation:
		Shape->Rotation = (DragBoneRotation.Inverse() * ShapeGizmoTarget->GetWorldRotation().Quaternion()).Normalized().ToFRotator();
		break;
	case EGizmoMode::Scale:
	{
		const FVector RawScale = ShapeGizmoTarget->GetRelativeScale3D();
		if (!std::isfinite(RawScale.X) || !std::isfinite(RawScale.Y) || !std::isfinite(RawScale.Z)) return;
		const FVector Scale(FMath::Max(RawScale.X, 0.01f), FMath::Max(RawScale.Y, 0.01f), FMath::Max(RawScale.Z, 0.01f));
		FVector Size = DragStartSize;
		if (SelectedShapeType == EPreviewShapeType::Sphere)
			Size.X = FMath::Max(0.001f, Size.X * Scale[DragScaleAxis >= 0 && DragScaleAxis < 3 ? DragScaleAxis : 0]);
		else if (SelectedShapeType == EPreviewShapeType::Box)
			Size = FVector(FMath::Max(0.001f, Size.X * Scale.X), FMath::Max(0.001f, Size.Y * Scale.Y), FMath::Max(0.001f, Size.Z * Scale.Z));
		else if (SelectedShapeType == EPreviewShapeType::Capsule)
		{
			// X 또는 Y 핸들은 원형 단면 전체를 조절하고, Z 핸들은 높이만 조절합니다.
			const float RadiusScale = DragScaleAxis == 2 ? 1.0f : (DragScaleAxis == 1 ? Scale.Y : Scale.X);
			Size.X = FMath::Max(0.001f, DragStartSize.X * RadiusScale);
			const float HalfHeight = (DragStartSize.Y * 0.5f + DragStartSize.X) * Scale.Z;
			Size.Y = FMath::Max(0.0f, 2.0f * (HalfHeight - Size.X));
		}
		if (std::isfinite(Size.X) && std::isfinite(Size.Y) && std::isfinite(Size.Z))
			SetSelectedShapeSize(Size);
		break;
	}
	default: break;
	}
	if (!ShapePropertiesEqual(Before, SizeBefore, *Shape, GetSelectedShapeSize())) MarkModified();
}
