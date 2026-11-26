cbuffer DepthOfFieldConstants : register(b0)
{
    float InvWidth; 
    float InvHeight; // 주변 픽셀을 읽을 때 픽셀 간격을 UV 간격으로 변환
    float ProjectionA;
    float ProjectionB; // 깊이 버퍼 값을 카메라로부터의 거리로 복원
    float FocusDistanceMM; // S
    float FocalLengthMM; // F
    float SensorHeightMM; // 센서 전체의 세로 길이. CoC의 크기를 화면 픽셀 크기로 바꿀때 사용
    float ApertureFStop; // F/D
    float DepthBlurHalfDistanceCM;
    float DepthBlurRadiusPixels; // 별도의 원거리 Depth Blur 계산
    float MaxRadiusPixels; // 샘플링 범위를 제한해 비용을 억제
    float MinimumFStop; 
    float DiaphragmBladeCount; //	CoC의 조리개 모양 계산
    float SkyDepthCM;
    float2 Padding;
};

Texture2D<float4> SceneColor : register(t0);
Texture2D<float> SceneDepth : register(t1);
SamplerState LinearClampSampler : register(s0);

struct VSOutput
{
    float4 Position : SV_Position;
};

VSOutput mainVS(uint VertexID : SV_VertexID)
{
    float2 UV = float2((VertexID << 1) & 2, VertexID & 2);
    VSOutput Output;
    Output.Position = float4(UV * float2(2, -2) + float2(-1, 1), 0, 1);
    return Output;
}

float ViewDepth(float DeviceZ)
{
    return DeviceZ >= 1.0f ? SkyDepthCM : max(ProjectionB / (DeviceZ - ProjectionA), 0.0f);
}

float BlurRadius(float Z) 
{
    if (Z <= 0.0f) return 0.0f;
    float LensRadius = 0.0f;
    if (FocusDistanceMM > FocalLengthMM)
    {
        float Zmm = Z * 10.0f;
        float CocMM = FocalLengthMM * FocalLengthMM * abs(Zmm - FocusDistanceMM) /
            (ApertureFStop * Zmm * (FocusDistanceMM - FocalLengthMM)); // Coc 계산
        LensRadius = 0.5f * CocMM / SensorHeightMM / InvHeight; // 화면의 픽셀반경으로 변경
    }
    float DistanceRadius = DepthBlurHalfDistanceCM > 0.0f
        ? DepthBlurRadiusPixels * Z / (Z + DepthBlurHalfDistanceCM) : 0.0f;
    return min(max(LensRadius, DistanceRadius), MaxRadiusPixels);
}

float BladeScale(float Angle)
{
    float Sector = 6.2831853f / DiaphragmBladeCount;
    float LocalAngle = frac((Angle + 3.14159265f) / Sector) * Sector - 0.5f * Sector;
    float Polygon = cos(0.5f * Sector) / max(cos(LocalAngle), 0.01f);
    float PolygonWeight = saturate((ApertureFStop - MinimumFStop) / max(MinimumFStop, 0.1f));
    return lerp(1.0f, Polygon, PolygonWeight);
}

float4 mainPS(VSOutput Input) : SV_Target
{
    int2 Pixel = int2(Input.Position.xy);
    float2 UV = Input.Position.xy * float2(InvWidth, InvHeight);
    float CenterDepth = ViewDepth(SceneDepth.Load(int3(Pixel, 0)));
    float Radius = BlurRadius(CenterDepth);
    float4 Center = SceneColor.Load(int3(Pixel, 0));
    if (Radius < 0.5f) return Center;

    float4 Sum = Center;
    float WeightSum = 1.0f;
    [unroll] for (int Ring = 1; Ring <= 4; ++Ring)
    {
        [unroll] for (int Sample = 0; Sample < 8; ++Sample)
        {
            float Angle = 6.2831853f * (Sample + 0.5f * (Ring & 1)) / 8.0f;
            float2 Offset = float2(cos(Angle), sin(Angle)) * Radius * (Ring / 4.0f) * BladeScale(Angle);
            float2 SampleUV = saturate(UV + Offset * float2(InvWidth, InvHeight));
            int2 SamplePixel = clamp(int2(SampleUV / float2(InvWidth, InvHeight)), int2(0, 0),
                int2(1.0f / InvWidth - 1, 1.0f / InvHeight - 1));
            float SampleDepth = ViewDepth(SceneDepth.Load(int3(SamplePixel, 0)));
            // 가까운 물체의 색이 먼 물체의 흐림에 과도하게 섞이지 않도록 깊이 경계를 제한한다.
            float Weight = SampleDepth > 0.0f && abs(SampleDepth - CenterDepth) < max(CenterDepth * 0.05f, 5.0f)
                ? 1.0f : 0.0f;
            Sum += SceneColor.SampleLevel(LinearClampSampler, SampleUV, 0) * Weight;
            WeightSum += Weight;
        }
    }
    return Sum / WeightSum;
}
