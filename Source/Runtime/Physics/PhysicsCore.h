#pragma once

namespace physx
{
	class PxFoundation;
	class PxPhysics;
	class PxCpuDispatcher;
	class PxDefaultCpuDispatcher;
	class PxMaterial;
}

class FPhysicsCore
{
public:
	static bool Init();
	static void Shutdown();

	static bool IsInitialized() { return Physics != nullptr; }
	static physx::PxPhysics* GetPhysics() { return Physics; }
	static physx::PxCpuDispatcher* GetCpuDispatcher();
	static physx::PxMaterial* GetDefaultMaterial() { return DefaultMaterial; }

private:
	inline static physx::PxFoundation* Foundation = nullptr;
	inline static physx::PxPhysics* Physics = nullptr;
	inline static physx::PxDefaultCpuDispatcher* CpuDispatcher = nullptr;
	inline static physx::PxMaterial* DefaultMaterial = nullptr;
	inline static bool bExtensionInitialized = false;
	inline static bool bVehicleExtensionInitialized = false;
};
