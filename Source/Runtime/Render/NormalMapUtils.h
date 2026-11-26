#pragma once

#include "Render/Texture2D.h"

namespace NormalMapUtils
{
	inline bool IsSRGBFormat(DXGI_FORMAT Format)
	{
		switch (Format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return true;

		default:
			return false;
		}
	}

	// 노말 인코딩 종류 반환
	inline float GetEncoding(FTexture2D* Texture)
	{
		if (!Texture || !Texture->GetSRV())
			return 0.0f;

		D3D11_SHADER_RESOURCE_VIEW_DESC ViewDesc{};
		Texture->GetSRV()->GetDesc(&ViewDesc);

		if (IsSRGBFormat(ViewDesc.Format))
			return 0.0f;

		switch (ViewDesc.Format)
		{
		case DXGI_FORMAT_BC5_UNORM:
			return 2.0f;

		case DXGI_FORMAT_BC5_SNORM:
			return 3.0f;

		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC7_UNORM:
			return 1.0f;

		default:
			return 0.0f;
		}
	}
}
