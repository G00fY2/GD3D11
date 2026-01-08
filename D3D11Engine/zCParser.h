#pragma once
#include "HookedFunctions.h"

#if (defined(BUILD_GOTHIC_2_6_fix) || defined(BUILD_GOTHIC_1_08k))
#if !defined(BUILD_SPACER) && !defined(BUILD_1_12F)
#define zCParserSupported
#endif
#endif


class zCParser {
public:
#ifndef zCParserSupported
    static zCParser* GetParser() { return nullptr; }
#else
    static zCParser* GetParser() { return reinterpret_cast<zCParser*>(GothicMemoryLocations::GlobalObjects::zCParser); }
#endif


    int GetIndex( const zSTRING& name )
    {
#ifndef zCParserSupported
        return -1;
#else
        return reinterpret_cast<int( __thiscall* )(void*, const zSTRING*)>(GothicMemoryLocations::zCParser::GetIndex)(this, &name);
#endif
    }

    void CallFunc( int symbolId, ... ) {
#ifndef zCParserSupported

#else
        va_list args;
        va_start( args, symbolId );
        CallFuncInternal( this, symbolId, args );
        va_end( args );
#endif
    }

    void CallFunc( const zSTRING& name ) {
#ifndef zCParserSupported
#else
        const int symbolId = GetIndex( name );

        if ( symbolId != -1 ) {
            CallFuncInternal( this, symbolId );
        }
#endif
    }

private:
    static void CallFuncInternal( zCParser* p, int symbolId, ... ) {

#ifndef zCParserSupported
#else
        va_list args;
        va_start(args, symbolId);
        const auto zCParser_CallFunc = reinterpret_cast<void(__fastcall*)(zCParser*, int, ...)>(GothicMemoryLocations::zCParser::CallFunc);
        zCParser_CallFunc(p, symbolId, args);
        va_end(args);
#endif
    }
};
