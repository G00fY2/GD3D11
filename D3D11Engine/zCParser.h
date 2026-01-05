#pragma once
#include "HookedFunctions.h"

class zCParser {
public:
#if defined(BUILD_GOTHIC_1_08k) || defined(BUILD_SPACER)
    void CallFunc( int symbolId, ... ) {

    }

    static zCParser* GetParser() { return nullptr; }
#else
    static zCParser* GetParser() { return reinterpret_cast<zCParser*>(GothicMemoryLocations::GlobalObjects::zCParser); }
    
    
    int GetIndex(const zSTRING& name)
    {
#ifndef BUILD_GOTHIC_1_08k
        return reinterpret_cast<int(__thiscall*)(void*, const zSTRING*)>(GothicMemoryLocations::zCParser::GetIndex)(this, &name);
#endif
        return -1;
    }
    
    void CallFunc(int symbolId,  ... ) {;
        
#ifndef BUILD_GOTHIC_1_08k
        va_list args;
        va_start(args, symbolId);
        CallFuncInternal(this, symbolId, args);
        va_end(args);
#endif
    }
    
    void CallFunc(const zSTRING& name) {
        
#ifndef BUILD_GOTHIC_1_08k
        const int symbolId = GetIndex(name);
        
        if (symbolId != -1) {
            CallFuncInternal(this, symbolId);
        }
#endif
    }
#endif
    
private:
    static void CallFuncInternal(zCParser* p, int symbolId,  ... ) {
        
#ifndef BUILD_GOTHIC_1_08k
        va_list args;
        va_start(args, symbolId);
        const auto zCParser_CallFunc = reinterpret_cast<void(__fastcall*)(zCParser*, int, ...)>(GothicMemoryLocations::zCParser::CallFunc);
        zCParser_CallFunc(p, symbolId, args);
        va_end(args);
#endif
    }
};
