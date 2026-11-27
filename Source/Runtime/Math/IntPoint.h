#pragma once

#include <functional>
#include "Core/Types.h"

struct FIntPoint
{
	int32 X = 0;
	int32 Y = 0;

	FIntPoint() = default;
	FIntPoint(int32 InX, int32 InY) : X(InX), Y(InY) {}

	bool operator==(const FIntPoint& Other) const { return X == Other.X && Y == Other.Y; }
	bool operator!=(const FIntPoint& Other) const { return !(*this == Other); }

	bool operator<(const FIntPoint& Other) const
	{
		return (Y != Other.Y) ? (Y < Other.Y) : (X < Other.X);
	}

	FIntPoint operator+(const FIntPoint& Other) const { return { X + Other.X, Y + Other.Y }; }
	FIntPoint operator-(const FIntPoint& Other) const { return { X - Other.X, Y - Other.Y }; }
};

template <>
struct std::hash<FIntPoint>
{
	size_t operator()(const FIntPoint& P) const noexcept
	{
		const uint64 Key = (static_cast<uint64>(static_cast<uint32>(P.X)) << 32)
			|  static_cast<uint64>(static_cast<uint32>(P.Y));
		return std::hash<uint64>{}(Key);
	}
};
