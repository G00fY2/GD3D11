#include "pch.h"
#include "BaseLineRenderer.h"

using namespace DirectX;
using namespace DirectX::SimpleMath;

BaseLineRenderer::BaseLineRenderer()
{
}


BaseLineRenderer::~BaseLineRenderer()
{
}

/** Plots a vector of floats */
void BaseLineRenderer::PlotNumbers(const std::vector<float> & values, const Vector3 & location, const Vector3 & direction, float distance, float heightScale, const float4& color)
{
	for(unsigned int i=1;i<values.size();i++)
	{
		AddLine(LineVertex(location + (direction * (float)(i-1) * distance) + Vector3(0, 0,values[i-1]*heightScale), color), 
				LineVertex(location + (direction * (float)i * distance) + Vector3(0, 0,values[i]*heightScale), color));
	}
}

/** Adds a triangle to the renderlist */
void BaseLineRenderer::AddTriangle(const Vector3 & t0, const Vector3 & t1, const Vector3 & t2, const float4& color)
{
	AddLine(LineVertex(t0, color), LineVertex(t1, color));
	AddLine(LineVertex(t0, color), LineVertex(t2, color));
	AddLine(LineVertex(t1, color), LineVertex(t2, color));
}

/** Adds a point locator to the renderlist */
void BaseLineRenderer::AddPointLocator(const Vector3 & location, float size, const float4& color)
{
	Vector3 u = location; u.z += size;
	Vector3 d = location; d.z -= size;

	Vector3 r = location; r.x += size;
	Vector3 l = location; l.x -= size;

	Vector3 b = location; b.y += size;
	Vector3 f = location; f.y -= size;

	AddLine(LineVertex(u, color), LineVertex(d, color));
	AddLine(LineVertex(r, color), LineVertex(l, color));
	AddLine(LineVertex(f, color), LineVertex(b, color));
}

/** Adds a plane to the renderlist */
void BaseLineRenderer::AddPlane(const Vector4& plane, const Vector3 & origin, float size, const float4& color)
{
	Vector3 pNormal = Vector3(plane);

	Vector3 DebugPlaneP1;
	DebugPlaneP1.x = 1;
	DebugPlaneP1.y = 1;
	DebugPlaneP1.z = ((-plane.x - plane.y) / plane.z);

	DebugPlaneP1.Normalize();

	Vector3 DebugPlaneP2
		= pNormal.Cross(DebugPlaneP1);

	//DebugPlaneP2 += SlidingPlaneOrigin;
	Vector3 & p1 = DebugPlaneP1;
	Vector3 & p2 = DebugPlaneP2;
	Vector3 O = origin;

	Vector3 from; Vector3 to;
	from = (O - p1) - p2; 
	to = (O - p1) + p2; 
	AddLine(LineVertex(from), LineVertex(to));

	from = (O - p1) + p2; 
	to = (O + p1) + p2; 
	AddLine(LineVertex(from), LineVertex(to));

	from = (O + p1) + p2; 
	to = (O + p1) - p2; 
	AddLine(LineVertex(from), LineVertex(to));

	from = (O + p1) - p2; 
	to = (O - p1) - p2; 
	AddLine(LineVertex(from), LineVertex(to));
}

/** Adds an AABB-Box to the renderlist */
void BaseLineRenderer::AddAABB(const Vector3 & location, float halfSize, const float4& color)
{
	// Bottom -x -y -z to +x -y -z
	AddLine(LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z - halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z - halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z - halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z - halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z - halfSize), color));

	// Top
	AddLine(LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z + halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z + halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z + halfSize), color));

	AddLine(LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z + halfSize), color));

	// Sides
	AddLine(LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y - halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y - halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x + halfSize, location.y + halfSize, location.z - halfSize), color));

	AddLine(LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z + halfSize), color), LineVertex(Vector3(location.x - halfSize, location.y + halfSize, location.z - halfSize), color));

}

/** Adds an AABB-Box to the renderlist */
void BaseLineRenderer::AddAABB(const Vector3 & location, const Vector3 &  halfSize, const float4 & color)
{
	AddAABBMinMax(Vector3(	location.x - halfSize.x, 
						location.y - halfSize.y, 
						location.z - halfSize.z), Vector3(	location.x + halfSize.x, 
															location.y + halfSize.y, 
															location.z + halfSize.z), color);
}



void BaseLineRenderer::AddAABBMinMax(const Vector3 & min, const Vector3 & max, const float4& color)
{
	AddLine(LineVertex(Vector3(min.x, min.y, min.z), color), LineVertex(Vector3(max.x, min.y, min.z), color));
	AddLine(LineVertex(Vector3(max.x, min.y, min.z), color), LineVertex(Vector3(max.x, max.y, min.z), color));
	AddLine(LineVertex(Vector3(max.x, max.y, min.z), color), LineVertex(Vector3(min.x, max.y, min.z), color));
	AddLine(LineVertex(Vector3(min.x, max.y, min.z), color), LineVertex(Vector3(min.x, min.y, min.z), color));
													
	AddLine(LineVertex(Vector3(min.x, min.y, max.z), color), LineVertex(Vector3(max.x, min.y, max.z), color));
	AddLine(LineVertex(Vector3(max.x, min.y, max.z), color), LineVertex(Vector3(max.x, max.y, max.z), color));
	AddLine(LineVertex(Vector3(max.x, max.y, max.z), color), LineVertex(Vector3(min.x, max.y, max.z), color));
	AddLine(LineVertex(Vector3(min.x, max.y, max.z), color), LineVertex(Vector3(min.x, min.y, max.z), color));
												
	AddLine(LineVertex(Vector3(min.x, min.y, min.z), color), LineVertex(Vector3(min.x, min.y, max.z), color));
	AddLine(LineVertex(Vector3(max.x, min.y, min.z), color), LineVertex(Vector3(max.x, min.y, max.z), color));
	AddLine(LineVertex(Vector3(max.x, max.y, min.z), color), LineVertex(Vector3(max.x, max.y, max.z), color));
	AddLine(LineVertex(Vector3(min.x, max.y, min.z), color), LineVertex(Vector3(min.x, max.y, max.z), color));
}

/** Adds a ring to the renderlist */
void BaseLineRenderer::AddRingZ(const Vector3 & location, float size, const float4& color, int res)
{
	std::vector<Vector3> points;
	float step = (float)(XM_PI * 2) / (float)res;

	for(int i=0;i<res;i++)
	{
		points.push_back(Vector3(size * sinf(step * i) + location.x, size * cosf(step * i) + location.y, location.z));
	}

	for(unsigned int i=0; i<points.size()-1;i++)
	{
		AddLine(LineVertex(points[i], color), LineVertex(points[i+1], color));
	}

	AddLine(LineVertex(points[points.size()-1], color), LineVertex(points[0], color));
}

/** Draws a wireframe mesh */
void BaseLineRenderer::AddWireframeMesh(const std::vector<ExVertexStruct> & vertices, const std::vector<VERTEX_INDEX> & indices, const float4& color, const XMMATRIX* world)
{
	for(size_t i=0;i<indices.size();i+=3)
	{
		Vector3 vx[3];
		for(int v=0;v<3;v++)
		{
			if (world) {
				auto transformed = XMVector3TransformCoord(vertices[indices[i + v]].Position, *world);
				XMStoreFloat3(&vx[v], transformed);
			}
			else
				vx[v] = vertices[indices[i + v]].Position;
		}

		AddTriangle(vx[0], vx[1], vx[2], color);
	}
}