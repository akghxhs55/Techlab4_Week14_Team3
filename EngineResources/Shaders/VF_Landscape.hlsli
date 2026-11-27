// Landscape: 정점에는 격자 좌표만 있고, 높이와 노멀은 하이트맵 텍스처에서 읽는다 (UE FLandscapeVertexFactory)
// 입력 레이아웃은 VertexFactory.cpp의 GLandscapeElements와 같아야 한다
struct FVertexFactoryInput
{
    uint4 Packed : POSITION; // x, y: 이 LOD 격자 좌표, z: LOD 번호, w: 미사용
};

Texture2D<float4> LandscapeHeightmap : register(t19);

// FLandscapeShaderParams(LandscapeRender.h, 2단계)와 레이아웃이 같아야 한다
cbuffer LandscapeParams : register(b6)
{
    float4 LandscapeSectionParams; // x: 섹션 쿼드 수(63), y: LOD0 한 변 정점 수(64), z: LOD, w: 미사용
    int4 LandscapeHeightmapOffset; // xy: 하이트맵 안에서 이 섹션의 시작 텍셀 (청크별 텍스처면 0)
};

// LandscapeDataAccess::GetLocalHeight와 같은 식이어야 CPU 질의·PhysX와 높이가 일치한다
float LandscapeDecodeHeight(float4 Texel)
{
    const uint High = (uint) round(Texel.r * 255.0f);
    const uint Low = (uint) round(Texel.g * 255.0f);
    const float Raw = (float) ((High << 8) | Low);
    return (Raw - 32768.0f) * (1.0f / 128.0f);
}

float3 LandscapeDecodeNormal(float4 Texel)
{
    const float2 XY = Texel.ba * 2.0f - 1.0f;
    return float3(XY, sqrt(saturate(1.0f - dot(XY, XY))));
}

FVFIntermediates VF_GetIntermediates(FVertexFactoryInput Input, float4x4 World, float4x4 NormalMatrix)
{
    const uint LOD = Input.Packed.z;
    const uint LodVerts = ((uint) LandscapeSectionParams.y) >> LOD;
    const float LodQuads = (float) (LodVerts - 1);
    const float SectionQuads = LandscapeSectionParams.x;
    const float2 Grid = float2(Input.Packed.xy);

    // LOD k 격자는 LOD0의 부분집합이 아니라 같은 너비를 (LodVerts-1)칸으로 나눈 별도 격자다
    const float2 LocalXY = Grid * (SectionQuads / LodQuads);

    // LOD k 정점 = 하이트맵 mip k 텍셀 (1:1). Phase 1은 mip0만 있으므로 LOD0만 그린다.
    // Load: 필터링 없이 정확한 값을 읽어야 CPU·PhysX 높이와 비트 단위로 같다
    const int2 Texel = int2(Input.Packed.xy) + (LandscapeHeightmapOffset.xy >> LOD);
    const float4 Sample = LandscapeHeightmap.Load(int3(Texel, LOD));

    const float3 LocalPos = float3(LocalXY, LandscapeDecodeHeight(Sample));
    const float3 LocalNormal = LandscapeDecodeNormal(Sample);
    // Z-up: 접선은 +X 방향을 표면에 투영
    const float3 LocalTangent = normalize(float3(1.0f, 0.0f, 0.0f) - LocalNormal * LocalNormal.x);

    const float2 UV = Grid / LodQuads; // 섹션 0~1 (Phase 3에서 Splat/타일링 UV로 교체)
    return VF_LocalToWorld(LocalPos, LocalNormal, float4(LocalTangent, 1.0f), UV, float4(1.0f, 1.0f, 1.0f, 1.0f), World, NormalMatrix);
}