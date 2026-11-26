#include "EnginePCH.h"
#include "Render/StockingMaterial.h"
#include "Render/Material.h"
#include "Render/MaterialInstance.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	int32 AddGraphNode(UMaterial& Mat, int32& NextId, EMaterialExpressionType Type, const FVector2& Pos)
	{
		FMaterialExpressionNode Node{};
		Node.Id = ++NextId;
		Node.Type = Type;
		Node.Position = Pos;
		Mat.ExpressionNodes.Add(Node);
		return Node.Id;
	}

	FMaterialExpressionNode* FindMutableNode(UMaterial& Mat, int32 NodeId)
	{
		for (int32 i = 0; i < Mat.ExpressionNodes.Num(); ++i)
		{
			if (Mat.ExpressionNodes[i].Id == NodeId)
			{
				return &Mat.ExpressionNodes[i];
			}
		}
		return nullptr;
	}

	int32 AddVectorParamNode(UMaterial& Mat, int32& NextId, const char* Name, const FVector4& DefaultValue, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::VectorParameter, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->ParameterName = Name;
			Node->ParameterGroup = "Stocking";
			Node->VectorValue = DefaultValue;
		}
		return Id;
	}

	int32 AddScalarParamNode(UMaterial& Mat, int32& NextId, const char* Name, float DefaultValue, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::ScalarParameter, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->ParameterName = Name;
			Node->ParameterGroup = "Stocking";
			Node->ScalarValue = DefaultValue;
		}
		return Id;
	}

	int32 AddTextureParamNode(UMaterial& Mat, int32& NextId, const char* Name, bool bSRGB, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::TextureSampleParameter2D, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->ParameterName = Name;
			Node->ParameterGroup = "Stocking";
			Node->bSRGB = bSRGB;
		}
		return Id;
	}

	int32 AddConstantNode(UMaterial& Mat, int32& NextId, float Value, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::Constant, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->ScalarValue = Value;
		}
		return Id;
	}

	int32 AddConstantVec3Node(UMaterial& Mat, int32& NextId, const FVector& Value, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::Constant3Vector, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->VectorValue = FVector4(Value.X, Value.Y, Value.Z, 1.0f);
		}
		return Id;
	}

	int32 AddMaskNode(UMaterial& Mat, int32& NextId, uint8 MaskBits, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::ComponentMask, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->ComponentMask = MaskBits;
		}
		return Id;
	}

	int32 AddCustomExprNode(UMaterial& Mat, int32& NextId, const char* Code, EMaterialValueType OutType, const FVector2& Pos)
	{
		const int32 Id = AddGraphNode(Mat, NextId, EMaterialExpressionType::Custom, Pos);
		if (FMaterialExpressionNode* Node = FindMutableNode(Mat, Id))
		{
			Node->CustomCode = Code;
			Node->CustomOutputType = OutType;
		}
		return Id;
	}

	void ConnectGraphPins(UMaterial& Mat, int32 OutNode, int32 InNode, int32 InPin)
	{
		FMaterialExpressionConnection Conn{};
		Conn.OutputNodeId = OutNode;
		Conn.OutputPin = 0;
		Conn.InputNodeId = InNode;
		Conn.InputPin = InPin;
		Mat.ExpressionConnections.Add(Conn);
	}
}

UMaterial* FStockingMaterialLibrary::GetOrCreateStockingMasterMaterial()
{
	static UMaterial* CachedMaster = nullptr;
	if (CachedMaster && IsValid(CachedMaster))
	{
		return CachedMaster;
	}

	UMaterial* Mat = NewObject<UMaterial>();
	if (!Mat)
	{
		return nullptr;
	}

	Mat->SetPath("Engine/Materials/M_StockingMaster_Lit");
	Mat->SetUsage(EMaterialUsage::StaticMesh, true);
	Mat->SetUsage(EMaterialUsage::SkeletalMesh, true);
	Mat->SetBlendState(EBlendState::AlphaBlend);
	Mat->SamplerState = ESamplerState::LinearWrap;
	Mat->BaseColor = FVector4(0.035f, 0.030f, 0.045f, 0.45f);

	int32 NextId = 0;
	const int32 OutputId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::MaterialOutput, FVector2(1800.0f, 240.0f));
	Mat->OutputNodeId = OutputId;

	// 패킹된 벡터 파라미터 정의
	const int32 FabricColorParamId = AddVectorParamNode(*Mat, NextId, "FabricColor", FVector4(0.035f, 0.030f, 0.045f, 1.0f), FVector2(0.0f, -240.0f));
	const int32 HemColorParamId = AddVectorParamNode(*Mat, NextId, "HemColor", FVector4(0.018f, 0.016f, 0.024f, 1.0f), FVector2(0.0f, -120.0f));
	const int32 OpacityParamsId = AddVectorParamNode(*Mat, NextId, "OpacityParams", FVector4(0.42f, 0.52f, 2.4f, 0.06f), FVector2(0.0f, 0.0f));
	const int32 StretchParamsId = AddVectorParamNode(*Mat, NextId, "StretchParams", FVector4(2.6f, 0.48f, 140.0f, 0.14f), FVector2(0.0f, 120.0f));
	const int32 DetailParamsId = AddVectorParamNode(*Mat, NextId, "DetailParams", FVector4(0.36f, 0.28f, 28.0f, 0.65f), FVector2(0.0f, 240.0f));
	const int32 SheenParamsId = AddVectorParamNode(*Mat, NextId, "SheenParams", FVector4(0.14f, 0.85f, 0.0f, 0.0f), FVector2(0.0f, 360.0f));
	const int32 TearStyleParamsId = AddVectorParamNode(*Mat, NextId, "TearStyleParams", FVector4(0.72f, 0.58f, 0.85f, 0.0f), FVector2(0.0f, 440.0f));
	const int32 TearMaskActiveId = AddScalarParamNode(*Mat, NextId, "TearMaskActive", 0.0f, FVector2(0.0f, 520.0f));

	// 각 스칼라 성분 분리
	const int32 FabricRgbId = AddMaskNode(*Mat, NextId, 0x07, FVector2(220.0f, -240.0f));
	const int32 HemRgbId = AddMaskNode(*Mat, NextId, 0x07, FVector2(220.0f, -120.0f));
	ConnectGraphPins(*Mat, FabricColorParamId, FabricRgbId, 0);
	ConnectGraphPins(*Mat, HemColorParamId, HemRgbId, 0);

	const int32 BaseOpacityId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, -20.0f));
	const int32 FresnelOpacityId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 20.0f));
	const int32 FresnelPowerId = AddMaskNode(*Mat, NextId, 0x04, FVector2(220.0f, 60.0f));
	const int32 MinOpacityId = AddMaskNode(*Mat, NextId, 0x08, FVector2(220.0f, 100.0f));
	ConnectGraphPins(*Mat, OpacityParamsId, BaseOpacityId, 0);
	ConnectGraphPins(*Mat, OpacityParamsId, FresnelOpacityId, 0);
	ConnectGraphPins(*Mat, OpacityParamsId, FresnelPowerId, 0);
	ConnectGraphPins(*Mat, OpacityParamsId, MinOpacityId, 0);

	const int32 StretchSensId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, 150.0f));
	const int32 StretchDropId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 190.0f));
	const int32 WeaveScaleId = AddMaskNode(*Mat, NextId, 0x04, FVector2(220.0f, 230.0f));
	const int32 WeaveStrengthId = AddMaskNode(*Mat, NextId, 0x08, FVector2(220.0f, 270.0f));
	ConnectGraphPins(*Mat, StretchParamsId, StretchSensId, 0);
	ConnectGraphPins(*Mat, StretchParamsId, StretchDropId, 0);
	ConnectGraphPins(*Mat, StretchParamsId, WeaveScaleId, 0);
	ConnectGraphPins(*Mat, StretchParamsId, WeaveStrengthId, 0);

	const int32 HemBandStrId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, 320.0f));
	const int32 SpecIntensityId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 360.0f));
	const int32 SpecPowerId = AddMaskNode(*Mat, NextId, 0x04, FVector2(220.0f, 400.0f));
	const int32 WrinkleShadingId = AddMaskNode(*Mat, NextId, 0x08, FVector2(220.0f, 440.0f));
	ConnectGraphPins(*Mat, DetailParamsId, HemBandStrId, 0);
	ConnectGraphPins(*Mat, DetailParamsId, SpecIntensityId, 0);
	ConnectGraphPins(*Mat, DetailParamsId, SpecPowerId, 0);
	ConnectGraphPins(*Mat, DetailParamsId, WrinkleShadingId, 0);

	const int32 SheenStrId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, 490.0f));
	const int32 HemFeatherPowId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 530.0f));
	ConnectGraphPins(*Mat, SheenParamsId, SheenStrId, 0);
	ConnectGraphPins(*Mat, SheenParamsId, HemFeatherPowId, 0);

	const int32 TearRimStrId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, 560.0f));
	const int32 TearContactStrId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 600.0f));
	const int32 TearRungStrId = AddMaskNode(*Mat, NextId, 0x04, FVector2(220.0f, 640.0f));
	ConnectGraphPins(*Mat, TearStyleParamsId, TearRimStrId, 0);
	ConnectGraphPins(*Mat, TearStyleParamsId, TearContactStrId, 0);
	ConnectGraphPins(*Mat, TearStyleParamsId, TearRungStrId, 0);

	// 정점 컬러 채널 분리 빨강 끝단 초록 높이 파랑 주름 알파 신장 비율
	const int32 VertexColorId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::VertexColor, FVector2(0.0f, 620.0f));
	const int32 EdgeMaskRawId = AddMaskNode(*Mat, NextId, 0x01, FVector2(220.0f, 570.0f));
	const int32 HeightNormId = AddMaskNode(*Mat, NextId, 0x02, FVector2(220.0f, 610.0f));
	const int32 CreaseRawId = AddMaskNode(*Mat, NextId, 0x04, FVector2(220.0f, 650.0f));
	const int32 StretchRatioId = AddMaskNode(*Mat, NextId, 0x08, FVector2(220.0f, 690.0f));
	ConnectGraphPins(*Mat, VertexColorId, EdgeMaskRawId, 0);
	ConnectGraphPins(*Mat, VertexColorId, HeightNormId, 0);
	ConnectGraphPins(*Mat, VertexColorId, CreaseRawId, 0);
	ConnectGraphPins(*Mat, VertexColorId, StretchRatioId, 0);

	// 선명한 허벅지 끝단 커트와 탄성 커프 밴드 마스크 계산
	const int32 CuffCutoffId = AddCustomExprNode(*Mat, NextId,
		"saturate(pow(max(A, 0.0001f), max(B, 0.1f)) * 12.0f)",
		EMaterialValueType::Scalar, FVector2(480.0f, 560.0f));
	ConnectGraphPins(*Mat, EdgeMaskRawId, CuffCutoffId, 0);
	ConnectGraphPins(*Mat, HemFeatherPowId, CuffCutoffId, 1);

	const int32 CuffBandId = AddCustomExprNode(*Mat, NextId,
		"(1.0f - saturate((A - 0.12f) * 2.2f)) * B",
		EMaterialValueType::Scalar, FVector2(700.0f, 560.0f));
	ConnectGraphPins(*Mat, EdgeMaskRawId, CuffBandId, 0);
	ConnectGraphPins(*Mat, CuffCutoffId, CuffBandId, 1);

	// 신장 초과 비율 계산
	const int32 StretchFactorId = AddCustomExprNode(*Mat, NextId,
		"saturate((A - 1.0f) * B)",
		EMaterialValueType::Scalar, FVector2(480.0f, 690.0f));
	ConnectGraphPins(*Mat, StretchRatioId, StretchFactorId, 0);
	ConnectGraphPins(*Mat, StretchSensId, StretchFactorId, 1);

	// 주름 편차 음수는 압축 관절과 골 양수는 능선
	const int32 CreaseSignedId = AddCustomExprNode(*Mat, NextId,
		"clamp((A - 0.5f) * 2.0f, -1.0f, 1.0f) * B",
		EMaterialValueType::Scalar, FVector2(480.0f, 630.0f));
	ConnectGraphPins(*Mat, CreaseRawId, CreaseSignedId, 0);
	ConnectGraphPins(*Mat, WrinkleShadingId, CreaseSignedId, 1);

	// 관절 압축부 비균일 미세 주름 파형과 법선 기울기 계산
	const int32 UvId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::TextureCoordinate, FVector2(0.0f, 780.0f));
	const int32 FoldPhaseId = AddCustomExprNode(*Mat, NextId,
		"A * 225.0f + sin(A * 41.0f + B.x * 9.0f) * 2.6f + sin(B.x * 21.0f - B.y * 13.0f) * 1.6f",
		EMaterialValueType::Scalar, FVector2(480.0f, 740.0f));
	ConnectGraphPins(*Mat, HeightNormId, FoldPhaseId, 0);
	ConnectGraphPins(*Mat, UvId, FoldPhaseId, 1);

	const int32 JointCompMaskId = AddCustomExprNode(*Mat, NextId,
		"pow(saturate((-A - 0.08f) * 1.95f), 1.45f) * saturate(B)",
		EMaterialValueType::Scalar, FVector2(700.0f, 650.0f));
	ConnectGraphPins(*Mat, CreaseSignedId, JointCompMaskId, 0);
	ConnectGraphPins(*Mat, WrinkleShadingId, JointCompMaskId, 1);

	const int32 MicroWrinkleSignalId = AddCustomExprNode(*Mat, NextId,
		"(sin(A) * 0.65f + sin(A * 1.73f + 0.9f) * 0.35f) * B",
		EMaterialValueType::Scalar, FVector2(700.0f, 740.0f));
	ConnectGraphPins(*Mat, FoldPhaseId, MicroWrinkleSignalId, 0);
	ConnectGraphPins(*Mat, JointCompMaskId, MicroWrinkleSignalId, 1);

	const int32 MicroFoldSlopeId = AddCustomExprNode(*Mat, NextId,
		"(cos(A) * 0.65f + cos(A * 1.73f + 0.9f) * 0.605f) * B * 0.62f",
		EMaterialValueType::Scalar, FVector2(700.0f, 790.0f));
	ConnectGraphPins(*Mat, FoldPhaseId, MicroFoldSlopeId, 0);
	ConnectGraphPins(*Mat, JointCompMaskId, MicroFoldSlopeId, 1);

	const int32 TotalCreaseId = AddCustomExprNode(*Mat, NextId,
		"clamp(A * 0.20f + B * 0.85f, -1.0f, 1.0f)",
		EMaterialValueType::Scalar, FVector2(880.0f, 700.0f));
	ConnectGraphPins(*Mat, CreaseSignedId, TotalCreaseId, 0);
	ConnectGraphPins(*Mat, MicroWrinkleSignalId, TotalCreaseId, 1);

	// 거친 체크무늬 대신 매끄러운 실크 표면과 미세 글리터 하이라이트 생성
	const int32 SilkGlitterId = AddCustomExprNode(*Mat, NextId,
		"pow(saturate(sin(A.x * B * 14.0f) * sin(A.y * B * 14.0f) * 0.6f + sin((A.x * 1.37f - A.y * 0.83f) * B * 19.0f) * 0.4f), 3.0f)",
		EMaterialValueType::Scalar, FVector2(480.0f, 850.0f));
	ConnectGraphPins(*Mat, UvId, SilkGlitterId, 0);
	ConnectGraphPins(*Mat, WeaveScaleId, SilkGlitterId, 1);

	const int32 WeaveFactorId = AddCustomExprNode(*Mat, NextId,
		"1.0f + (A - 0.25f) * B * 0.14f",
		EMaterialValueType::Scalar, FVector2(700.0f, 850.0f));
	ConnectGraphPins(*Mat, SilkGlitterId, WeaveFactorId, 0);
	ConnectGraphPins(*Mat, WeaveStrengthId, WeaveFactorId, 1);

	// 주름 기울기로 픽셀 법선을 기울여 입체 디퓨즈와 스페큘러 계산
	const int32 NormalWsId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::VertexNormalWS, FVector2(0.0f, 920.0f));
	const int32 PerturbedNormalId = AddCustomExprNode(*Mat, NextId,
		"normalize(normalize(A) - normalize(float3(0.0f, 0.0f, 1.0f) - normalize(A) * normalize(A).z + float3(0.001f, 0.0f, 0.0f)) * B)",
		EMaterialValueType::Vector3, FVector2(260.0f, 920.0f));
	ConnectGraphPins(*Mat, NormalWsId, PerturbedNormalId, 0);
	ConnectGraphPins(*Mat, MicroFoldSlopeId, PerturbedNormalId, 1);

	const int32 CamVecWsId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::CameraVectorWS, FVector2(0.0f, 1000.0f));
	const int32 ZeroConstId = AddConstantNode(*Mat, NextId, 0.0f, FVector2(0.0f, 1080.0f));
	const int32 FresnelId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Fresnel, FVector2(480.0f, 920.0f));
	ConnectGraphPins(*Mat, PerturbedNormalId, FresnelId, 0);
	ConnectGraphPins(*Mat, FresnelPowerId, FresnelId, 1);
	ConnectGraphPins(*Mat, ZeroConstId, FresnelId, 2);

	const int32 KeyLightDirId = AddConstantVec3Node(*Mat, NextId, FVector(0.45f, -0.65f, 0.61f), FVector2(0.0f, 1160.0f));
	const int32 FillLightDirId = AddConstantVec3Node(*Mat, NextId, FVector(-0.66f, 0.44f, 0.61f), FVector2(0.0f, 1240.0f));

	const int32 KeyWrapDiffId = AddCustomExprNode(*Mat, NextId,
		"saturate(dot(normalize(A), normalize(B)) * 0.58f + 0.42f)",
		EMaterialValueType::Scalar, FVector2(480.0f, 1140.0f));
	ConnectGraphPins(*Mat, PerturbedNormalId, KeyWrapDiffId, 0);
	ConnectGraphPins(*Mat, KeyLightDirId, KeyWrapDiffId, 1);

	const int32 FillWrapDiffId = AddCustomExprNode(*Mat, NextId,
		"saturate(dot(normalize(A), normalize(B)) * 0.5f + 0.5f)",
		EMaterialValueType::Scalar, FVector2(480.0f, 1220.0f));
	ConnectGraphPins(*Mat, PerturbedNormalId, FillWrapDiffId, 0);
	ConnectGraphPins(*Mat, FillLightDirId, FillWrapDiffId, 1);

	const int32 TotalDiffuseId = AddCustomExprNode(*Mat, NextId,
		"0.36f + 0.64f * A + 0.18f * B",
		EMaterialValueType::Scalar, FVector2(720.0f, 1160.0f));
	ConnectGraphPins(*Mat, KeyWrapDiffId, TotalDiffuseId, 0);
	ConnectGraphPins(*Mat, FillWrapDiffId, TotalDiffuseId, 1);

	// 하프 벡터 기반 실크 스페큘러 계산
	const int32 HalfVecId = AddCustomExprNode(*Mat, NextId,
		"normalize(normalize(A) + normalize(B))",
		EMaterialValueType::Vector3, FVector2(480.0f, 1020.0f));
	ConnectGraphPins(*Mat, KeyLightDirId, HalfVecId, 0);
	ConnectGraphPins(*Mat, CamVecWsId, HalfVecId, 1);

	const int32 NdotHId = AddCustomExprNode(*Mat, NextId,
		"saturate(dot(normalize(A), normalize(B)))",
		EMaterialValueType::Scalar, FVector2(720.0f, 1020.0f));
	ConnectGraphPins(*Mat, PerturbedNormalId, NdotHId, 0);
	ConnectGraphPins(*Mat, HalfVecId, NdotHId, 1);

	const int32 SpecBaseId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Power, FVector2(920.0f, 1020.0f));
	ConnectGraphPins(*Mat, NdotHId, SpecBaseId, 0);
	ConnectGraphPins(*Mat, SpecPowerId, SpecBaseId, 1);

	const int32 SpecScaledId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1100.0f, 1020.0f));
	ConnectGraphPins(*Mat, SpecBaseId, SpecScaledId, 0);
	ConnectGraphPins(*Mat, SpecIntensityId, SpecScaledId, 1);

	// 기본 색상과 탄성 커프 색상 혼합 후 라이팅 니트 주름 음영 적용
	const int32 BaseSwatchRgbId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Lerp, FVector2(920.0f, -140.0f));
	ConnectGraphPins(*Mat, FabricRgbId, BaseSwatchRgbId, 0);
	ConnectGraphPins(*Mat, HemRgbId, BaseSwatchRgbId, 1);
	ConnectGraphPins(*Mat, CuffBandId, BaseSwatchRgbId, 2);

	const int32 CreaseShadeId = AddCustomExprNode(*Mat, NextId,
		"clamp(1.0f - saturate(-A) * 0.16f + saturate(A) * 0.06f, 0.82f, 1.08f) * B",
		EMaterialValueType::Scalar, FVector2(1020.0f, 700.0f));
	ConnectGraphPins(*Mat, TotalCreaseId, CreaseShadeId, 0);
	ConnectGraphPins(*Mat, WeaveFactorId, CreaseShadeId, 1);

	const int32 LightAndWeaveId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1160.0f, 760.0f));
	ConnectGraphPins(*Mat, TotalDiffuseId, LightAndWeaveId, 0);
	ConnectGraphPins(*Mat, CreaseShadeId, LightAndWeaveId, 1);

	const int32 LitFabricRgbId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1380.0f, 40.0f));
	ConnectGraphPins(*Mat, BaseSwatchRgbId, LitFabricRgbId, 0);
	ConnectGraphPins(*Mat, LightAndWeaveId, LitFabricRgbId, 1);

	// 알파 계산 나일론 데니어 프레넬 커프 보강 주름 골 보강 신장 투명도 차감
	const int32 FresnelBoostId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(720.0f, 900.0f));
	ConnectGraphPins(*Mat, FresnelId, FresnelBoostId, 0);
	ConnectGraphPins(*Mat, FresnelOpacityId, FresnelBoostId, 1);

	const int32 DenierBaseId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Add, FVector2(920.0f, 880.0f));
	ConnectGraphPins(*Mat, BaseOpacityId, DenierBaseId, 0);
	ConnectGraphPins(*Mat, FresnelBoostId, DenierBaseId, 1);

	const int32 CuffAlphaBoostId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(920.0f, 560.0f));
	ConnectGraphPins(*Mat, CuffBandId, CuffAlphaBoostId, 0);
	ConnectGraphPins(*Mat, HemBandStrId, CuffAlphaBoostId, 1);

	const int32 DenierWithCuffId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Add, FVector2(1120.0f, 600.0f));
	ConnectGraphPins(*Mat, DenierBaseId, DenierWithCuffId, 0);
	ConnectGraphPins(*Mat, CuffAlphaBoostId, DenierWithCuffId, 1);

	const int32 StretchAlphaSubId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(920.0f, 660.0f));
	ConnectGraphPins(*Mat, StretchFactorId, StretchAlphaSubId, 0);
	ConnectGraphPins(*Mat, StretchDropId, StretchAlphaSubId, 1);

	const int32 AlphaAfterStretchId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Subtract, FVector2(1280.0f, 620.0f));
	ConnectGraphPins(*Mat, DenierWithCuffId, AlphaAfterStretchId, 0);
	ConnectGraphPins(*Mat, StretchAlphaSubId, AlphaAfterStretchId, 1);

	// 주름 골과 뭉친 관절 구간은 불투명도 상승 능선은 얇아짐
	const int32 AlphaWithWrinkleId = AddCustomExprNode(*Mat, NextId,
		"A - B * 0.06f",
		EMaterialValueType::Scalar, FVector2(1420.0f, 620.0f));
	ConnectGraphPins(*Mat, AlphaAfterStretchId, AlphaWithWrinkleId, 0);
	ConnectGraphPins(*Mat, TotalCreaseId, AlphaWithWrinkleId, 1);

	const int32 AlphaWithBunchId = AddCustomExprNode(*Mat, NextId,
		"A + B * 0.14f",
		EMaterialValueType::Scalar, FVector2(1480.0f, 660.0f));
	ConnectGraphPins(*Mat, AlphaWithWrinkleId, AlphaWithBunchId, 0);
	ConnectGraphPins(*Mat, JointCompMaskId, AlphaWithBunchId, 1);

	const int32 MaxAlphaConstId = AddConstantNode(*Mat, NextId, 0.96f, FVector2(1380.0f, 700.0f));
	const int32 ClampedAlphaId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Clamp, FVector2(1540.0f, 620.0f));
	ConnectGraphPins(*Mat, AlphaWithBunchId, ClampedAlphaId, 0);
	ConnectGraphPins(*Mat, MinOpacityId, ClampedAlphaId, 1);
	ConnectGraphPins(*Mat, MaxAlphaConstId, ClampedAlphaId, 2);

	const int32 FinalAlphaId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1660.0f, 540.0f));
	ConnectGraphPins(*Mat, ClampedAlphaId, FinalAlphaId, 0);
	ConnectGraphPins(*Mat, CuffCutoffId, FinalAlphaId, 1);

	// 동적 찢김 마스크 샘플링과 구멍 주위로 밀려 뭉치는 단일 흐름 실 다발 셰이딩 합성
	const int32 TearMaskSampleId = AddTextureParamNode(*Mat, NextId, "TearMaskTex", false, FVector2(220.0f, 1340.0f));
	ConnectGraphPins(*Mat, UvId, TearMaskSampleId, 0);

	const int32 TearHoleRawId = AddMaskNode(*Mat, NextId, 0x01, FVector2(440.0f, 1300.0f));
	const int32 TearRunRawId = AddMaskNode(*Mat, NextId, 0x02, FVector2(440.0f, 1340.0f));
	const int32 TearPushRawId = AddMaskNode(*Mat, NextId, 0x04, FVector2(440.0f, 1380.0f));
	const int32 TearInnerRawId = AddMaskNode(*Mat, NextId, 0x08, FVector2(440.0f, 1420.0f));
	ConnectGraphPins(*Mat, TearMaskSampleId, TearHoleRawId, 0);
	ConnectGraphPins(*Mat, TearMaskSampleId, TearRunRawId, 0);
	ConnectGraphPins(*Mat, TearMaskSampleId, TearPushRawId, 0);
	ConnectGraphPins(*Mat, TearMaskSampleId, TearInnerRawId, 0);

	const int32 TearHoleId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(620.0f, 1300.0f));
	ConnectGraphPins(*Mat, TearHoleRawId, TearHoleId, 0);
	ConnectGraphPins(*Mat, TearMaskActiveId, TearHoleId, 1);

	const int32 TearRunId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(620.0f, 1340.0f));
	ConnectGraphPins(*Mat, TearRunRawId, TearRunId, 0);
	ConnectGraphPins(*Mat, TearMaskActiveId, TearRunId, 1);

	const int32 TearPushDispId = AddCustomExprNode(*Mat, NextId,
		"(A - 0.5f) * 0.15f * B",
		EMaterialValueType::Scalar, FVector2(620.0f, 1380.0f));
	ConnectGraphPins(*Mat, TearPushRawId, TearPushDispId, 0);
	ConnectGraphPins(*Mat, TearMaskActiveId, TearPushDispId, 1);

	const int32 TearInnerId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(620.0f, 1420.0f));
	ConnectGraphPins(*Mat, TearInnerRawId, TearInnerId, 0);
	ConnectGraphPins(*Mat, TearMaskActiveId, TearInnerId, 1);

	// 구멍 변위 필드로 중립 가로 실 좌표를 휘어 각 구멍의 당김 방향에 맞춰 뭉치는 실 가닥 생성
	const int32 WarpedThreadCoordId = AddCustomExprNode(*Mat, NextId,
		"A.y - B + 0.0020f * sin(A.x * 68.0f + A.y * 19.0f)",
		EMaterialValueType::Scalar, FVector2(820.0f, 1380.0f));
	ConnectGraphPins(*Mat, UvId, WarpedThreadCoordId, 0);
	ConnectGraphPins(*Mat, TearPushDispId, WarpedThreadCoordId, 1);

	const int32 FlowStrandPatternId = AddCustomExprNode(*Mat, NextId,
		"saturate((pow(saturate(1.0f - abs(sin(A * 290.0f))), 4.5f) + 0.58f * pow(saturate(1.0f - abs(sin(A * 610.0f + 1.1f))), 6.0f)) * B)",
		EMaterialValueType::Scalar, FVector2(1000.0f, 1380.0f));
	ConnectGraphPins(*Mat, WarpedThreadCoordId, FlowStrandPatternId, 0);
	ConnectGraphPins(*Mat, TearRungStrId, FlowStrandPatternId, 1);

	// 구멍 안쪽은 실 없이 깨끗한 맨살만 드러내고 경계는 날카롭게 컷아웃
	const int32 HoleOpenMaskId = AddCustomExprNode(*Mat, NextId,
		"smoothstep(0.495f, 0.516f, A + 0.004f * sin(B.y * 540.0f))",
		EMaterialValueType::Scalar, FVector2(820.0f, 1280.0f));
	ConnectGraphPins(*Mat, TearHoleId, HoleOpenMaskId, 0);
	ConnectGraphPins(*Mat, UvId, HoleOpenMaskId, 1);

	const int32 HoleCutFactorId = AddCustomExprNode(*Mat, NextId,
		"saturate(1.0f - A * saturate(B + 1.0f))",
		EMaterialValueType::Scalar, FVector2(1020.0f, 1300.0f));
	ConnectGraphPins(*Mat, HoleOpenMaskId, HoleCutFactorId, 0);
	ConnectGraphPins(*Mat, TearMaskActiveId, HoleCutFactorId, 1);

	// 구멍 경계에 밀려 뭉친 말린 테두리와 밀착 시 안쪽 피부 접촉 음영 계산
	const int32 TearRimBandId = AddCustomExprNode(*Mat, NextId,
		"saturate(1.0f - abs(A - 0.490f) * 34.0f) * saturate(A * 8.0f) * B",
		EMaterialValueType::Scalar, FVector2(1020.0f, 1220.0f));
	ConnectGraphPins(*Mat, TearHoleId, TearRimBandId, 0);
	ConnectGraphPins(*Mat, TearRimStrId, TearRimBandId, 1);

	const int32 ContactWeightId = AddCustomExprNode(*Mat, NextId,
		"saturate(1.0f - saturate((A - 1.03f) * 3.8f)) * B",
		EMaterialValueType::Scalar, FVector2(820.0f, 1520.0f));
	ConnectGraphPins(*Mat, StretchRatioId, ContactWeightId, 0);
	ConnectGraphPins(*Mat, TearContactStrId, ContactWeightId, 1);

	const int32 InnerSkinShadowId = AddCustomExprNode(*Mat, NextId,
		"saturate(A * B * 0.28f)",
		EMaterialValueType::Scalar, FVector2(1020.0f, 1500.0f));
	ConnectGraphPins(*Mat, TearInnerId, InnerSkinShadowId, 0);
	ConnectGraphPins(*Mat, ContactWeightId, InnerSkinShadowId, 1);

	// 올 풀림 리본 구간은 실 사이는 투명하게 비치고 밀려 모인 실 가닥만 선명하게 남김
	const int32 RunAlphaModId = AddCustomExprNode(*Mat, NextId,
		"lerp(1.0f, clamp(0.10f + B * 1.15f, 0.08f, 1.35f), saturate(A))",
		EMaterialValueType::Scalar, FVector2(1180.0f, 1360.0f));
	ConnectGraphPins(*Mat, TearRunId, RunAlphaModId, 0);
	ConnectGraphPins(*Mat, FlowStrandPatternId, RunAlphaModId, 1);

	const int32 AlphaWithRunId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1320.0f, 1340.0f));
	ConnectGraphPins(*Mat, FinalAlphaId, AlphaWithRunId, 0);
	ConnectGraphPins(*Mat, RunAlphaModId, AlphaWithRunId, 1);

	const int32 AlphaWithRimId = AddCustomExprNode(*Mat, NextId,
		"saturate(A + B * 0.22f)",
		EMaterialValueType::Scalar, FVector2(1420.0f, 1300.0f));
	ConnectGraphPins(*Mat, AlphaWithRunId, AlphaWithRimId, 0);
	ConnectGraphPins(*Mat, TearRimBandId, AlphaWithRimId, 1);

	const int32 FabricAlphaCutId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1520.0f, 1300.0f));
	ConnectGraphPins(*Mat, AlphaWithRimId, FabricAlphaCutId, 0);
	ConnectGraphPins(*Mat, HoleCutFactorId, FabricAlphaCutId, 1);

	const int32 TearFinalAlphaId = AddCustomExprNode(*Mat, NextId,
		"max(A, B)",
		EMaterialValueType::Scalar, FVector2(1640.0f, 1340.0f));
	ConnectGraphPins(*Mat, FabricAlphaCutId, TearFinalAlphaId, 0);
	ConnectGraphPins(*Mat, InnerSkinShadowId, TearFinalAlphaId, 1);

	const int32 RunStrandShadeId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1140.0f, 1240.0f));
	ConnectGraphPins(*Mat, FlowStrandPatternId, RunStrandShadeId, 0);
	ConnectGraphPins(*Mat, TearRunId, RunStrandShadeId, 1);

	const int32 RimShadeMulId = AddCustomExprNode(*Mat, NextId,
		"clamp(1.0f - A * 0.42f - B * 0.14f, 0.48f, 1.0f)",
		EMaterialValueType::Scalar, FVector2(1240.0f, 1200.0f));
	ConnectGraphPins(*Mat, TearRimBandId, RimShadeMulId, 0);
	ConnectGraphPins(*Mat, RunStrandShadeId, RimShadeMulId, 1);

	const int32 RimShadedRgbId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Multiply, FVector2(1420.0f, 1180.0f));
	ConnectGraphPins(*Mat, LitFabricRgbId, RimShadedRgbId, 0);
	ConnectGraphPins(*Mat, RimShadeMulId, RimShadedRgbId, 1);

	const int32 SkinContactRgbId = AddCustomExprNode(*Mat, NextId,
		"A * 0.30f + float3(0.06f, 0.035f, 0.025f) * saturate(B + 1.0f)",
		EMaterialValueType::Vector3, FVector2(1420.0f, 1460.0f));
	ConnectGraphPins(*Mat, FabricRgbId, SkinContactRgbId, 0);
	ConnectGraphPins(*Mat, InnerSkinShadowId, SkinContactRgbId, 1);

	const int32 ShadowBlendMaskId = AddCustomExprNode(*Mat, NextId,
		"saturate(A * (1.0f - B))",
		EMaterialValueType::Scalar, FVector2(1420.0f, 1540.0f));
	ConnectGraphPins(*Mat, HoleOpenMaskId, ShadowBlendMaskId, 0);
	ConnectGraphPins(*Mat, HoleCutFactorId, ShadowBlendMaskId, 1);

	const int32 TearFinalRgbId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::Lerp, FVector2(1580.0f, 1200.0f));
	ConnectGraphPins(*Mat, RimShadedRgbId, TearFinalRgbId, 0);
	ConnectGraphPins(*Mat, SkinContactRgbId, TearFinalRgbId, 1);
	ConnectGraphPins(*Mat, ShadowBlendMaskId, TearFinalRgbId, 2);

	// 최종 베이스 컬러와 알파 결합
	const int32 FinalBaseColorId = AddGraphNode(*Mat, NextId, EMaterialExpressionType::AppendVector, FVector2(1660.0f, 160.0f));
	ConnectGraphPins(*Mat, TearFinalRgbId, FinalBaseColorId, 0);
	ConnectGraphPins(*Mat, TearFinalAlphaId, FinalBaseColorId, 1);
	ConnectGraphPins(*Mat, FinalBaseColorId, OutputId, 0);

	// 실크 스페큘러와 림 글리터 광택을 이미시브에 반영해 주름 능선 입체감 강조
	const int32 GlitterBoostId = AddCustomExprNode(*Mat, NextId,
		"A * B * 2.2f",
		EMaterialValueType::Scalar, FVector2(920.0f, 860.0f));
	ConnectGraphPins(*Mat, SilkGlitterId, GlitterBoostId, 0);
	ConnectGraphPins(*Mat, WeaveStrengthId, GlitterBoostId, 1);

	const int32 RimSheenId = AddCustomExprNode(*Mat, NextId,
		"A * (B + 0.08f)",
		EMaterialValueType::Scalar, FVector2(1100.0f, 920.0f));
	ConnectGraphPins(*Mat, FresnelId, RimSheenId, 0);
	ConnectGraphPins(*Mat, SheenStrId, RimSheenId, 1);

	const int32 RimWithGlitterId = AddCustomExprNode(*Mat, NextId,
		"A * (1.0f + B * 3.5f)",
		EMaterialValueType::Scalar, FVector2(1200.0f, 900.0f));
	ConnectGraphPins(*Mat, RimSheenId, RimWithGlitterId, 0);
	ConnectGraphPins(*Mat, GlitterBoostId, RimWithGlitterId, 1);

	const int32 HighlightTotalId = AddCustomExprNode(*Mat, NextId,
		"(A + B * 0.55f)",
		EMaterialValueType::Scalar, FVector2(1320.0f, 960.0f));
	ConnectGraphPins(*Mat, SpecScaledId, HighlightTotalId, 0);
	ConnectGraphPins(*Mat, RimWithGlitterId, HighlightTotalId, 1);

	const int32 HighlightMaskId = AddCustomExprNode(*Mat, NextId,
		"A * clamp(0.85f + B * 0.35f, 0.55f, 1.35f)",
		EMaterialValueType::Scalar, FVector2(1460.0f, 960.0f));
	ConnectGraphPins(*Mat, HighlightTotalId, HighlightMaskId, 0);
	ConnectGraphPins(*Mat, TotalCreaseId, HighlightMaskId, 1);

	const int32 HighlightTearMaskId = AddCustomExprNode(*Mat, NextId,
		"A * B",
		EMaterialValueType::Scalar, FVector2(1560.0f, 960.0f));
	ConnectGraphPins(*Mat, HighlightMaskId, HighlightTearMaskId, 0);
	ConnectGraphPins(*Mat, HoleCutFactorId, HighlightTearMaskId, 1);

	const int32 HighlightTintId = AddCustomExprNode(*Mat, NextId,
		"(A * 0.45f + float3(0.50f, 0.48f, 0.52f)) * B",
		EMaterialValueType::Vector3, FVector2(1640.0f, 920.0f));
	ConnectGraphPins(*Mat, FabricRgbId, HighlightTintId, 0);
	ConnectGraphPins(*Mat, HighlightTearMaskId, HighlightTintId, 1);
	ConnectGraphPins(*Mat, HighlightTintId, OutputId, 3);

	CachedMaster = Mat;
	return CachedMaster;
}

UMaterialInstanceDynamic* FStockingMaterialLibrary::CreateStockingMaterialInstance(const FStockingMaterialParams& Params)
{
	UMaterial* Master = GetOrCreateStockingMasterMaterial();
	if (!Master)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* Instance = UMaterial::CreateDynamicInstance(Master);
	if (Instance)
	{
		ApplyParameters(Instance, Params);
	}
	return Instance;
}

void FStockingMaterialLibrary::ApplyParameters(UMaterialInstanceDynamic* MaterialInstance, const FStockingMaterialParams& Params)
{
	if (!MaterialInstance)
	{
		return;
	}

	MaterialInstance->SetVectorParameterValue(FName("FabricColor"), Params.FabricColor);
	MaterialInstance->SetVectorParameterValue(FName("HemColor"), Params.HemColor);
	MaterialInstance->SetVectorParameterValue(FName("OpacityParams"),
		FVector4(Params.BaseOpacity, Params.FresnelOpacity, Params.FresnelPower, Params.MinOpacity));
	MaterialInstance->SetVectorParameterValue(FName("StretchParams"),
		FVector4(Params.StretchSensitivity, Params.StretchAlphaDrop, Params.WeaveScale, Params.WeaveStrength));
	MaterialInstance->SetVectorParameterValue(FName("DetailParams"),
		FVector4(Params.HemBandStrength, Params.SpecularIntensity, Params.SpecularPower, Params.WrinkleShadingStrength));
	MaterialInstance->SetVectorParameterValue(FName("SheenParams"),
		FVector4(Params.SheenStrength, Params.HemFeatherPower, 0.0f, 0.0f));
	MaterialInstance->SetVectorParameterValue(FName("TearStyleParams"),
		FVector4(Params.TearRimStrength, Params.TearSkinContactStrength, Params.TearRungStrength, 0.0f));
}
