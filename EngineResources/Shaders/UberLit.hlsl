#pragma pack_matrix(row_major)

#include "LightData.hlsli"
#include "LightEvaluation.hlsli"
#include "LightGridCommon.hlsli"
#include "VertexFactory.hlsli"
#include "GPUSceneCommon.hlsli"
#include "MaterialUV.hlsli"

// 셰이더 스캐너는 매크로 정의 없이 이 파일을 컴파일하므로 기본 변형에는 Lambert를 쓴다.
// 명시적인 변형은 조명 모델 또는 뷰 모드 하나만 1로 설정한다.
#if !defined(LIGHTING_MODEL_GOURAUD) && !defined(LIGHTING_MODEL_LAMBERT) && !defined(LIGHTING_MODEL_PHONG) && !defined(LIGHTING_MODEL_UNLIT) && !defined(VIEW_MODE_WORLD_NORMAL)
    #define LIGHTING_MODEL_LAMBERT 1
#endif

#if (LIGHTING_MODEL_GOURAUD + LIGHTING_MODEL_LAMBERT + LIGHTING_MODEL_PHONG + LIGHTING_MODEL_UNLIT + VIEW_MODE_WORLD_NORMAL) != 1
    #error Exactly one lighting model or view mode must be enabled.
#endif

struct FAmbientLightInfo
{
    float4 ColorIntensity; // rgb: 선형 색상, a: 광도
};

cbuffer ViewConstants : register(b0)
{
    matrix VP;
    matrix View;
    matrix Projection;
    float4 ViewTimeParams;
};

cbuffer Lighting : register(b1)
{
    FAmbientLightInfo Ambient;
    float4 CameraPosition;
    uint4 LightCounts; // x: 포인트 개수, y: 스포트 개수, z: 방향광 개수
    uint4 LightOffsets; // SceneLights에서 각 종류가 시작되는 위치, 순서는 위와 같음
    // 그림자는 광원마다 Light.ShadowMapChannel로 ScreenShadowMask(t13)에서 읽는다.
};

StructuredBuffer<FLightRenderParameters> SceneLights : register(t8);
StructuredBuffer<uint4> TileLightRanges : register(t9);
StructuredBuffer<uint> TilePointIndices : register(t10);
StructuredBuffer<uint> TileSpotIndices : register(t12);
// 그림자 광원마다 배정된 채널(R~A)에 Projection 결과가 들어 있다. 1 = 빛 받음.
Texture2D<float4> ScreenShadowMask : register(t13);
// 투영 데칼 버퍼
Texture2D<float4> DBufferLit : register(t14);
Texture2D<float4> DBufferUnlit : register(t15);
Texture2D<float4> DBufferNormal : register(t16);

cbuffer MaterialParams : register(b2)
{
    float4 BaseColor;
    float4 Emissive;
    float4 SurfaceParams; // x: 노멀맵 Y 반전, y: 알파 마스크(clip), z: 컬러 텍스처 사용, w: 노멀맵 인코딩
    float4 UVTransform; // xy: 이동량, zw: 배율
    float4 SpecularColorShininess; // rgb: 정반사 색상, w: 광택 지수
    float4 WorldUVParams;
    float4 UVScrollAndFlags;
};

Texture2D ColorTexture : register(t0);
Texture2D NormalTexture : register(t1);

SamplerState ColorSampler : register(s0);

#ifdef MATERIAL_GRAPH
#include MATERIAL_GRAPH_INCLUDE
#endif

struct VS_OUT
{
    float4 Position : SV_POSITION;
    float4 Color : COLOR;
    float2 UV : TEXCOORD0;
    float3 WorldNormal : TEXCOORD4;
    float4 WorldTangent : TEXCOORD5;
#if LIGHTING_MODEL_GOURAUD
    float3 VertexDiffuse : TEXCOORD1;
    float3 VertexSpecular : TEXCOORD2;
    float3 WorldPosition : TEXCOORD3;
#elif !VIEW_MODE_WORLD_NORMAL
    float3 WorldPosition : TEXCOORD1;
    float ViewDepth : TEXCOORD2;
#endif
    float BoneWeight : TEXCOORD6;
    nointerpolation uint2 ObjectIds : TEXCOORD7;
    nointerpolation float2 ObjectFlags : TEXCOORD8;
    float3 MaterialWorldPos : TEXCOORD9;
    float3 MaterialLocalPos : TEXCOORD10;
    float3 MaterialLocalNormal : TEXCOORD11;
};

struct FUberOutput
{
	float4 LitColor : SV_Target0;
};

// 광원의 그림자 (1 = 빛 받음). 채널이 없거나 이 물체의 bReceiveShadow가 꺼져 있으면 1.
// Gouraud는 채널을 받지 않으므로 정점 셰이더에서 불려도 마스크를 읽지 않는다.
float GetLightShadow(FLightRenderParameters Light, float3 PixelPosition, bool bReceiveShadow)
{
    if (Light.ShadowMapChannel == INVALID_SHADOW_CHANNEL || !bReceiveShadow) return 1.0f;
    return ScreenShadowMask.Load(int3(int2(PixelPosition.xy), 0))[Light.ShadowMapChannel];
}


float3 DecodeMapNormal(float3 Sample, float Encoding)
{
    if (Encoding > 1.5f)
    {
        float2 XY = Encoding > 2.5f ? Sample.xy : Sample.xy * 2.0f - 1.0f;
        return float3(XY, sqrt(saturate(1.0f - dot(XY, XY))));
    }
    return Sample * 2.0f - 1.0f;
}

// 조명과 World Normal 보기가 같은 노멀맵 해석 및 TBN 변환을 사용한다.
float3 GetShadingNormal(VS_OUT Input, float3 ProjectedT, float3 ProjectedB)
{
    float LengthSquared = dot(Input.WorldNormal, Input.WorldNormal);
    float3 N = LengthSquared > 1.0e-8f
        ? Input.WorldNormal * rsqrt(LengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
    if (SurfaceParams.w > 0.5f)
    {
        float3 MapNormal = DecodeMapNormal(NormalTexture.Sample(ColorSampler, Input.UV).xyz, SurfaceParams.w);
        if (SurfaceParams.x > 0.5f) MapNormal.y = -MapNormal.y;
        float3 T = Input.WorldTangent.xyz - N * dot(N, Input.WorldTangent.xyz);
        if (WorldUVParams.x > 0.0f) T = ProjectedT - N * dot(N, ProjectedT);
        float TangentLengthSquared = dot(T, T);
        if (TangentLengthSquared > 1.0e-8f)
        {
            T *= rsqrt(TangentLengthSquared);
            float3 B = cross(N, T) * Input.WorldTangent.w;
            if (WorldUVParams.x > 0.0f) B = normalize(ProjectedB);
            float3 MappedNormal = T * MapNormal.x + B * MapNormal.y + N * MapNormal.z;
            float MappedLengthSquared = dot(MappedNormal, MappedNormal);
            if (MappedLengthSquared > 1.0e-8f)
                N = MappedNormal * rsqrt(MappedLengthSquared);
        }
    }
    return N;
}

#if !VIEW_MODE_WORLD_NORMAL
float3 CalculateAmbientLight(FAmbientLightInfo Info)
{
    return Info.ColorIntensity.rgb * max(Info.ColorIntensity.a, 0.0f);
}

void AddLight(inout FLightResult Result, float3 N, float3 V, float3 L,
    float3 Radiance, bool bIncludeSpecular, float4 ActiveSpecular)
{
    AccumulateLight(Result, N, V, L, Radiance, ActiveSpecular.rgb,
        max(abs(ActiveSpecular.w), 1.0f), bIncludeSpecular);
}

void CalculateDirectionalLight(inout FLightResult Result, FLightRenderParameters Info, float3 N, float3 V, bool bIncludeSpecular, float4 ActiveSpecular)
{
    if (Info.Intensity <= 0.0f)
    {
        return;
    }
    
    float3 L = normalize(-Info.Direction);
    float3 Radiance = Info.Color * Info.Intensity;
    
    AddLight(Result, N, V, L, Radiance, bIncludeSpecular, ActiveSpecular);
}

void CalculatePointLight(inout FLightResult Result, FLightRenderParameters Info,
    float3 P, float3 N, float3 V, bool bIncludeSpecular, float4 ActiveSpecular)
{
    float3 ToLight = Info.Position - P;
    float DistanceSquared = dot(ToLight, ToLight);
    float RadiusSquared = Info.Radius * Info.Radius;
    if (RadiusSquared <= 0.0f || DistanceSquared >= RadiusSquared || DistanceSquared < 1.0e-8f)
        return;
    float Distance = sqrt(max(DistanceSquared, 1.0e-6f));
    // Point Light의 RadiusFallOff를 반경 감쇠 지수로 사용한다.
    float Fade = pow(saturate(1.0f - Distance / Info.Radius), max(Info.FalloffExponent, 0.0f));
    if (Fade <= 0.0f) return;
    float3 L = ToLight / Distance;
    float3 Radiance = Info.Color * max(Info.Intensity, 0.0f) * Fade;
    AddLight(Result, N, V, L, Radiance, bIncludeSpecular, ActiveSpecular);
}

void CalculateSpotLight(inout FLightResult Result, FLightRenderParameters Info, float3 P, float3 N, float3 V, bool bIncludeSpecular, float4 ActiveSpecular)
{
    float3 ToLight = Info.Position - P;
    float DistanceSquared = dot(ToLight, ToLight);
    float RadiusFade = CalculateDistanceAttenuation(DistanceSquared, Info.Radius * Info.Radius);
    if (RadiusFade <= 0.0f) return;
    float3 L = ToLight / sqrt(max(DistanceSquared, 1.0e-6f));
	float ConeCos = dot(-L, normalize(Info.Direction));
	float ConeFade = CalculateConeAttenuation(ConeCos, Info.InnerConeCos, Info.OuterConeCos);
	ConeFade = pow(ConeFade, max(Info.FalloffExponent, 0.0001f));

	float3 Radiance = Info.Color * max(Info.Intensity, 0.0f) * ConeFade * RadiusFade;

    AddLight(Result, N, V, L, Radiance, bIncludeSpecular, ActiveSpecular);
}

FLightResult CalculateBaseLighting(float3 PixelPosition, float3 P, float ViewDepth, float3 N, float3 V, bool bIncludeSpecular, bool bReceiveShadow, float AmbientOcclusion, float4 ActiveSpecular)
{
    FLightResult Result;
    Result.Diffuse = CalculateAmbientLight(Ambient) * AmbientOcclusion;
    Result.Specular = 0.0f;

    [loop]
    for (uint DirectionalIndex = 0; DirectionalIndex < LightCounts.z; ++DirectionalIndex)
    {
        FLightRenderParameters Light = SceneLights[LightOffsets.z + DirectionalIndex];
        
        float Shadow = GetLightShadow(Light, PixelPosition, bReceiveShadow);
            
        FLightResult DirectionalResult = (FLightResult) 0;
        CalculateDirectionalLight(DirectionalResult, Light, N, V, bIncludeSpecular, ActiveSpecular);
        DirectionalResult.Diffuse *= Shadow;
        DirectionalResult.Specular *= Shadow;
         
        Result.Diffuse += DirectionalResult.Diffuse;
        Result.Specular += DirectionalResult.Specular;
    }
    
    return Result;
}

void AddAllLocalLights(inout FLightResult Result, float3 PixelPosition, float3 P, float3 N,
    float3 V, bool bIncludeSpecular, bool bReceiveShadow, float4 ActiveSpecular)
{
    [loop]
    for (uint PointIndex = 0; PointIndex < LightCounts.x; ++PointIndex)
    {
        FLightRenderParameters Light = SceneLights[LightOffsets.x + PointIndex];

        float Shadow = GetLightShadow(Light, PixelPosition, bReceiveShadow);

        FLightResult PointResult = (FLightResult) 0;
        CalculatePointLight(PointResult, SceneLights[LightOffsets.x + PointIndex], P, N, V, bIncludeSpecular, ActiveSpecular);
         
        Result.Diffuse += PointResult.Diffuse * Shadow;
        Result.Specular += PointResult.Specular * Shadow;
    }
    [loop]
    for (uint SpotIndex = 0; SpotIndex < LightCounts.y; ++SpotIndex)
    {
        FLightRenderParameters Light = SceneLights[LightOffsets.y + SpotIndex];
        
        float Shadow = GetLightShadow(Light, PixelPosition, bReceiveShadow);
            
        FLightResult SpotResult = (FLightResult) 0;
        CalculateSpotLight(SpotResult, Light, P, N, V, bIncludeSpecular, ActiveSpecular);

        Result.Diffuse += SpotResult.Diffuse * Shadow;
        Result.Specular += SpotResult.Specular * Shadow;
    }
}

FLightResult CalculateLighting(float3 PixelPosition, float3 P, float ViewDepth, float3 Normal, bool bIncludeSpecular, bool bReceiveShadow, float AmbientOcclusion, float4 ActiveSpecular)
{
    float3 N = normalize(Normal);
    float3 ToCamera = CameraPosition.xyz - P;
    float3 V = ToCamera * rsqrt(max(dot(ToCamera, ToCamera), 1.0e-8f));
    FLightResult Result = CalculateBaseLighting(PixelPosition, P, ViewDepth, N, V, bIncludeSpecular, bReceiveShadow, AmbientOcclusion, ActiveSpecular);
    AddAllLocalLights(Result, PixelPosition, P, N, V, bIncludeSpecular, bReceiveShadow, ActiveSpecular);
    return Result;
}

// 타일 목록의 인덱스는 kim의 공유 SceneLights 안에서 종류별 시작 위치에 더한다.
FLightResult CalculateTiledLighting(float3 PixelPosition, float3 P, float ViewDepth, float3 Normal, bool bIncludeSpecular, bool bReceiveShadow, float AmbientOcclusion, float4 ActiveSpecular)
{
    float3 N = normalize(Normal);
    float3 ToCamera = CameraPosition.xyz - P;
    float3 V = ToCamera * rsqrt(max(dot(ToCamera, ToCamera), 1.0e-8f));
    FLightResult Result = CalculateBaseLighting(PixelPosition, P, ViewDepth, N, V, bIncludeSpecular, bReceiveShadow, AmbientOcclusion, ActiveSpecular);

    if (GridMode == LIGHT_GRID_MODE_OFF)
    {
        // Off는 업로드된 전체 로컬 광원을 평가한다.
        AddAllLocalLights(Result, PixelPosition, P, N, V, bIncludeSpecular, bReceiveShadow, ActiveSpecular);
    }
    else
    {
        // SV_POSITION.xy는 픽셀 위치, z는 현재 표면의 DeviceZ다.
        uint CellIndex = GetLightGridCellIndex(uint2(PixelPosition.xy), PixelPosition.z);
        uint4 Range = TileLightRanges[CellIndex];
        bool UsePointList = Range.x != 0xffffffffu;
        bool UseSpotList = Range.z != 0xffffffffu;
        uint PointCount = UsePointList ? Range.y : LightCounts.x;
        uint SpotCount = UseSpotList ? Range.w : LightCounts.y;

        // 예약 실패한 종류만 전체 배열을 평가하는 기존 fallback을 유지한다.
        [loop]
        for (uint PointListIndex = 0; PointListIndex < PointCount; ++PointListIndex)
        {
            uint LightIndex = UsePointList ? TilePointIndices[Range.x + PointListIndex] : PointListIndex;
            
            FLightRenderParameters Light = SceneLights[LightOffsets.x + LightIndex];

            float Shadow = GetLightShadow(Light, PixelPosition, bReceiveShadow);
            
            FLightResult PointResult = (FLightResult) 0;
            CalculatePointLight(PointResult, Light, P, N, V, bIncludeSpecular, ActiveSpecular);
         
            Result.Diffuse += PointResult.Diffuse * Shadow;
            Result.Specular += PointResult.Specular * Shadow;
        }

        [loop]
        for (uint SpotListIndex = 0; SpotListIndex < SpotCount; ++SpotListIndex)
        {
            uint LightIndex = UseSpotList ? TileSpotIndices[Range.z + SpotListIndex] : SpotListIndex;
            
            FLightRenderParameters Light = SceneLights[LightOffsets.y + LightIndex];
            
            float Shadow = GetLightShadow(Light, PixelPosition, bReceiveShadow);
            
            FLightResult SpotResult = (FLightResult) 0;
            CalculateSpotLight(SpotResult, Light, P, N, V, bIncludeSpecular, ActiveSpecular);
            
            Result.Diffuse += SpotResult.Diffuse * Shadow;
            Result.Specular += SpotResult.Specular * Shadow;
        }
    }
    return Result;
}

#endif


// Week11 Unreal 스타일 본 가중치 히트맵 그라디언트 (0: 마젠타 -> 파랑 -> 하늘 -> 초록 -> 노랑 -> 빨강)
float3 CalculateBoneWeightHeatmap(float Weight)
{
    const float t = saturate(Weight);
    const float3 MagentaColor = float3(160.0f / 255.0f, 32.0f / 255.0f, 240.0f / 255.0f);
    const float3 BlueColor    = float3(0.0f, 0.05f, 0.95f);
    const float3 CyanColor    = float3(0.0f, 0.95f, 0.95f);
    const float3 GreenColor   = float3(0.1f, 0.95f, 0.1f);
    const float3 YellowColor  = float3(1.0f, 0.90f, 0.0f);
    const float3 RedColor     = float3(1.0f, 0.05f, 0.05f);

    if (t < 0.08f)
    {
        float u = t / 0.08f;
        u = u * u * (3.0f - 2.0f * u);
        return lerp(MagentaColor, BlueColor, u);
    }
    else if (t < 0.22f)
    {
        return lerp(BlueColor, CyanColor, (t - 0.08f) / (0.22f - 0.08f));
    }
    else if (t < 0.40f)
    {
        return lerp(CyanColor, GreenColor, (t - 0.22f) / (0.40f - 0.22f));
    }
    else if (t < 0.55f)
    {
        return lerp(GreenColor, YellowColor, (t - 0.40f) / (0.55f - 0.40f));
    }
    else if (t < 0.72f)
    {
        return lerp(YellowColor, RedColor, (t - 0.55f) / (0.72f - 0.55f));
    }
    return RedColor;
}

VS_OUT Uber_VS(FVertexFactoryInput Input, uint InstanceId)
{
	VS_OUT Output;
	const uint PrimId = LoadScenePrimitiveId(InstanceId);
	const FPrimitiveSceneData Prim = LoadScenePrimitive(InstanceId);
	const FVFIntermediates VF = VF_GetIntermediates(Input, Prim.LocalToWorld, Prim.NormalMatrix);
    float4 WorldPosition = float4(VF.WorldPosition, 1.0f);
    float4 ViewPosition = mul(WorldPosition, View);
    Output.Position = mul(ViewPosition, Projection);
    Output.Color = VF.Color;
    Output.ObjectIds = uint2(PrimId, 0);
    Output.ObjectFlags = float2(Prim.Flags.x, Prim.Flags.z);
    Output.BoneWeight = 0.0f;
    if (Prim.Flags.z > 0.5f)
    {
#if VF_GPU_SKIN
        const uint Bone = (uint)(Prim.Flags.z - 0.5f);   // 본 번호 + 1로 실은 값을 되돌린다
        Output.BoneWeight = GpuSkinWeightForBone(Input.BlendIndices, Input.BlendWeights, Bone);
#else
        Output.BoneWeight = Input.Color.r;
#endif
    }
    Output.UV = VF.UV;
    Output.MaterialWorldPos = VF.WorldPosition;
    Output.MaterialLocalPos = VF.LocalPosition;
    Output.MaterialLocalNormal = VF.LocalNormal;

    float3 N = VF.WorldNormal;
    Output.WorldNormal = N;
    Output.WorldTangent = VF.WorldTangent;
#if LIGHTING_MODEL_GOURAUD
    FLightResult Lit = CalculateLighting(float3(0.0f, 0.0f, 0.0f), WorldPosition.xyz, ViewPosition.x, N, true, false, 1.0f, SpecularColorShininess);
    Output.VertexDiffuse = Lit.Diffuse;
    Output.VertexSpecular = Lit.Specular;
    Output.WorldPosition = WorldPosition.xyz;
#elif !VIEW_MODE_WORLD_NORMAL
    Output.WorldPosition = WorldPosition.xyz;
    Output.ViewDepth = ViewPosition.x;
#endif
    return Output;
}

FUberOutput Uber_PS(VS_OUT Input)
{
#if VF_MESH_PARTICLE
	const FPrimitiveSceneData Prim = LoadScenePrimitive(0u);
#else
	const FPrimitiveSceneData Prim = PrimitiveSceneData[Input.ObjectIds.x];
#endif
	FUberOutput Output;
    float3 ProjectedT, ProjectedB;
    float2 DynamicUVOffset = UVTransform.xy + UVScrollAndFlags.xy * ViewTimeParams.x;
    Input.UV = ResolveMaterialUV(Input.UV, Input.MaterialWorldPos, Input.WorldNormal,
        Input.MaterialLocalPos, Input.MaterialLocalNormal, Prim.LocalToWorld,
        UVTransform.zw, DynamicUVOffset, WorldUVParams.xy, WorldUVParams.z > 0.5f, ProjectedT, ProjectedB);
    float3 ShadingNormal = GetShadingNormal(Input, ProjectedT, ProjectedB);
#if VIEW_MODE_WORLD_NORMAL
    Output.LitColor = float4(ShadingNormal * 0.5f + 0.5f, 1.0f);
#else
    if (Input.ObjectFlags.y > 0.5f)
    {
        const float3 HeatColor = CalculateBoneWeightHeatmap(Input.BoneWeight);
        if (HeatmapWireMode > 0u)
        {
            const float3 WireColor = (HeatmapWireMode > 1u)
                ? float3(1.0f, 1.0f, 1.0f)
                : saturate(HeatColor * 1.35f + float3(0.08f, 0.08f, 0.08f));
            Output.LitColor = float4(WireColor, 1.0f);
            return Output;
        }

        const float3 HeatmapL = normalize(CameraPosition.xyz - Input.WorldPosition);
        const float HalfLambert = dot(ShadingNormal, HeatmapL) * 0.5f + 0.5f;
        const float LightingFactor = HalfLambert * 0.7f + 0.3f;
        Output.LitColor = float4(HeatColor * LightingFactor, 1.0f);
        return Output;
    }

    if (SurfaceParams.y > 0.5f)
    {
        // 양면 알파 마스크(식생 카드·철망)의 뒷면 법선을 카메라 쪽으로 뒤집고 상향 보정해 교차면 명암 꺾임을 완화한다.
        const float3 ToCam = CameraPosition.xyz - Input.WorldPosition;
        if (dot(ShadingNormal, ToCam) < 0.0f)
        {
            ShadingNormal = -ShadingNormal;
        }
        ShadingNormal = normalize(lerp(ShadingNormal, float3(0.0f, 0.0f, 1.0f), 0.35f));
    }

    float4 ActiveSpecular = SpecularColorShininess;
    float3 ActiveEmissive = Emissive.rgb;
    float AmbientOcclusion = 1.0f;

#ifdef MATERIAL_GRAPH
    FMaterialGraphOutput GraphOutput = EvaluateMaterialGraph(Input.UV, Input.MaterialWorldPos,
        normalize(CameraPosition.xyz - Input.MaterialWorldPos), normalize(Input.WorldNormal), Input.Color, ViewTimeParams.x);

#ifdef MATERIAL_GRAPH_HAS_AMBIENT_OCCLUSION
    AmbientOcclusion = saturate(GraphOutput.AmbientOcclusion);
#endif

#ifdef MATERIAL_GRAPH_HAS_OPACITY_MASK
    clip(GraphOutput.OpacityMask - 0.001f);
#endif

#ifdef MATERIAL_GRAPH_HAS_SPECULAR_COLOR
    ActiveSpecular.rgb = GraphOutput.SpecularColor;
#endif

#ifdef MATERIAL_GRAPH_HAS_NORMAL
    {
        float3 N = normalize(Input.WorldNormal);
        float3 T = Input.WorldTangent.xyz - N * dot(N, Input.WorldTangent.xyz);
        float TangentLengthSquared = dot(T, T);
        if (TangentLengthSquared > 1.0e-8f)
        {
            T *= rsqrt(TangentLengthSquared);
            float3 B = cross(N, T) * Input.WorldTangent.w;
            ShadingNormal = normalize(T * GraphOutput.Normal.x + B * GraphOutput.Normal.y + N * GraphOutput.Normal.z);
        }
    }
#endif

#ifdef MATERIAL_GRAPH_HAS_SHININESS
    ActiveSpecular.a = GraphOutput.Shininess;
#endif

#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_COLOR
    ActiveEmissive = GraphOutput.EmissiveColor;
#endif

#ifdef MATERIAL_GRAPH_HAS_EMISSIVE_FACTOR
    ActiveEmissive *= GraphOutput.EmissiveFactor;
#endif
#endif

    float4 SurfaceColor;
#ifdef MATERIAL_GRAPH_HAS_BASE_COLOR
    SurfaceColor = GraphOutput.BaseColor;
    float3 Albedo = SurfaceColor.rgb;
#else
    if (SurfaceParams.z > 0.5f)
    {
        float4 TextureColor = ColorTexture.Sample(ColorSampler, Input.UV);
        if (SurfaceParams.y > 0.5f)
        {
            clip(TextureColor.a - 0.5f);
        }
        SurfaceColor = float4(pow(max(TextureColor.rgb, 0.0f), 2.2f), TextureColor.a);
#if VF_MESH_PARTICLE
        // 텍스처가 있으면 정점 색을 쓰지 않으므로 파티클 색(Color/ColorOverLife 모듈)을 여기서 곱한다
        SurfaceColor *= Input.Color;
#endif
    }
    else
    {
        SurfaceColor = Input.Color;
    }
    float3 Albedo = SurfaceColor.rgb * BaseColor.rgb;
#endif

#if defined(MATERIAL_GRAPH_HAS_METALLIC) || defined(MATERIAL_GRAPH_HAS_ROUGHNESS)
    {
        const float PI = 3.14159265359f;
        float Metallic = saturate(GraphOutput.Metallic);
        float Roughness = clamp(GraphOutput.Roughness, 0.045f, 1.0f);
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), Albedo, Metallic);
        float3 ToCamera = normalize(CameraPosition.xyz - Input.MaterialWorldPos);
        float NoV = saturate(dot(ShadingNormal, ToCamera));
        float3 Fview = F0 + (1.0f - F0) * pow(1.0f - NoV, 5.0f);
        Albedo *= (1.0f - Metallic) * (1.0f - Fview) / PI;
        ActiveSpecular = float4(F0, -Roughness);
    }
#endif

    const int3 DecalPixelCoord = int3(int2(Input.Position.xy), 0);
    const bool bReceiveDecal = Prim.Flags.y > 0.5f;
    if (bReceiveDecal)
    {
        float4 LitDecal = DBufferLit.Load(DecalPixelCoord);
        Albedo = Albedo * (1.0f - LitDecal.a) + LitDecal.rgb;
    }

#if LIGHTING_MODEL_LAMBERT || LIGHTING_MODEL_PHONG
    if (bReceiveDecal)
    {
        float4 DecalNormal = DBufferNormal.Load(DecalPixelCoord);
        float3 CombinedNormal = ShadingNormal * (1.0f - DecalNormal.a) + DecalNormal.rgb;
        float NormalLenSq = dot(CombinedNormal, CombinedNormal);
        if (NormalLenSq > 1.0e-8f)
        {
            ShadingNormal = CombinedNormal * rsqrt(NormalLenSq);
        }
    }
#endif

    float3 Diffuse;
    float3 Specular = 0.0f;
    const bool bReceiveShadow = Input.ObjectFlags.x >= 0.5f;

#if LIGHTING_MODEL_GOURAUD
    Diffuse = Input.VertexDiffuse;
    Specular = Input.VertexSpecular;
#elif LIGHTING_MODEL_LAMBERT
    // 픽셀 위치와 깊이를 함께 전달한다.
    Diffuse = CalculateTiledLighting(Input.Position.xyz, Input.WorldPosition, Input.ViewDepth, ShadingNormal, false, bReceiveShadow, AmbientOcclusion, ActiveSpecular).Diffuse;
#elif LIGHTING_MODEL_PHONG
    // 같은 목록 선택 경로에서 기존 Blinn-Phong 조명을 계산한다.
    FLightResult Lit = CalculateTiledLighting(Input.Position.xyz, Input.WorldPosition, Input.ViewDepth, ShadingNormal, true, bReceiveShadow, AmbientOcclusion, ActiveSpecular);
    Diffuse = Lit.Diffuse;
    Specular = Lit.Specular;
#elif LIGHTING_MODEL_UNLIT
    // 조명·그림자를 계산하지 않고 표면 색을 그대로 낸다.
    Diffuse = 1.0f;
#endif

    float3 FinalColor = Albedo * Diffuse + Specular + ActiveEmissive;
    if (bReceiveDecal)
    {
        float4 UnlitDecal = DBufferUnlit.Load(DecalPixelCoord);
        FinalColor = FinalColor * (1.0f - UnlitDecal.a) + UnlitDecal.rgb;
    }

	Output.LitColor = float4(FinalColor,
		WorldUVParams.w > 0.5f ? 1.0f : SurfaceColor.a * BaseColor.a);
#endif
    return Output;
}

// 현재 리소스 매니저는 .hlsl 파일을 스캔할 때 mainVS/mainPS 진입점을 찾는다.
VS_OUT mainVS(FVertexFactoryInput Input, uint InstanceId : SV_InstanceID) { return Uber_VS(Input, InstanceId); }
FUberOutput mainPS(VS_OUT Input) { return Uber_PS(Input); }
