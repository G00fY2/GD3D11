#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zCTimer.h"
#include "zCPolyStrip.h"

enum EZParticleAlignment {
    zPARTICLE_ALIGNMENT_VELOCITY = 1,
    zPARTICLE_ALIGNMENT_XY = 2,
    zPARTICLE_ALIGNMENT_VELOCITY_3D = 3,
};

class zSTRING;
class zCPolyStrip;
class zCMesh;

struct zTParticle {
    zTParticle* Next;

#ifdef BUILD_GOTHIC_2_6_fix
    DirectX::XMFLOAT3 PositionLocal;
#endif
    DirectX::XMFLOAT3 PositionWS;
    DirectX::XMFLOAT3 Vel;
    float LifeSpan;
    float Alpha;
    float AlphaVel;
    DirectX::XMFLOAT2 Size;
    DirectX::XMFLOAT2 SizeVel;
    DirectX::XMFLOAT3 Color;
    DirectX::XMFLOAT3 ColorVel;

#ifdef BUILD_GOTHIC_1_08k
    float TexAniFrame;
#endif

    zCPolyStrip* PolyStrip; // TODO: Use this too
};

class zCParticleEmitter {
public:


    zCTexture* GetVisTexture() {
        return *(zCTexture**)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexture );
    }

    zTRnd_AlphaBlendFunc GetVisAlphaFunc() {
        return *(zTRnd_AlphaBlendFunc*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlphaBlendFunc );
    }

    int GetVisIsQuadPoly() {
        return *(int*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisIsQuadPoly );
    }

    int GetVisAlignment() {
        return *(int*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlignment );
    }

#ifndef BUILD_GOTHIC_1_08k


    int GetVisTexAniIsLooping() {
        return *(int*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisTexAniIsLooping );
    }

    float GetVisAlphaStart() {
        return *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_VisAlphaStart );
    }

    float GetAlphaDist() {
        return *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleEmitter::Offset_AlphaDist );
    }



#else
    int GetVisTexAniIsLooping() {
        return 0;
    }

    float GetAlphaDist() {
        return 0;
    }

    float GetVisAlphaStart() {
        return 0;
    }
#endif
};

class zCStaticPfxList {
public:
    void TouchPfx( zCParticleFX* pfx ) {
        XCALL( GothicMemoryLocations::zCStaticPfxList::TouchPFX );
    }

    zCParticleFX* PfxListHead;
    zCParticleFX* PfxListTail;
    int	NumInPfxList;
};

class zCParticleFX {
public:
    zCParticleFX();
    ~zCParticleFX();

    /** Hooks the functions of this Class */
    static void Hook() {
        XHook( HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor, GothicMemoryLocations::zCParticleFX::Destructor, zCParticleFX::Hooked_Destructor );
    }

    static void __fastcall Hooked_Destructor( void* thisptr, void* unknwn ) {
        hook_infunc
            // Notify the world
            if ( Engine::GAPI )
                Engine::GAPI->OnParticleFXDeleted( (zCParticleFX*)thisptr );

        HookedFunctions::OriginalFunctions.original_zCParticleFXDestructor( thisptr );

        hook_outfunc
    }

    static float SinEase( float value ) {
        return (float)((sin( value * DirectX::XM_PI - DirectX::XM_PIDIV2 ) + 1.0) / 2.0);
    }

    static float SinSmooth( float value ) {
        if ( value < 0.5f )
            return SinEase( value * 2 );
        else
            return 1.0f - SinEase( (value - 0.5f) * 2 );
    }

    zCMesh* GetPartMeshQuad() {
        return *(zCMesh**)GothicMemoryLocations::zCParticleFX::OBJ_s_partMeshQuad;
    }

    zCParticleEmitter* GetEmitter() {
        return *(zCParticleEmitter**)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_Emitters );
    }

    zTParticle* GetFirstParticle() {
        return *(zTParticle**)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_FirstParticle );
    }

    void SetFirstParticle( zTParticle* particle ) {
        *(zTParticle**)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_FirstParticle ) = particle;
    }


    zCVob* GetConnectedVob()
    {
        return *(zCVob**)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_ConnectedVob );
    }

    float GetTimeScale() {
        return *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_TimeScale );
    }

    float* GetPrivateTotalTime() {
        return (float*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_PrivateTotalTime );
    }

    int UpdateParticleFX() {
        XCALL( GothicMemoryLocations::zCParticleFX::UpdateParticleFX );
    }

    void CheckDependentEmitter() {
        XCALL( GothicMemoryLocations::zCParticleFX::CheckDependentEmitter );
    }

    /*bool IsRemoved()
    {
        zCParticleFX* next = *(zCParticleFX **)THISPTR_OFFSET(GothicMemoryLocations::zCParticleFX::Offset_NextPFX);
        zCParticleFX* prev = *(zCParticleFX **)THISPTR_OFFSET(GothicMemoryLocations::zCParticleFX::Offset_PrevPFX);

        // The remove-function sets these two to nullptr
        return prev == nullptr && next == nullptr;
    }*/

    zCStaticPfxList* GetStaticPFXList() {
        return (zCStaticPfxList*)GothicMemoryLocations::zCParticleFX::OBJ_s_pfxList;
    }

    void SetLocalTimeF( float t ) {
        *(float*)THISPTR_OFFSET( GothicMemoryLocations::zCParticleFX::Offset_LocalFrameTimeF ) = t;
    }


    void UpdateTime() {
        SetLocalTimeF( GetTimeScale() * zCTimer::GetTimer()->frameTimeFloat );
    }

    void CreateParticlesUpdateDependencies() {
        XCALL( GothicMemoryLocations::zCParticleFX::CreateParticlesUpdateDependencies );
    }

    void UpdateParticle( zTParticle* p ) {
        XCALL( GothicMemoryLocations::zCParticleFX::UpdateParticle );
    }

    void SetVisualUsedBy( zCVob* vob ) {
        XCALL( GothicMemoryLocations::zCParticleFX::SetVisualUsedBy );
    }

    int GetVisualDied() {
        XCALL( GothicMemoryLocations::zCParticleFX::GetVisualDied );
    }
};

