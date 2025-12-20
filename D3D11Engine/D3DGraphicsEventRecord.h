#pragma once

#include "GraphicsEventRecord.h"

class D3DGraphicsEventRecord :
    public GraphicsEventRecord {
public:
    D3DGraphicsEventRecord() = default;

    D3DGraphicsEventRecord( ID3DUserDefinedAnnotation* userAnnotation, LPCWSTR region )
        : m_Annotation( userAnnotation )
    {
        if ( m_Annotation ) {
            m_Annotation->BeginEvent( region );
        }
    }
    ~D3DGraphicsEventRecord() override {
        if ( m_Annotation ) {
            m_Annotation->EndEvent();
        }
        m_Annotation = nullptr;
    }

    D3DGraphicsEventRecord( const D3DGraphicsEventRecord& other ) = delete;
    D3DGraphicsEventRecord& operator=( const D3DGraphicsEventRecord& ) = delete;

    D3DGraphicsEventRecord( D3DGraphicsEventRecord&& other ) noexcept
        : m_Annotation( std::move( other.m_Annotation ) )
    {
        other.m_Annotation = nullptr;
    }

private:
    ID3DUserDefinedAnnotation* m_Annotation;
};
