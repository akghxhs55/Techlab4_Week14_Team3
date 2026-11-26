#pragma once

#include "Core/FrameTime.h"

// UE TRange<FFrameNumber> 축소판: [Lower, Upper). 열린 쪽은 무한
struct FMovieSceneFrameRange
{
	FFrameNumber LowerBound;
	FFrameNumber UpperBound;
	bool bHasLowerBound = false;
	bool bHasUpperBound = false;

	static FMovieSceneFrameRange All() { return {}; }
	static FMovieSceneFrameRange Make(FFrameNumber Lower, FFrameNumber Upper)
	{
		FMovieSceneFrameRange Range;
		Range.LowerBound = Lower;
		Range.UpperBound = Upper;
		Range.bHasLowerBound = Range.bHasUpperBound = true;
		return Range;
	}

	bool Contains(FFrameNumber Frame) const
	{
		if (bHasLowerBound && Frame < LowerBound) return false;
		if (bHasUpperBound && Frame >= UpperBound) return false;
		return true;
	}
	bool Contains(const FFrameTime& Time) const { return Contains(Time.FloorToFrame()); }

	FFrameNumber GetLowerBoundValue() const { return LowerBound; }
	FFrameNumber GetUpperBoundValue() const { return UpperBound; }
	int32 Size() const { return UpperBound.Value - LowerBound.Value; }
};
