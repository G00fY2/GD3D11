#pragma once
#include "HookedFunctions.h"

class zCParser {
public:
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
    static zCParser* GetParser() { return nullptr; }
#else
    static zCParser* GetParser() { return reinterpret_cast<zCParser*>(GothicMemoryLocations::GlobalObjects::zCParser); }
#endif

    
    int GetIndex(const zSTRING& name)
    {
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
        return -1;
#else
        return reinterpret_cast<int(__thiscall*)(void*, const zSTRING*)>(GothicMemoryLocations::zCParser::GetIndex)(this, &name);
#endif
    }
    
    void CallFunc(int symbolId,  ... ) {;
        
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
    
#else
        va_list args;
        va_start(args, symbolId);
        CallFuncInternal(this, symbolId, args);
        va_end(args);
#endif
    }
    
    void CallFunc(const zSTRING& name) {
        
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
#else
        const int symbolId = GetIndex(name);
        
        if (symbolId != -1) {
            CallFuncInternal(this, symbolId);
        }
#endif
    }
    
private:
    static void CallFuncInternal(zCParser* p, int symbolId,  ... ) {
        
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
#else
        va_list args;
        va_start(args, symbolId);
        const auto zCParser_CallFunc = reinterpret_cast<void(__fastcall*)(zCParser*, int, ...)>(GothicMemoryLocations::zCParser::CallFunc);
        zCParser_CallFunc(p, symbolId, args);
        va_end(args);
#endif
    }
};
