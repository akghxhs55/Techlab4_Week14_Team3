cbuffer DepthOfFieldConstants : register(b0)
{
    float InvWidth;
    float InvHeight;
    float ProjectionA;
    float ProjectionB;
    float FocusDistanceMM;
    float FocalLengthMM;
    float SensorHeightMM;
    float ApertureFStop;
    float DepthBlurHalfDistanceCM;
    float DepthBlurRadiusPixels;
    float MaxRadiusPixels;
    float MinimumFStop;
    float DiaphragmBladeCount;
    float SkyDepthCM;
	float NearBlurScale;
	float FarBlurScale;
	float NearMaxRadiusPixels;
	float FarMaxRadiusPixels;
	float2 Padding;
};

struct DOFVSOutput
{
    float4 Position : SV_Position;
};

DOFVSOutput DOFMainVS(uint VertexID)
{
    float2 UV = float2((VertexID << 1) & 2, VertexID & 2);
    DOFVSOutput Output;
    Output.Position = float4(UV * float2(2, -2) + float2(-1, 1), 0, 1);
    return Output;
}

float DOFViewDepth(float DeviceZ)
{
    return DeviceZ >= 1.0f ? SkyDepthCM : max(ProjectionB / (DeviceZ - ProjectionA), 0.0f);
}

float DOFSignedCoC(float Z)
{
    if (Z <= 0.0f) return 0.0f;
    float LensRadius = 0.0f;
    if (FocusDistanceMM > FocalLengthMM)
    {
        float Zmm = Z * 10.0f;
        float CocMM = FocalLengthMM * FocalLengthMM * abs(Zmm - FocusDistanceMM) /
            (ApertureFStop * Zmm * (FocusDistanceMM - FocalLengthMM));
        LensRadius = 0.5f * CocMM / SensorHeightMM / InvHeight;
    }
    float DistanceRadius = DepthBlurHalfDistanceCM > 0.0f
        ? DepthBlurRadiusPixels * Z / (Z + DepthBlurHalfDistanceCM) : 0.0f;
	// 근경과 원경에 서로 다른 배율과 최대 반경 적용
	bool bNear = FocusDistanceMM > 0.0f && Z * 10.0f < FocusDistanceMM;
	float Scale = bNear ? NearBlurScale : FarBlurScale;
	float MaxRadius = bNear ? NearMaxRadiusPixels : FarMaxRadiusPixels;
	float Radius = min(max(LensRadius, DistanceRadius) * max(Scale, 0.0f), MaxRadius);
	return Radius <= 0.0f ? 0.0f : (bNear ? -Radius : Radius);
}

float DOFBladeScale(float Angle)
{
	float Sector = 6.2831853f / max(DiaphragmBladeCount, 3.0f);
	float LocalAngle = frac((Angle + 3.14159265f) / Sector) * Sector - 0.5f * Sector;
	float Polygon = cos(0.5f * Sector) / max(cos(LocalAngle), 0.01f);
	// 다각형 변과 꼭짓점 차이를 강조해 날 수 구분을 뚜렷하게 유지
	float CrispPolygon = saturate(1.0f - (1.0f - Polygon) * 1.6f);
	float PolygonWeight = saturate((ApertureFStop - MinimumFStop) / max(MinimumFStop, 0.1f));
	return lerp(1.0f, CrispPolygon, PolygonWeight);
}
