#pragma once
// Factory for the built-in vphysics replacement.
// Include this instead of loading vphysics.dll at runtime.

class IPhysicsCollision;
class IPhysicsSurfaceProps;

IPhysicsCollision*   CreateMiniPhysicsCollision();
IPhysicsSurfaceProps* CreateMiniPhysicsSurfaceProps();
