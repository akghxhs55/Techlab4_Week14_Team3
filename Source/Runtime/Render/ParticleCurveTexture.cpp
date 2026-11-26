#include "EnginePCH.h"
#include "Render/ParticleCurveTexture.h"
#include "Render/RenderCommand.h"
#include <algorithm>
#include <cstring>

namespace
{
	bool bParticleCurveTextureAlive = false;
}

FParticleCurveTexture& FParticleCurveTexture::Get()
{
	static FParticleCurveTexture Instance;
	return Instance;
}

bool FParticleCurveTexture::IsInitialized()
{
	return bParticleCurveTextureAlive;
}

bool FParticleCurveTexture::Init()
{
	if (bShutDown)
	{
		return false;
	}
	if (RowFreeSpans.IsEmpty())
	{
		ResetFreeSpans();
	}
	if (AtlasTexture)
	{
		return true;
	}

	TArray<FVector4> InitialPixels;
	InitialPixels.SetNum(AtlasWidth * AtlasHeight);
	for (uint32 i = 0; i < AtlasWidth * AtlasHeight; ++i)
	{
		InitialPixels[i] = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
	}

	FTextureDesc Desc{};
	Desc.Width = AtlasWidth;
	Desc.Height = AtlasHeight;
	Desc.Format = ETextureFormat::R32G32B32A32_Float;
	Desc.BindFlags = ETextureBindFlags::ShaderResource;
	Desc.Usage = ETextureUsage::Default;

	AtlasTexture = RenderCommand::CreateTexture2D(Desc, InitialPixels.GetData());
	if (!AtlasTexture)
	{
		return false;
	}

	bParticleCurveTextureAlive = true;
	return true;
}

void FParticleCurveTexture::ResetFreeSpans()
{
	RowFreeSpans.SetNum(AtlasHeight);
	for (uint32 Row = 0; Row < AtlasHeight; ++Row)
	{
		RowFreeSpans[Row].Reset();
		RowFreeSpans[Row].Add(FTexelSpan{ 0, static_cast<uint16>(AtlasWidth) });
	}
	AllocatedTexels = 0;
	ActiveCurveCount = 0;
}

void FParticleCurveTexture::Shutdown()
{
	bParticleCurveTextureAlive = false;
	bShutDown = true;
	PendingUploads.Empty();
	RowFreeSpans.Empty();
	AllocatedTexels = 0;
	ActiveCurveCount = 0;
	AtlasTexture.reset();
}

FParticleCurveAtlasHandle FParticleCurveTexture::MakeHandle(uint16 X, uint16 Y, uint16 Size) const
{
	FParticleCurveAtlasHandle Handle{};
	Handle.X = X;
	Handle.Y = Y;
	Handle.Size = Size;
	Handle.UStart = (static_cast<float>(X) + 0.5f) / static_cast<float>(AtlasWidth);
	Handle.UScale = Size > 1 ? static_cast<float>(Size - 1) / static_cast<float>(AtlasWidth) : 0.0f;
	Handle.VCoord = (static_cast<float>(Y) + 0.5f) / static_cast<float>(AtlasHeight);
	return Handle;
}

void FParticleCurveTexture::EnqueueUpload(uint16 X, uint16 Y, uint16 Width, const TArray<FVector4>& Samples)
{
	FPendingUpload Upload{};
	Upload.X = X;
	Upload.Y = Y;
	Upload.Texels.SetNum(Width);

	for (uint16 Index = 0; Index < Width; ++Index)
	{
		if (Samples.IsEmpty())
		{
			Upload.Texels[Index] = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
		}
		else if (Index < Samples.Num())
		{
			Upload.Texels[Index] = Samples[Index];
		}
		else
		{
			Upload.Texels[Index] = Samples.Last();
		}
	}

	PendingUploads.Add(std::move(Upload));
}

FParticleCurveAtlasHandle FParticleCurveTexture::AllocateCurve(const TArray<FVector4>& Samples)
{
	if (bShutDown)
	{
		return FParticleCurveAtlasHandle{};
	}
	if (RowFreeSpans.IsEmpty())
	{
		ResetFreeSpans();
	}

	const uint16 RequestedWidth = static_cast<uint16>(std::clamp<int32>(Samples.IsEmpty() ? 1 : Samples.Num(), 1, static_cast<int32>(MaxCurveSampleWidth)));

	for (uint32 Row = 0; Row < AtlasHeight; ++Row)
	{
		TArray<FTexelSpan>& Spans = RowFreeSpans[Row];
		for (int32 SpanIndex = 0; SpanIndex < Spans.Num(); ++SpanIndex)
		{
			FTexelSpan& Span = Spans[SpanIndex];
			if (Span.Size < RequestedWidth)
			{
				continue;
			}

			const FParticleCurveAtlasHandle Handle = MakeHandle(Span.Begin, static_cast<uint16>(Row), RequestedWidth);
			Span.Begin += RequestedWidth;
			Span.Size -= RequestedWidth;
			if (Span.Size == 0)
			{
				Spans.RemoveAt(SpanIndex);
			}

			AllocatedTexels += RequestedWidth;
			++ActiveCurveCount;
			EnqueueUpload(Handle.X, Handle.Y, RequestedWidth, Samples);
			return Handle;
		}
	}

	return FParticleCurveAtlasHandle{};
}

void FParticleCurveTexture::UpdateCurveSamples(const FParticleCurveAtlasHandle& Handle, const TArray<FVector4>& Samples)
{
	if (bShutDown || !Handle.IsValid())
	{
		return;
	}
	EnqueueUpload(Handle.X, Handle.Y, Handle.Size, Samples);
}

void FParticleCurveTexture::FreeCurve(FParticleCurveAtlasHandle& InOutHandle)
{
	if (!InOutHandle.IsValid())
	{
		return;
	}

	if (!bShutDown && InOutHandle.Y < RowFreeSpans.Num())
	{
		TArray<FTexelSpan>& Spans = RowFreeSpans[InOutHandle.Y];
		const FTexelSpan Freed{ InOutHandle.X, InOutHandle.Size };
		int32 InsertIndex = 0;
		while (InsertIndex < Spans.Num() && Spans[InsertIndex].Begin < Freed.Begin)
		{
			++InsertIndex;
		}
		Spans.Insert(Freed, InsertIndex);

		if (InsertIndex > 0)
		{
			FTexelSpan& Prev = Spans[InsertIndex - 1];
			if (Prev.Begin + Prev.Size == Spans[InsertIndex].Begin)
			{
				Prev.Size += Spans[InsertIndex].Size;
				Spans.RemoveAt(InsertIndex);
				--InsertIndex;
			}
		}
		if (InsertIndex + 1 < Spans.Num())
		{
			FTexelSpan& Curr = Spans[InsertIndex];
			if (Curr.Begin + Curr.Size == Spans[InsertIndex + 1].Begin)
			{
				Curr.Size += Spans[InsertIndex + 1].Size;
				Spans.RemoveAt(InsertIndex + 1);
			}
		}

		AllocatedTexels = AllocatedTexels >= InOutHandle.Size ? AllocatedTexels - InOutHandle.Size : 0;
		if (ActiveCurveCount > 0)
		{
			--ActiveCurveCount;
		}
	}

	InOutHandle.Invalidate();
}

void FParticleCurveTexture::FlushPendingUploads()
{
	if (bShutDown || PendingUploads.IsEmpty())
	{
		return;
	}
	if (!AtlasTexture && !Init())
	{
		return;
	}

	for (const FPendingUpload& Upload : PendingUploads)
	{
		if (Upload.Texels.IsEmpty())
		{
			continue;
		}
		RenderCommand::UpdateTextureSubregion(
			AtlasTexture.get(),
			Upload.X,
			Upload.Y,
			static_cast<uint32>(Upload.Texels.Num()),
			1,
			Upload.Texels.GetData(),
			static_cast<uint32>(Upload.Texels.Num() * sizeof(FVector4)));
	}
	PendingUploads.Reset();
}
