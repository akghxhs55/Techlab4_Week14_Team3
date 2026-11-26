#pragma once

#include <foundation/PxVec3.h>
#include <foundation/PxQuat.h>
#include <foundation/PxTransform.h>

#include "Math/Transform.h"

inline physx::PxVec3 K2PVector(const FVector& V)
{
	return { V.X, V.Y, V.Z };
}

inline FVector P2KVector(const physx::PxVec3& V)
{
	return { V.x, V.y, V.z };
}

inline physx::PxQuat K2PQuat(const FQuat& Q)
{
	return { Q.X, Q.Y, Q.Z, Q.W };
}

inline FQuat P2KQuat(const physx::PxQuat& Q)
{
	return { Q.x, Q.y, Q.z, Q.w };
}

inline physx::PxTransform K2PTransform(const FTransform& T)
{
	return { K2PVector(T.Location), K2PQuat(T.Rotation.Normalized()) };
}

inline FTransform P2KTransform(const physx::PxTransform& T)
{
	return { P2KQuat(T.q), P2KVector(T.p) };
}