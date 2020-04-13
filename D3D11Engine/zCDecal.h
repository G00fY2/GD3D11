#pragma once

#include "HookedFunctions.h"
#include "zCMaterial.h"
#include "zCVisual.h"

struct DecalSettings {
	zCMaterial * DecalMaterial;
	float2 DecalSize;
	float2 DecalOffset;
	BOOL DecalTwoSided;
	BOOL IgnoreDayLight;
	BOOL DecalOnTop;
};

class zCDecal : public zCVisual {
public:
	DecalSettings * GetDecalSettings() {
		return (DecalSettings *)THISPTR_OFFSET(GothicMemoryLocations::zCDecal::Offset_DecalSettings);
	}

	bool GetAlphaTestEnabled() {
#ifdef BUILD_GOTHIC_1_08k
		return GetDecalSettings()->DecalMaterial->GetAlphaFunc() == zMAT_ALPHA_FUNC_TEST;
#else
		XCALL(GothicMemoryLocations::zCDecal::GetAlphaTestEnabled);
#endif
	}
};
