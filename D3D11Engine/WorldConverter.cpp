#include "pch.h"
#include "WorldConverter.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "D3D11VertexBuffer.h"
#include "zCPolygon.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "zCVisual.h"
#include "zCVob.h"
#include "zCProgMeshProto.h"
#include "zCMeshSoftSkin.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include <set>
#include "ConstantBufferStructs.h"
#include "D3D11ConstantBuffer.h"
#include "zCMesh.h"
#include "zCLightmap.h"
#include "GMesh.h"
#include "MeshModifier.h"
#include "D3D11Texture.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "zCQuadMark.h"

using namespace DirectX;
using namespace DirectX::SimpleMath;

WorldConverter::WorldConverter()
{
}

WorldConverter::~WorldConverter()
{
}

/** Collects all world-polys in the specific range. Drops all materials that have no alphablending */
void WorldConverter::WorldMeshCollectPolyRange(const Vector3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>> & inSections, std::map<MeshKey, WorldMeshInfo *, cmpMeshKey> & outMeshes) {
	INT2 s = GetSectionOfPos(position);
	MeshKey opaqueKey;
	opaqueKey.Material = nullptr;
	opaqueKey.Info = nullptr;
	opaqueKey.Texture = nullptr;

	WorldMeshInfo * opaqueMesh = new WorldMeshInfo;
	outMeshes[opaqueKey] = opaqueMesh;

	XMVECTOR xmPosition = position;

	// Generate the meshes
	for (auto const& itx : Engine::GAPI->GetWorldSections()) {
		for (auto const& ity : itx.second) {
			const XMVECTOR a = XMVectorSet(static_cast<float>(itx.first - s.x), static_cast<float>(ity.first - s.y), 0, 0);
			if (Toolbox::XMVector2LengthFloat(a) < 2) {
				// Check all polys from all meshes
				for (auto const& it : ity.second.WorldMeshes) {
					WorldMeshInfo * m;
					
					// Create new mesh-part for alphatested surfaces
					if (it.first.Texture && it.first.Texture->HasAlphaChannel()) {
						m = new WorldMeshInfo;
						outMeshes[it.first] = m;
					} else {
						// Just use the same mesh for opaque surfaces
						m = opaqueMesh;
					}

					for (unsigned int i = 0; i < it.second->Indices.size(); i += 3) {
						// Check if one of them is in range

						const float range2 = range*range;
						if (Toolbox::XMVector3LengthSqFloat(xmPosition - XMLoadFloat3(&it.second->Vertices[it.second->Indices[i + 0]].Position)) < range2
							|| Toolbox::XMVector3LengthSqFloat(xmPosition - XMLoadFloat3(&it.second->Vertices[it.second->Indices[i + 1]].Position)) < range2
							|| Toolbox::XMVector3LengthSqFloat(xmPosition - XMLoadFloat3(&it.second->Vertices[it.second->Indices[i + 2]].Position)) < range2)
						{
							for (int v = 0; v < 3; v++)
								m->Vertices.push_back(it.second->Vertices[it.second->Indices[i+v]]);
						}
					}
				}
			}
		}
	}

	// Index all meshes
	for (auto it = outMeshes.begin(); it != outMeshes.end();) {
		if (it->second->Vertices.empty()) {
			it = outMeshes.erase(it);
			continue;
		}

		std::vector<VERTEX_INDEX> indices;
		std::vector<ExVertexStruct> vertices;
		IndexVertices(&it->second->Vertices[0], it->second->Vertices.size(), vertices, indices);

		it->second->Vertices = vertices;
		it->second->Indices = indices;

		// Create the buffers
		Engine::GraphicsEngine->CreateVertexBuffer(&it->second->MeshVertexBuffer);
		Engine::GraphicsEngine->CreateVertexBuffer(&it->second->MeshIndexBuffer);

		// Init and fill them
		it->second->MeshVertexBuffer->Init(&it->second->Vertices[0], it->second->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
		it->second->MeshIndexBuffer->Init(&it->second->Indices[0], it->second->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

		++it;
	}
}

/** Converts a loaded custommesh to be the worldmesh */
XRESULT WorldConverter::LoadWorldMeshFromFile(const std::string & file, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo ** outWrappedMesh)
{
	GMesh* mesh = new GMesh();

	const float worldScale = 100.0f;

	// Check if we have this file cached
	if (Toolbox::FileExists((file + ".mcache").c_str()))
	{
		// Load the meshfile, cached
		mesh->LoadMesh((file + ".mcache").c_str(), worldScale);
	} else
	{
		// Create cache-file
		mesh->LoadMesh(file, worldScale);

		std::vector<MeshInfo *> & meshes = mesh->GetMeshes();
		std::vector<std::string> & textures = mesh->GetTextures();
		std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> gm;

		for(unsigned int m = 0;m<meshes.size();m++)
		{
			auto& meshData = gm[textures[m]];

			meshData.push_back(std::make_pair(meshes[m]->Vertices, meshes[m]->Indices));
		}

		CacheMesh(gm, file + ".mcache");
	}
	

	std::vector<MeshInfo *> & meshes = mesh->GetMeshes();
	std::vector<std::string> & textures = mesh->GetTextures();
	std::map<std::string, D3D11Texture *> loadedTextures;
	std::set<std::string> missingTextures;

	// run through meshes and pack them into sections
	for(unsigned int m = 0;m<meshes.size();m++)
	{
		D3D11Texture * customTexture = nullptr;
		zCMaterial * mat = Engine::GAPI->GetMaterialByTextureName(textures[m]);
		MeshKey key;
		key.Material = mat;
		key.Texture = mat != nullptr ? mat->GetTexture() : nullptr;
		
		// Save missing textures
		if (!mat)
		{
			missingTextures.insert(textures[m]);
		} else
		{
			if (mat->GetMatGroup() == zMAT_GROUP_WATER)
			{
				// Give water surfaces a water-shader
				MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(mat->GetTexture());
				if (info)
				{
					info->PixelShader = "PS_Water";
					info->MaterialType = MaterialInfo::MT_Water;
				}
			}
		}

		//key.Lightmap = poly->GetLightmap();

		for(unsigned int i=0;i<meshes[m]->Vertices.size();i++)
		{
			// Mesh needs to be rotated differently
			meshes[m]->Vertices[i].Position = Vector3(meshes[m]->Vertices[i].Position.x,
				meshes[m]->Vertices[i].Position.y,
				-meshes[m]->Vertices[i].Position.z);

			// Fix disoriented texcoords
			meshes[m]->Vertices[i].TexCoord = Vector2(meshes[m]->Vertices[i].TexCoord.x, -meshes[m]->Vertices[i].TexCoord.y);
		}

		for(unsigned int i=0;i<meshes[m]->Indices.size();i+=3)
		{

			if (meshes[m]->Indices[i] > meshes[m]->Vertices.size() || 
				meshes[m]->Indices[i+1] > meshes[m]->Vertices.size() || 
				meshes[m]->Indices[i+2] > meshes[m]->Vertices.size())
				break; // Catch broken meshes
			
			ExVertexStruct* v[3] = {   &meshes[m]->Vertices[meshes[m]->Indices[i]],
										&meshes[m]->Vertices[meshes[m]->Indices[i+2]],
										&meshes[m]->Vertices[meshes[m]->Indices[i+1]]};


			// Calculate midpoint of this triange to get the section
			Vector3 avgPos = (v[0]->Position + v[1]->Position + v[2]->Position) / 3.0f;
			INT2 sxy = GetSectionOfPos(avgPos);

			WorldMeshSectionInfo& section = (*outSections)[sxy.x][sxy.y];
			section.WorldCoordinates = sxy;

			Vector3 & bbmin = section.BoundingBox.Min;
			Vector3 & bbmax = section.BoundingBox.Max;

			// Check bounding box
			bbmin.x = bbmin.x > v[0]->Position.x ? v[0]->Position.x : bbmin.x;
			bbmin.y = bbmin.y > v[0]->Position.y ? v[0]->Position.y : bbmin.y;
			bbmin.z = bbmin.z > v[0]->Position.z ? v[0]->Position.z : bbmin.z;

			bbmax.x = bbmax.x < v[0]->Position.x ? v[0]->Position.x : bbmax.x;
			bbmax.y = bbmax.y < v[0]->Position.y ? v[0]->Position.y : bbmax.y;
			bbmax.z = bbmax.z < v[0]->Position.z ? v[0]->Position.z : bbmax.z;

			if (section.WorldMeshes.find(key) == section.WorldMeshes.end())
			{
				key.Info = Engine::GAPI->GetMaterialInfoFrom(key.Texture);

				section.WorldMeshes[key] = new WorldMeshInfo;

			}

			for (int i = 0; i < 3; i++)
			{
				section.WorldMeshes[key]->Vertices.push_back(*v[i]);
			}
		}
	}

	// Print textures we couldn't find any materials for if there are any
	if (!missingTextures.empty())
	{
		std::string ms = "\nMissing materials for custom-mesh:\n";

		for(auto it = missingTextures.begin(); it != missingTextures.end(); it++)
		{
			ms += "\t" + (*it)+ "\n";
		}

		LogWarn() << ms;
	}

	// Dont need that anymore
	delete mesh;

	Vector2 avgSections = Vector2(0, 0);
	int numSections = 0;

	std::list<std::vector<ExVertexStruct>*> vertexBuffers;
	std::list<std::vector<VERTEX_INDEX>*> indexBuffers;

	// Create the vertexbuffers for every material
	for (auto const& itx : *outSections)
	{
		for (auto const& ity : itx.second)
		{
			numSections++;
			avgSections += Vector2(static_cast<float>(itx.first), static_cast<float>(ity.first));

			for(auto const& it : ity.second.WorldMeshes)
			{
				std::vector<ExVertexStruct> indexedVertices;
				std::vector<VERTEX_INDEX> indices;
				IndexVertices(&it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices);

				it.second->Vertices = indexedVertices;
				it.second->Indices = indices;

				// Create the buffers
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshVertexBuffer);
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshIndexBuffer);

				// Optimize faces
				it.second->MeshVertexBuffer->OptimizeFaces(&it.second->Indices[0],
					(byte *)&it.second->Vertices[0], 
					it.second->Indices.size(), 
					it.second->Vertices.size(), 
					sizeof(ExVertexStruct));

				// Then optimize vertices
				it.second->MeshVertexBuffer->OptimizeVertices(&it.second->Indices[0],
					(byte *)&it.second->Vertices[0], 
					it.second->Indices.size(), 
					it.second->Vertices.size(), 
					sizeof(ExVertexStruct));

				// Init and fill them
				it.second->MeshVertexBuffer->Init(&it.second->Vertices[0], it.second->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
				it.second->MeshIndexBuffer->Init(&it.second->Indices[0], it.second->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

				// Remember them, to wrap then up later
				vertexBuffers.push_back(&it.second->Vertices);
				indexBuffers.push_back(&it.second->Indices);
			}
		}
	}

	std::vector<ExVertexStruct> wrappedVertices;
	std::vector<unsigned int> wrappedIndices;
	std::vector<unsigned int> offsets;

	// Calculate fat vertexbuffer
	WorldConverter::WrapVertexBuffers(vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets);

	// Propergate the offsets
	int i=0;
	for (auto& itx : *outSections)
	{
		for (auto& ity : itx.second)
		{
			int numIndices = 0;
			for(auto const& it : ity.second.WorldMeshes)
			{
				it.second->BaseIndexLocation = offsets[i];
				numIndices += it.second->Indices.size();

				i++;
			}

			ity.second.NumIndices = numIndices;

			if (!ity.second.WorldMeshes.empty())
				ity.second.BaseIndexLocation = (*ity.second.WorldMeshes.begin()).second->BaseIndexLocation;
		}
	}

	// Create the buffers for wrapped mesh
	MeshInfo * wmi = new MeshInfo;
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshVertexBuffer);
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshIndexBuffer);
	
	// Init and fill them
	wmi->MeshVertexBuffer->Init(&wrappedVertices[0], wrappedVertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	wmi->MeshIndexBuffer->Init(&wrappedIndices[0], wrappedIndices.size() * sizeof(unsigned int), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	*outWrappedMesh = wmi;

	// Calculate the approx midpoint of the world
	avgSections /= static_cast<float>(numSections);

	if (info)
	{
		WorldInfo i;
		i.MidPoint = avgSections * WORLD_SECTION_SIZE;
		i.LowestVertex = 0;
		i.HighestVertex = 0;

		memcpy(info, &i, sizeof(WorldInfo));
	}


	return XR_SUCCESS;
}

/** Converts the worldmesh into a PNAEN-buffer */
HRESULT WorldConverter::ConvertWorldMeshPNAEN(zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo ** outWrappedMesh)
{
	// Go through every polygon and put it into it's section
	for(unsigned int i=0;i<numPolygons;i++)
	{
		zCPolygon* poly = polys[i];

		// Check if we even need this polygon
		if (poly->GetPolyFlags()->GhostOccluder || 
			poly->GetPolyFlags()->PortalPoly)
			continue;

		// Use the section of the first point for the whole polygon
		INT2 section = GetSectionOfPos(poly->getVertices()[0]->Position);
		(*outSections)[section.x][section.y].WorldCoordinates = section;

		Vector3 & bbmin = (*outSections)[section.x][section.y].BoundingBox.Min;
		Vector3 & bbmax = (*outSections)[section.x][section.y].BoundingBox.Max;

		DWORD sectionColor = float4((section.x % 2) + 0.5f, (section.x % 2) + 0.5f, 1, 1).ToDWORD();

		if (poly->GetNumPolyVertices() < 3)
		{
			LogWarn() << "Poly with less than 3 vertices!";
		}

		// Extract poly vertices
		std::vector<ExVertexStruct> polyVertices;
		for(int v=0;v<poly->GetNumPolyVertices();v++)
		{
			ExVertexStruct t;
			t.Position = poly->getVertices()[v]->Position;
			t.TexCoord = poly->getFeatures()[v]->texCoord;
			t.Normal = poly->getFeatures()[v]->normal;
			t.Color = poly->getFeatures()[v]->lightStatic;

			// Check bounding box
			bbmin.x = bbmin.x > poly->getVertices()[v]->Position.x ? poly->getVertices()[v]->Position.x : bbmin.x;
			bbmin.y = bbmin.y > poly->getVertices()[v]->Position.y ? poly->getVertices()[v]->Position.y : bbmin.y;
			bbmin.z = bbmin.z > poly->getVertices()[v]->Position.z ? poly->getVertices()[v]->Position.z : bbmin.z;
																						 
			bbmax.x = bbmax.x < poly->getVertices()[v]->Position.x ? poly->getVertices()[v]->Position.x : bbmax.x;
			bbmax.y = bbmax.y < poly->getVertices()[v]->Position.y ? poly->getVertices()[v]->Position.y : bbmax.y;
			bbmax.z = bbmax.z < poly->getVertices()[v]->Position.z ? poly->getVertices()[v]->Position.z : bbmax.z;

			if (poly->GetLightmap())
			{
				t.TexCoord2 = poly->GetLightmap()->GetLightmapUV(t.Position);
				t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
			} else
			{
				t.Color = 0xFFFFFFFF;
				t.TexCoord2.x = 0.0f;
				t.TexCoord2.y = 0.0f;

				if (poly->GetMaterial() && poly->GetMaterial()->GetMatGroup() == zMAT_GROUP_WATER)
				{
					t.Normal = Vector3(0, 1, 0); // Get rid of ugly shadows on water
				}
			}

			polyVertices.push_back(t);
		}		

		// Use the map to put the polygon to those using the same material

		zCMaterial * mat = poly->GetMaterial();
		MeshKey key;
		key.Texture = mat != nullptr ? mat->GetTexture() : nullptr;
		key.Material = mat;
		
		//key.Lightmap = poly->GetLightmap();

		if ((*outSections)[section.x][section.y].WorldMeshes.count(key) == 0)
		{
			key.Info = Engine::GAPI->GetMaterialInfoFrom(key.Texture);
			(*outSections)[section.x][section.y].WorldMeshes[key] = new WorldMeshInfo;
		}

		//std::vector<ExVertexStruct> TriangleVertices;
		std::vector<ExVertexStruct> finalVertices;
		TriangleFanToList(&polyVertices[0], polyVertices.size(), &finalVertices);

		//if (mat && mat->GetTexture())
		//	LogInfo() << "Got texture name: " << mat->GetTexture()->GetName();

		if (poly->GetMaterial() && poly->GetMaterial()->GetMatGroup() == zMAT_GROUP_WATER)
		{
			// Give water surfaces a water-shader
			MaterialInfo* polyInfo = Engine::GAPI->GetMaterialInfoFrom(poly->GetMaterial()->GetTexture());
			if (polyInfo)
			{
				polyInfo->PixelShader = "PS_Water";
				polyInfo->MaterialType = MaterialInfo::MT_Water;
			}
		}

		for(unsigned int v=0;v<finalVertices.size();v++)
			(*outSections)[section.x][section.y].WorldMeshes[key]->Vertices.push_back(finalVertices[v]);
	}
	
	Vector2 avgSections = Vector2(0, 0);
	int numSections = 0;

	std::list<std::vector<ExVertexStruct>*> vertexBuffers;
	std::list<std::vector<VERTEX_INDEX>*> indexBuffers;

	// Create the vertexbuffers for every material
	for(auto const& itx : *outSections)
	{
		for(auto const& ity : itx.second)
		{
			numSections++;
			avgSections += Vector2((float)itx.first, (float)ity.first);

			for(auto const& it : ity.second.WorldMeshes)
			{
				std::vector<ExVertexStruct> indexedVertices;
				std::vector<VERTEX_INDEX> indices;
				IndexVertices(&it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices);

				// Generate normals
				GenerateVertexNormals(it.second->Vertices, it.second->Indices);

				std::vector<VERTEX_INDEX> indicesPNAEN; // Use PNAEN to detect the borders of the mesh
				MeshModifier::ComputePNAEN18Indices(indexedVertices, indices, indicesPNAEN);

				it.second->Vertices = indexedVertices;
				it.second->Indices = indicesPNAEN;

				// Create the buffers
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshVertexBuffer);
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshIndexBuffer);

				// Init and fill them
				it.second->MeshVertexBuffer->Init(&it.second->Vertices[0], it.second->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
				it.second->MeshIndexBuffer->Init(&it.second->Indices[0], it.second->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

				// Remember them, to wrap then up later
				vertexBuffers.push_back(&it.second->Vertices);
				indexBuffers.push_back(&it.second->Indices);
			}
		}
	}

	std::vector<ExVertexStruct> wrappedVertices;
	std::vector<unsigned int> wrappedIndicesSimple;
	std::vector<unsigned int> wrappedIndicesPNAEN;
	std::vector<unsigned int> offsets;

	// Calculate fat vertexbuffer
	WorldConverter::WrapVertexBuffers(vertexBuffers, indexBuffers, wrappedVertices, wrappedIndicesSimple, offsets);

	for(unsigned int i=0;i<offsets.size();i++)
	{
		offsets[i] *= 6;
	}

	// Run PNAEN on that
	MeshModifier::ComputePNAEN18Indices(wrappedVertices, wrappedIndicesSimple, wrappedIndicesPNAEN, false);

	// Generate smooth normals
	//MeshModifier::ComputeSmoothNormals(wrappedVertices);

	// Propergate the offsets
	int i=0;
	for(auto const& itx : *outSections)
	{
		for(auto const& ity : itx.second)
		{
			for(auto const& it : ity.second.WorldMeshes)
			{
				MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom(it.first.Texture);
				info->TesselationShaderPair = "PNAEN_Tesselation";

				it.second->BaseIndexLocation = offsets[i];

				i++;
			}
		}
	}

	// Create the buffers for wrapped mesh
	MeshInfo * wmi = new MeshInfo;
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshVertexBuffer);
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshIndexBuffer);
	
	// Init and fill them
	wmi->MeshVertexBuffer->Init(&wrappedVertices[0], wrappedVertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	wmi->MeshIndexBuffer->Init(&wrappedIndicesPNAEN[0], wrappedIndicesPNAEN.size() * sizeof(unsigned int), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	*outWrappedMesh = wmi;

	// Calculate the approx midpoint of the world
	avgSections /= (float)numSections;

	if (info)
	{
		info->MidPoint = avgSections * WORLD_SECTION_SIZE;
		info->LowestVertex = 0;
		info->HighestVertex = 0;	
	}

	return XR_SUCCESS;
}

/** Converts the worldmesh into a more usable format */
HRESULT WorldConverter::ConvertWorldMesh(zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo ** outWrappedMesh)
{
	// Go through every polygon and put it into its section
	for (unsigned int i = 0; i < numPolygons; i++) {
		zCPolygon * poly = polys[i];

		// Check if we even need this polygon
		if (poly->GetPolyFlags()->GhostOccluder || poly->GetPolyFlags()->PortalPoly) {
			continue;
		}

		//if (polygons[i]->GetNumPolyVertices() != 3)
		//	continue;

		// Calculate midpoint of this triange to get the section
		Vector3 avgPos = (poly->getVertices()[0]->Position + poly->getVertices()[1]->Position + poly->getVertices()[2]->Position) / 3.0f;
		INT2 section = GetSectionOfPos(avgPos);
		(*outSections)[section.x][section.y].WorldCoordinates = section;

		if (poly->GetMaterial() && poly->GetMaterial()->GetMatGroup() == zMAT_GROUP_WATER) {
			//(*outSections)[section.x][section.y].OceanPoints.push_back(*poly->getVertices()[0]->Position);
			//continue;
		}

		Vector3 & bbmin = (*outSections)[section.x][section.y].BoundingBox.Min;
		Vector3 & bbmax = (*outSections)[section.x][section.y].BoundingBox.Max;

		DWORD sectionColor = float4((section.x % 2) + 0.5f, (section.x % 2) + 0.5f, 1, 1).ToDWORD();

		if (poly->GetNumPolyVertices() < 3) {
			LogWarn() << "Poly with less than 3 vertices!";
		}

		// Extract poly vertices
		std::vector<ExVertexStruct> polyVertices;
		for (int v = 0; v < poly->GetNumPolyVertices(); v++) {
			ExVertexStruct t;
			t.Position = poly->getVertices()[v]->Position;
			t.TexCoord = poly->getFeatures()[v]->texCoord;
			t.Normal = poly->getFeatures()[v]->normal;
			t.Color = poly->getFeatures()[v]->lightStatic;

			// Check bounding box
			bbmin.x = bbmin.x > poly->getVertices()[v]->Position.x ? poly->getVertices()[v]->Position.x : bbmin.x;
			bbmin.y = bbmin.y > poly->getVertices()[v]->Position.y ? poly->getVertices()[v]->Position.y : bbmin.y;
			bbmin.z = bbmin.z > poly->getVertices()[v]->Position.z ? poly->getVertices()[v]->Position.z : bbmin.z;
																						 
			bbmax.x = bbmax.x < poly->getVertices()[v]->Position.x ? poly->getVertices()[v]->Position.x : bbmax.x;
			bbmax.y = bbmax.y < poly->getVertices()[v]->Position.y ? poly->getVertices()[v]->Position.y : bbmax.y;
			bbmax.z = bbmax.z < poly->getVertices()[v]->Position.z ? poly->getVertices()[v]->Position.z : bbmax.z;

			if (poly->GetLightmap()) {
				t.TexCoord2 = poly->GetLightmap()->GetLightmapUV(t.Position);
				t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
			} else {
				t.TexCoord2.x = 0.0f;
				t.TexCoord2.y = 0.0f;

				if (poly->GetMaterial() && poly->GetMaterial()->GetMatGroup() == zMAT_GROUP_WATER) {
					t.Normal = Vector3(0, 1, 0); // Get rid of ugly shadows on water
				}
			}

			polyVertices.push_back(t);
		}		

		// Use the map to put the polygon to those using the same material

		zCMaterial * mat = poly->GetMaterial();
		MeshKey key;
		key.Texture = mat != nullptr ? mat->GetTexture() : nullptr;
		key.Material = mat;
		
		//key.Lightmap = poly->GetLightmap();

		if ((*outSections)[section.x][section.y].WorldMeshes.count(key) == 0) {
			key.Info = Engine::GAPI->GetMaterialInfoFrom(key.Texture);
			(*outSections)[section.x][section.y].WorldMeshes[key] = new WorldMeshInfo;
		}

		std::vector<ExVertexStruct> finalVertices;
		TriangleFanToList(&polyVertices[0], polyVertices.size(), &finalVertices);

		/*
		if (finalVertices.size() == 3) {
			if (mat && mat->GetTexture()) {
				if (strnicmp(mat->GetTexture()->GetNameWithoutExt().c_str(), "NW_Harbour_Stairs", strlen("NW_Harbour_Stairs")) == 0)
				{
					ExVertexStruct vx[3];
					memcpy(vx, &finalVertices[0], sizeof(ExVertexStruct) * 3);
					finalVertices.clear();

					TesselateTriangle(vx, finalVertices, 5);
				}
			}
		}
		*/

		//if (mat && mat->GetTexture())
		//	LogInfo() << "Got texture name: " << mat->GetTexture()->GetName();

		if (poly->GetMaterial() && poly->GetMaterial()->GetMatGroup() == zMAT_GROUP_WATER // Check for water
			&& poly->GetMaterial()->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST) // Fix foam on waterfalls
		{
			// Give water surfaces a water-shader
			MaterialInfo * info = Engine::GAPI->GetMaterialInfoFrom(poly->GetMaterial()->GetTexture());
			if (info) {
				info->PixelShader = "PS_Water";
				info->MaterialType = MaterialInfo::MT_Water;
			}
		}

		for (unsigned int v = 0; v < finalVertices.size(); v++) {
			(*outSections)[section.x][section.y].WorldMeshes[key]->Vertices.push_back(finalVertices[v]);
		}
	}
	
	Vector2 avgSections = Vector2(0, 0);
	int numSections = 0;

	std::list<std::vector<ExVertexStruct>*> vertexBuffers;
	std::list<std::vector<VERTEX_INDEX>*> indexBuffers;

	// Create the vertexbuffers for every material
	for (auto const& itx : *outSections) {
		for (auto const& ity : itx.second) {
			numSections++;
			avgSections += Vector2((float)itx.first, (float)ity.first);

			for (auto const& it : ity.second.WorldMeshes) {
				std::vector<ExVertexStruct> indexedVertices;
				std::vector<VERTEX_INDEX> indices;
				IndexVertices(&it.second->Vertices[0], it.second->Vertices.size(), indexedVertices, indices);

				it.second->Vertices = indexedVertices;
				it.second->Indices = indices;

				// Create the buffers
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshVertexBuffer);
				Engine::GraphicsEngine->CreateVertexBuffer(&it.second->MeshIndexBuffer);

				// Generate normals
				GenerateVertexNormals(it.second->Vertices, it.second->Indices);

				// Optimize faces
				it.second->MeshVertexBuffer->OptimizeFaces(&it.second->Indices[0],
					(byte *)&it.second->Vertices[0], 
					it.second->Indices.size(), 
					it.second->Vertices.size(), 
					sizeof(ExVertexStruct));

				// Then optimize vertices
				it.second->MeshVertexBuffer->OptimizeVertices(&it.second->Indices[0],
					(byte *)&it.second->Vertices[0], 
					it.second->Indices.size(), 
					it.second->Vertices.size(), 
					sizeof(ExVertexStruct));

				// Init and fill them
				it.second->MeshVertexBuffer->Init(&it.second->Vertices[0], it.second->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
				it.second->MeshIndexBuffer ->Init(&it.second->Indices[0], it.second->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);


				// Remember them, to wrap then up later
				vertexBuffers.push_back(&it.second->Vertices);
				indexBuffers.push_back(&it.second->Indices);
			}
		}
	}

	std::vector<ExVertexStruct> wrappedVertices;
	std::vector<unsigned int> wrappedIndices;
	std::vector<unsigned int> offsets;

	// Calculate fat vertexbuffer
	WorldConverter::WrapVertexBuffers(vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets);

	// Propergate the offsets
	int i = 0;
	for (auto const& itx : *outSections) {
		for(auto const& ity : itx.second) {
			for (auto const& it : ity.second.WorldMeshes) {
				it.second->BaseIndexLocation = offsets[i];

				i++;
			}
		}
	}

	// Create the buffers for wrapped mesh
	MeshInfo * wmi = new MeshInfo();
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshVertexBuffer);
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshIndexBuffer);
	
	LogInfo() << "Smoothing worldmesh normals...";
	DWORD sStart = Toolbox::timeSinceStartMs();

	// Generate smooth normals
	MeshModifier::ComputeSmoothNormals(wrappedVertices);

	LogInfo() << "Process took " << Toolbox::timeSinceStartMs() - sStart << "ms";

	// Init and fill them
	wmi->MeshVertexBuffer->Init(&wrappedVertices[0], wrappedVertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	wmi->MeshIndexBuffer->Init(&wrappedIndices[0], wrappedIndices.size() * sizeof(unsigned int), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	*outWrappedMesh = wmi;

	// Calculate the approx midpoint of the world
	avgSections /= (float)numSections;

	if (info) {
		/*WorldInfo i;
		i.MidPoint = avgSections * WORLD_SECTION_SIZE;
		i.LowestVertex = 0;
		i.HighestVertex = 0;

		memcpy(info, &i, sizeof(WorldInfo));*/

		info->MidPoint = avgSections * WORLD_SECTION_SIZE;
		info->LowestVertex = 0;
		info->HighestVertex = 0;	
	}
	//SaveSectionsToObjUnindexed("Test.obj", (*outSections));

	return XR_SUCCESS;
}

/** Creates the FullSectionMesh for the given section */
void WorldConverter::GenerateFullSectionMesh(WorldMeshSectionInfo& section)
{
	std::vector<ExVertexStruct> vx;
	for (auto const& it : section.WorldMeshes) {
		if (!it.first.Material ||
			it.first.Material->HasAlphaTest())
			continue;

		for(unsigned int i=0;i<it.second->Indices.size(); i+=3)
		{
			// Push all triangles
			vx.push_back(it.second->Vertices[it.second->Indices[i]]);
			vx.push_back(it.second->Vertices[it.second->Indices[i+1]]);
			vx.push_back(it.second->Vertices[it.second->Indices[i+2]]);
		}
	}

	// Get VOBs
	for(auto const& it : section.Vobs)
	{
		if (it->IsIndoorVob)
			continue;

		XMMATRIX world = it->Vob->GetWorldMatrixXM();
		world = XMMatrixTranspose(world);

		// Insert the vob
		for(auto const& itm : it->VisualInfo->Meshes)
		{
			if (!itm.first ||
				itm.first->HasAlphaTest())
				continue;

			for(unsigned int m=0;m<itm.second.size();m++)
			{
				for(unsigned int i=0;i<itm.second[m]->Indices.size(); i++)
				{
					ExVertexStruct v = itm.second[m]->Vertices[itm.second[m]->Indices[i]];

					// Transform everything into world space
					v.Position = XMVector3TransformCoord(v.Position, world);
					vx.push_back(v);
				}
			}
		}
	}

	// Catch empty section
	if (vx.empty())
		return;

	// Index the mesh
	std::vector<ExVertexStruct> indexedVertices;
	std::vector<VERTEX_INDEX> indices;

	section.FullStaticMesh = new MeshInfo;
	section.FullStaticMesh->Vertices = vx;

	// Create the buffers
	Engine::GraphicsEngine->CreateVertexBuffer(&section.FullStaticMesh->MeshVertexBuffer);

	// Init and fill them
	section.FullStaticMesh->MeshVertexBuffer->Init(&section.FullStaticMesh->Vertices[0], section.FullStaticMesh->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	Engine::GAPI->GetRendererState()->RendererInfo.SkeletalVerticesDataSize += section.FullStaticMesh->Vertices.size() * sizeof(ExVertexStruct);
}

/** Returns what section the given position is in */
INT2 WorldConverter::GetSectionOfPos(const float3 & pos)
{
	// Find out where it belongs
	int px = (int)((pos.x / WORLD_SECTION_SIZE) + 0.5f);
	int py = (int)((pos.z / WORLD_SECTION_SIZE) + 0.5f);

	// Fix the centerpiece
	/*if (pos.x < 0)
		px-=1;

	if (pos.z < 0)
		py-=1;*/

	return INT2(px, py);
}

/** Converts a triangle fan to a list */
void WorldConverter::TriangleFanToList(ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>* outVertices)
{
	for(UINT i=1;i<numInputVertices-1;i++)
	{
		outVertices->push_back(input[0]);
		outVertices->push_back(input[i+1]);
		outVertices->push_back(input[i]);
	}
}

/** Saves the given section-array to an obj file */
void WorldConverter::SaveSectionsToObjUnindexed(const char* file, const std::map<int, std::map<int, WorldMeshSectionInfo>> & sections)
{
	FILE* f = fopen(file, "w");

	if (!f)
	{
		LogError() << "Failed to open file " << file << " for writing!";
		return;
	}

	fputs("o World\n", f);

	for(auto const& itx : sections)
	{
		for(auto const& ity : itx.second)
		{
			for(auto const& it : ity.second.WorldMeshes)
			{
				for(auto const& vtx : it.second->Vertices)
				{
					std::string ln = "v " + std::to_string(vtx.Position.x) + " " + std::to_string(vtx.Position.y) + " " + std::to_string(vtx.Position.z) + "\n";
					fputs(ln.c_str(), f);
				}
			}
		}
	}

	fclose(f);
}

/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual(zCProgMeshProto* visual, MeshVisualInfo* meshInfo)
{
	std::vector<ExVertexStruct> vertices;

	// Get the data out for all submeshes
	vertices.clear();
	visual->ConstructVertexBuffer(&vertices);

	// This visual can hold multiple submeshes, each with it's own indices
	for(int i=0;i<visual->GetNumSubmeshes();i++)
	{
		zCSubMesh* m = visual->GetSubmesh(i);

		// Get the data from the indices
		for(int n=0; n<m->WedgeList.NumInArray;n++)
		{
			int idx = m->WedgeList.Get(n).position;

			vertices[idx].TexCoord.x = m->WedgeList.Get(n).texUV.x; // This produces wrong results
			vertices[idx].TexCoord.y = m->WedgeList.Get(n).texUV.y;
			vertices[idx].Color = 0xFFFFFFFF;
			vertices[idx].Normal = m->WedgeList.Get(n).normal;
		}

		// Get indices
		std::vector<VERTEX_INDEX> indices;
		for(int n=0;n<visual->GetSubmesh(i)->TriList.NumInArray;n++)
		{
			indices.push_back(visual->GetSubmesh(i)->WedgeList.Get(visual->GetSubmesh(i)->TriList.Get(n).wedge[0]).position);
			indices.push_back(visual->GetSubmesh(i)->WedgeList.Get(visual->GetSubmesh(i)->TriList.Get(n).wedge[1]).position);
			indices.push_back(visual->GetSubmesh(i)->WedgeList.Get(visual->GetSubmesh(i)->TriList.Get(n).wedge[2]).position);
		}
		
		zCMaterial * mat = visual->GetSubmesh(i)->Material;

		MeshInfo * mi = new MeshInfo;

		mi->Vertices = vertices;
		mi->Indices = indices;
		
		// Create the buffers
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshVertexBuffer);
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshIndexBuffer);
	
		// Init and fill it
		mi->MeshVertexBuffer->Init(&vertices[0], vertices.size() * sizeof(ExVertexStruct));
		mi->MeshIndexBuffer->Init(&indices[0], indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER);

		meshInfo->Meshes[mat].push_back(mi);
	}

	meshInfo->Visual = visual;
}

/** Extracts a skeletal mesh from a zCMeshSoftSkin */
void WorldConverter::ExtractSkeletalMeshFromVob(zCModel * model, SkeletalMeshVisualInfo * skeletalMeshInfo) {
	// This type has multiple skinned meshes inside
	for (int i = 0; i < model->GetMeshSoftSkinList()->NumInArray; i++) {
		zCMeshSoftSkin * s = model->GetMeshSoftSkinList()->Array[i];
		std::vector<ExSkelVertexStruct> posList;

		// This stream is built as the following:
		// 4byte int - Num of nodes
		// sizeof(zTWeightEntry) - The entry
		char * stream = s->GetVertWeightStream();

		// Get bone weights for each vertex
		for (int i = 0; i < s->GetPositionList()->NumInArray; i++) {
			// Get num of nodes
			int numNodes = *(int *)stream;
			stream += 4;

			ExSkelVertexStruct vx;
			//vx.Position = s->GetPositionList()->Array[i];
			vx.Color = 0xFFFFFFFF;
			vx.Normal = Vector3(0, 0, 0);
			ZeroMemory(vx.weights, sizeof(vx.weights));
			ZeroMemory(vx.Position, sizeof(vx.Position));
			ZeroMemory(vx.boneIndices, sizeof(vx.boneIndices));

			for (int n = 0; n < numNodes; n++) {
				// Get entry
				zTWeightEntry weightEntry = *(zTWeightEntry *)stream;
				stream += sizeof(zTWeightEntry);

				//if (s->GetNormalsList() && i < s->GetNormalsList()->NumInArray)
				//	(*vx.Normal.toD3DXVECTOR3()) += weightEntry.Weight * (*s->GetNormalsList()->Array[i].toD3DXVECTOR3());

				// Get index and weight
				if (n < 4) {
					vx.weights[n] = weightEntry.Weight;
					vx.boneIndices[n] = weightEntry.NodeIndex;
					vx.Position[n] = weightEntry.VertexPosition;
				}
			}

			posList.push_back(vx);
		}

		// The rest is the same as a zCProgMeshProto, but with a different vertex type
		for (int i = 0; i < s->GetNumSubmeshes(); i++) {
			std::vector<ExSkelVertexStruct> vertices;
			std::vector<ExVertexStruct> bindPoseVertices;
			std::vector<VERTEX_INDEX> indices;
			// Get the data out
			zCSubMesh* m = s->GetSubmesh(i);

			// Get indices
			for (int t = 0; t < m->TriList.NumInArray; t++) {				
				for (int v = 0; v < 3; v++) {
					indices.push_back(m->TriList.Array[t].wedge[v]);
				}
			}

			// Get vertices
			for (int v = 0; v < m->WedgeList.NumInArray; v++) {
				ExSkelVertexStruct vx;
				vx = posList[m->WedgeList.Array[v].position];
				vx.TexCoord	= m->WedgeList.Array[v].texUV;
				vx.Color = 0xFFFFFFFF;
				vx.Normal = m->WedgeList.Array[v].normal;

				vertices.push_back(vx);

				// Save vertexpos in bind pose, to run PNAEN on it
				ExVertexStruct pvx;
				pvx.Position = s->GetPositionList()->Array[m->WedgeList.Array[v].position];
				pvx.TexCoord = vx.TexCoord;
				pvx.Normal = vx.Normal;
				pvx.Color = vx.Color;

				bindPoseVertices.push_back(pvx);
			}
		
			zCMaterial * mat = s->GetSubmesh(i)->Material;

			SkeletalMeshInfo * mi = new SkeletalMeshInfo;
			mi->Vertices = vertices;
			mi->Indices = indices;
			mi->visual = s;

			// Create the buffers
			Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshVertexBuffer);
			Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshIndexBuffer);

			// Init and fill it
			mi->MeshVertexBuffer->Init(&mi->Vertices[0], mi->Vertices.size() * sizeof(ExSkelVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
			mi->MeshIndexBuffer->Init(&mi->Indices[0], mi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

			MeshInfo * bmi = new MeshInfo;
			bmi->Indices = indices;
			bmi->Vertices = bindPoseVertices;
			
			Engine::GraphicsEngine->CreateVertexBuffer(&bmi->MeshVertexBuffer);
			Engine::GraphicsEngine->CreateVertexBuffer(&bmi->MeshIndexBuffer);

			bmi->MeshVertexBuffer->Init(&bmi->Vertices[0], bmi->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
			bmi->MeshIndexBuffer->Init(&bmi->Indices[0], bmi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

			Engine::GAPI->GetRendererState()->RendererInfo.SkeletalVerticesDataSize += mi->Vertices.size() * sizeof(ExVertexStruct);
			Engine::GAPI->GetRendererState()->RendererInfo.SkeletalVerticesDataSize += mi->Indices.size() * sizeof(VERTEX_INDEX);

			skeletalMeshInfo->SkeletalMeshes[mat].push_back(mi);
			skeletalMeshInfo->Meshes[mat].push_back(bmi);
		}
	}

	static int s_NoMeshesNum = 0;

	skeletalMeshInfo->VisualName = model->GetVisualName();
	// Try to load saved settings for this mesh
	skeletalMeshInfo->LoadMeshVisualInfo(skeletalMeshInfo->VisualName);

	// Create additional information
	if (skeletalMeshInfo->TesselationInfo.buffer.VT_TesselationFactor > 0.0f
		&& Engine::GAPI->GetRendererState()->RendererSettings.AllowWorldMeshTesselation) // TODO: PNAEN for skeletals causes huge lags in the game and is barely
																						 // noticable anyways. Disable for now.
		skeletalMeshInfo->CreatePNAENInfo(skeletalMeshInfo->TesselationInfo.buffer.VT_DisplacementStrength > 0.0f);
}

/** Extracts a skeletal mesh from a zCMeshSoftSkin */
void WorldConverter::ExtractSkeletalMeshFromProto(zCModelMeshLib* model, SkeletalMeshVisualInfo * skeletalMeshInfo)
{
	// This type has multiple skinned meshes inside
	for(int i=0;i<model->GetMeshSoftSkinList()->NumInArray;i++)
	{
		zCMeshSoftSkin * s = model->GetMeshSoftSkinList()->Array[i];
		std::vector<ExSkelVertexStruct> posList;

		// This stream is built as the following:
		// 4byte int - Num of nodes
		// sizeof(zTWeightEntry) - The entry
		char* stream = s->GetVertWeightStream();

		// Get bone weights for each vertex
		for(int i=0; i<s->GetPositionList()->NumInArray;i++)
		{
			// Get num of nodes
			int numNodes = *(int *)stream;
			stream += 4;

			ExSkelVertexStruct vx;
			//vx.Position = s->GetPositionList()->Array[i];
			vx.Color = 0xFFFFFFFF;
			vx.Normal = Vector3(0, 0, 0);
			ZeroMemory(vx.weights, sizeof(vx.weights));
			ZeroMemory(vx.Position, sizeof(vx.Position));
			ZeroMemory(vx.boneIndices, sizeof(vx.boneIndices));

			for(int n=0;n<numNodes;n++)
			{
				// Get entry
				zTWeightEntry weightEntry = *(zTWeightEntry *)stream;
				stream += sizeof(zTWeightEntry);

				// Get index and weight
				if (n < 4)
				{
					vx.weights[n] = weightEntry.Weight;
					vx.boneIndices[n] = weightEntry.NodeIndex;
					vx.Position[n] = weightEntry.VertexPosition;
				}
			}

			posList.push_back(vx);
		}

		

		// The rest is the same as a zCProgMeshProto, but with a different vertex type
		for(int i=0;i<s->GetNumSubmeshes();i++)
		{
			std::vector<ExSkelVertexStruct> vertices;
			std::vector<ExVertexStruct> bindPoseVertices;
			std::vector<VERTEX_INDEX> indices;
			// Get the data out
			zCSubMesh* m = s->GetSubmesh(i);

			// Get indices
			for(int t=0;t<m->TriList.NumInArray;t++)
			{				
				for(int v=2;v>=0;v--)
				{
					indices.push_back(m->TriList.Array[t].wedge[v]);
				}
			}

			// Get vertices
			for(int v=0;v<m->WedgeList.NumInArray;v++)
			{
				ExSkelVertexStruct vx;
				vx = posList[m->WedgeList.Array[v].position];
				vx.TexCoord	= m->WedgeList.Array[v].texUV;
				vx.Color = 0xFFFFFFFF;
				vx.Normal = m->WedgeList.Array[v].normal;

				vertices.push_back(vx);

				// Save vertexpos in bind pose, to run PNAEN on it
				ExVertexStruct pvx;
				pvx.Position = s->GetPositionList()->Array[m->WedgeList.Array[v].position];
				pvx.TexCoord = vx.TexCoord;
				pvx.Normal = vx.Normal;
				pvx.Color = vx.Color;

				bindPoseVertices.push_back(pvx);
			}
		
			zCMaterial * mat = s->GetSubmesh(i)->Material;

			SkeletalMeshInfo * mi = new SkeletalMeshInfo;
			mi->Vertices = vertices;
			mi->Indices = indices;
			mi->visual = s;

			// Create the buffers
			Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshVertexBuffer);
			Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshIndexBuffer);

			// Init and fill it
			mi->MeshVertexBuffer->Init(&mi->Vertices[0], mi->Vertices.size() * sizeof(ExSkelVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
			mi->MeshIndexBuffer->Init(&mi->Indices[0], mi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);


			MeshInfo * bmi = new MeshInfo;
			bmi->Indices = indices;
			bmi->Vertices = bindPoseVertices;
			
			Engine::GraphicsEngine->CreateVertexBuffer(&bmi->MeshVertexBuffer);
			Engine::GraphicsEngine->CreateVertexBuffer(&bmi->MeshIndexBuffer);

			bmi->MeshVertexBuffer->Init(&bmi->Vertices[0], bmi->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
			bmi->MeshIndexBuffer->Init(&bmi->Indices[0], bmi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

			

			Engine::GAPI->GetRendererState()->RendererInfo.SkeletalVerticesDataSize += mi->Vertices.size() * sizeof(ExVertexStruct);
			Engine::GAPI->GetRendererState()->RendererInfo.SkeletalVerticesDataSize += mi->Indices.size() * sizeof(VERTEX_INDEX);

			skeletalMeshInfo->SkeletalMeshes[mat].push_back(mi);
			skeletalMeshInfo->Meshes[mat].push_back(bmi);
		}
	}

	static int s_NoMeshesNum = 0;

	skeletalMeshInfo->VisualName = model->GetVisualName();

	// Try to load saved settings for this mesh
	skeletalMeshInfo->LoadMeshVisualInfo(skeletalMeshInfo->VisualName);

	// Create additional information
	if (skeletalMeshInfo->TesselationInfo.buffer.VT_TesselationFactor > 0.0f)
		skeletalMeshInfo->CreatePNAENInfo(skeletalMeshInfo->TesselationInfo.buffer.VT_DisplacementStrength > 0.0f);
}

/** Extracts a node-visual */
void WorldConverter::ExtractNodeVisual(int index, zCModelNodeInst* node, std::map<int, std::vector<MeshVisualInfo *>> & attachments)
{
	// Only allow 1 attachment
	if (!attachments[index].empty())
	{
		delete attachments[index][0];
		attachments[index].clear();
	}

	// Extract node visuals
	if (node->NodeVisual)
	{
		const char* ext = node->NodeVisual->GetFileExtension(0);

		if (strcmp(ext, ".3DS") == 0)
		{
			zCProgMeshProto* pm = (zCProgMeshProto *)node->NodeVisual;

			if (pm->GetNumSubmeshes() == 0)
			{
				return;
			}

			MeshVisualInfo* mi = new MeshVisualInfo;

			Extract3DSMeshFromVisual2(pm, mi);

			

			attachments[index].push_back(mi);

		} else if (strcmp(ext, ".MMS") == 0)
		{
			// These are zCMorphMeshes
			zCProgMeshProto* pm = ((zCMorphMesh *)node->NodeVisual)->GetMorphMesh();

			if (pm->GetNumSubmeshes() == 0)
			{
				return;
			}

			MeshVisualInfo* mi = new MeshVisualInfo;

			Extract3DSMeshFromVisual2(pm, mi);
			mi->Visual = node->NodeVisual;

			attachments[index].push_back(mi);
		}
	}
}

/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual2PNAEN(zCProgMeshProto* visual, MeshVisualInfo* meshInfo)
{
	Vector3 tri0, tri1, tri2;
	Vector2 uv0, uv1, uv2;
	Vector3 bbmin, bbmax;
	bbmin = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
	bbmax = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	Vector3 * posList = visual->GetPositionList()->Array;

	std::list<std::vector<ExVertexStruct>*> vertexBuffers;
	std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
	std::list<MeshInfo *> meshInfos;

	// Construct unindexed mesh
	for(int i=0; i < visual->GetNumSubmeshes(); i++)
	{
		std::vector<ExVertexStruct> vertices;
		std::vector<VERTEX_INDEX> indices;

		// Get vertices
		for(int t=0;t<visual->GetSubmeshes()[i].TriList.NumInArray;t++)
		{				
			for(int v=0;v<3;v++)
			{
				indices.push_back(visual->GetSubmeshes()[i].TriList.Array[t].wedge[v]);
			}
		}

		for(int v=0;v<visual->GetSubmeshes()[i].WedgeList.NumInArray;v++)
		{
				ExVertexStruct vx;
				vx.Position = posList[visual->GetSubmeshes()[i].WedgeList.Array[v].position];
				vx.TexCoord	= visual->GetSubmeshes()[i].WedgeList.Array[v].texUV;
				vx.Color = 0xFFFFFFFF;
				vx.Normal = visual->GetSubmeshes()[i].WedgeList.Array[v].normal;

				vertices.push_back(vx);

				// Check bounding box
				bbmin.x = bbmin.x > vx.Position.x ? vx.Position.x : bbmin.x;
				bbmin.y = bbmin.y > vx.Position.y ? vx.Position.y : bbmin.y;
				bbmin.z = bbmin.z > vx.Position.z ? vx.Position.z : bbmin.z;

				bbmax.x = bbmax.x < vx.Position.x ? vx.Position.x : bbmax.x;
				bbmax.y = bbmax.y < vx.Position.y ? vx.Position.y : bbmax.y;
				bbmax.z = bbmax.z < vx.Position.z ? vx.Position.z : bbmax.z;
		}

		// Create the buffers and sort the mesh into the structure
		MeshInfo * mi = new MeshInfo;

		// Create the indexed mesh
		std::vector<ExVertexStruct> ixVertices;
		std::vector<unsigned short> ixIndices;

		if (vertices.empty())
			return;

		// ** PNAEN **
		MeshModifier::ComputePNAEN18Indices(vertices, indices, mi->Indices);
		mi->Vertices = vertices;
		

		// Create the buffers
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshVertexBuffer);
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshIndexBuffer);
	
		// Init and fill it
		mi->MeshVertexBuffer->Init(&mi->Vertices[0], mi->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
		mi->MeshIndexBuffer->Init(&mi->Indices[0], mi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

		Engine::GAPI->GetRendererState()->RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof(ExVertexStruct);
		Engine::GAPI->GetRendererState()->RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof(VERTEX_INDEX);

		zCMaterial * mat = visual->GetSubmesh(i)->Material;
		meshInfo->Meshes[mat].push_back(mi);

		MeshKey key;
		key.Material = mat;
		key.Texture = mat->GetTexture();
		key.Info = Engine::GAPI->GetMaterialInfoFrom(key.Texture);

		// ** PNAEN **
		key.Info->TesselationShaderPair = "PNAEN_Tesselation";

		meshInfo->MeshesByTexture[key].push_back(mi);

		vertexBuffers.push_back(&mi->Vertices);
		indexBuffers.push_back(&mi->Indices);
		meshInfos.push_back(mi);
	}

	std::vector<ExVertexStruct> wrappedVertices;
	std::vector<unsigned int> wrappedIndices;
	std::vector<unsigned int> wrappedIndicesPNAEN;
	std::vector<unsigned int> offsets;

	// Calculate fat vertexbuffer
	WorldConverter::WrapVertexBuffers(vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets);

	MeshModifier::ComputePNAEN18Indices(wrappedVertices, wrappedIndices, wrappedIndicesPNAEN);

	// Propergate the offsets
	int i=0;
	for(auto const& it : meshInfos)
	{	
		it->BaseIndexLocation = offsets[i] * 6;

		i++;
	}

	MeshInfo * wmi = new MeshInfo;
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshVertexBuffer);
	Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshIndexBuffer);
	


	// Init and fill them
	wmi->MeshVertexBuffer->Init(&wrappedVertices[0], wrappedVertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	wmi->MeshIndexBuffer->Init(&wrappedIndicesPNAEN[0], wrappedIndicesPNAEN.size() * sizeof(unsigned int), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	meshInfo->FullMesh = wmi;



	meshInfo->BBox.Min = bbmin;
	meshInfo->BBox.Max = bbmax;
	meshInfo->MeshSize = (bbmin - bbmax).Length();
	meshInfo->MidPoint = 0.5f * bbmin + 0.5f * bbmax;

	meshInfo->Visual = visual;
}



/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual2(zCProgMeshProto* visual, MeshVisualInfo* meshInfo)
{
	Vector3 tri0, tri1, tri2;
	Vector2 uv0, uv1, uv2;
	Vector3 bbmin, bbmax;
	bbmin = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
	bbmax = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

	Vector3 * posList = visual->GetPositionList()->Array;

	std::list<std::vector<ExVertexStruct>*> vertexBuffers;
	std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
	std::list<MeshInfo *> meshInfos;

	// Construct unindexed mesh
	for(int i=0; i < visual->GetNumSubmeshes(); i++)
	{
		std::vector<ExVertexStruct> vertices;
		std::vector<VERTEX_INDEX> indices;

		// Get vertices
		for(int t=0;t<visual->GetSubmeshes()[i].TriList.NumInArray;t++)
		{				
			for(int v=2;v>=0;v--)
			{
				indices.push_back(visual->GetSubmeshes()[i].TriList.Array[t].wedge[v]);
			}
		}

		for(int v=0;v<visual->GetSubmeshes()[i].WedgeList.NumInArray;v++)
		{
				ExVertexStruct vx;
				vx.Position = posList[visual->GetSubmeshes()[i].WedgeList.Array[v].position];
				vx.TexCoord	= visual->GetSubmeshes()[i].WedgeList.Array[v].texUV;

				//if (visual->GetSubmeshes()[i].Material)
				//	vx.Color = visual->GetSubmeshes()[i].Material->GetColor(); // Bake materialcolor into the mesh. This is ok for the most meshes.
				//else
					vx.Color = 0xFFFFFFFF;

				vx.Normal = visual->GetSubmeshes()[i].WedgeList.Array[v].normal;

				vertices.push_back(vx);

				// Check bounding box
				bbmin.x = bbmin.x > vx.Position.x ? vx.Position.x : bbmin.x;
				bbmin.y = bbmin.y > vx.Position.y ? vx.Position.y : bbmin.y;
				bbmin.z = bbmin.z > vx.Position.z ? vx.Position.z : bbmin.z;

				bbmax.x = bbmax.x < vx.Position.x ? vx.Position.x : bbmax.x;
				bbmax.y = bbmax.y < vx.Position.y ? vx.Position.y : bbmax.y;
				bbmax.z = bbmax.z < vx.Position.z ? vx.Position.z : bbmax.z;
		}

		// Create the buffers and sort the mesh into the structure
		MeshInfo * mi = new MeshInfo;

		// Create the indexed mesh
		if (vertices.empty())
		{
			LogWarn() << "Empty submesh (#" << i << ") on Visual " << visual->GetObjectName();
			continue;		
		}

		mi->Vertices = vertices;
		mi->Indices = indices;

		// Create the buffers
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshVertexBuffer);
		Engine::GraphicsEngine->CreateVertexBuffer(&mi->MeshIndexBuffer);
	
		// Optimize faces
		mi->MeshVertexBuffer->OptimizeFaces(&mi->Indices[0],
			(byte *)&mi->Vertices[0], 
			mi->Indices.size(), 
			mi->Vertices.size(), 
			sizeof(ExVertexStruct));

		// Then optimize vertices
		mi->MeshVertexBuffer->OptimizeVertices(&mi->Indices[0],
			(byte *)&mi->Vertices[0], 
			mi->Indices.size(), 
			mi->Vertices.size(), 
			sizeof(ExVertexStruct));

	
		// Init and fill it
		mi->MeshVertexBuffer->Init(&mi->Vertices[0], mi->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
		mi->MeshIndexBuffer->Init(&mi->Indices[0], mi->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

		Engine::GAPI->GetRendererState()->RendererInfo.VOBVerticesDataSize += mi->Vertices.size() * sizeof(ExVertexStruct);
		Engine::GAPI->GetRendererState()->RendererInfo.VOBVerticesDataSize += mi->Indices.size() * sizeof(VERTEX_INDEX);

		zCMaterial * mat = visual->GetSubmesh(i)->Material;
		meshInfo->Meshes[mat].push_back(mi);

		MeshKey key;
		key.Material = mat;
		key.Texture = mat->GetTexture();
		key.Info = Engine::GAPI->GetMaterialInfoFrom(key.Texture);

		meshInfo->MeshesByTexture[key].push_back(mi);

		vertexBuffers.push_back(&mi->Vertices);
		indexBuffers.push_back(&mi->Indices);
		meshInfos.push_back(mi);
	}

	std::vector<ExVertexStruct> wrappedVertices;
	std::vector<unsigned int> wrappedIndices;
	std::vector<unsigned int> offsets;

	if (visual->GetNumSubmeshes())
	{
		// Calculate fat vertexbuffer
		WorldConverter::WrapVertexBuffers(vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets);

		// Propergate the offsets
		int i = 0;
		for(auto const& it : meshInfos)
		{
			it->BaseIndexLocation = offsets[i];

			i++;
		}

		MeshInfo * wmi = new MeshInfo;
		Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshVertexBuffer);
		Engine::GraphicsEngine->CreateVertexBuffer(&wmi->MeshIndexBuffer);

		// Init and fill them
		wmi->MeshVertexBuffer->Init(&wrappedVertices[0], wrappedVertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
		wmi->MeshIndexBuffer->Init(&wrappedIndices[0], wrappedIndices.size() * sizeof(unsigned int), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

		meshInfo->FullMesh = wmi;
	}



	meshInfo->BBox.Min = bbmin;
	meshInfo->BBox.Max = bbmax;
	meshInfo->MeshSize = (bbmin - bbmax).Length();
	meshInfo->MidPoint = 0.5f * bbmin + 0.5f * bbmax;

	meshInfo->Visual = visual;
	meshInfo->VisualName = visual->GetObjectName();

	// Try to load saved settings for this mesh
	meshInfo->LoadMeshVisualInfo(meshInfo->VisualName);

	// Create additional information
	if (meshInfo->TesselationInfo.buffer.VT_TesselationFactor > 0.0f)
		meshInfo->CreatePNAENInfo(meshInfo->TesselationInfo.buffer.VT_DisplacementStrength > 0.0f);
}


const float eps = 0.001f;


struct CmpClass // class comparing vertices in the set
{
    bool operator() (const std::pair<ExVertexStruct, int> & p1, const std::pair<ExVertexStruct, int> & p2) const
    {
        if (fabs(p1.first.Position.x-p2.first.Position.x) > eps) return p1.first.Position.x < p2.first.Position.x;
        if (fabs(p1.first.Position.y-p2.first.Position.y) > eps) return p1.first.Position.y < p2.first.Position.y;
        if (fabs(p1.first.Position.z-p2.first.Position.z) > eps) return p1.first.Position.z < p2.first.Position.z;

		if (fabs(p1.first.TexCoord.x-p2.first.TexCoord.x) > eps) return p1.first.TexCoord.x < p2.first.TexCoord.x;
		if (fabs(p1.first.TexCoord.y-p2.first.TexCoord.y) > eps) return p1.first.TexCoord.y < p2.first.TexCoord.y;

        return false;
    }
};



/** Indexes the given vertex array */
void WorldConverter::IndexVertices(ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct> & outVertices, std::vector<VERTEX_INDEX> & outIndices)
{
	std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
	int index = 0;

    for (unsigned int i=0; i<numInputVertices; i++)
    {
        std::set<std::pair<ExVertexStruct, int>>::iterator it = vertices.find(std::make_pair(input[i], 0/*this value doesn't matter*/));
        if (it!=vertices.end()) outIndices.push_back(it->second);
        else
        {
            vertices.insert(std::make_pair(input[i], index));
            outIndices.push_back(index++);
        }
    }

	// TODO: Remove this and fix it properly!
	/*for (std::set<std::pair<ExVertexStruct, int>>::iterator it=vertices.begin(); it!=vertices.end(); it++)
	{
		if ((unsigned int)it->second >= vertices.size())
		{
			 // TODO: Investigate!
			it = vertices.erase(it);
			continue;
		}
	}

	for (std::vector<VERTEX_INDEX>::iterator it=outIndices.begin(); it!=outIndices.end(); it++)
	{
		if ((unsigned int)(*it) >= vertices.size())
		{
			 // TODO: Investigate!
			it = outIndices.erase(it);
			continue;
		}
	}*/

	// Check for overlaying triangles and throw them out
	// Some mods do that for the worldmesh for example
	std::set<std::tuple<VERTEX_INDEX,VERTEX_INDEX,VERTEX_INDEX>> triangles;
	for(size_t i=0;i<outIndices.size();i+=3)
	{
		// Insert every combination of indices here. Duplicates will be ignored
		triangles.insert(std::make_tuple(outIndices[i+0], outIndices[i+1], outIndices[i+2]));
	}

	// Extract the cleaned triangles to the indices vector
	outIndices.clear();
	for(auto const& it : triangles)
	{
		outIndices.push_back(std::get<0>(it));
		outIndices.push_back(std::get<1>(it));
		outIndices.push_back(std::get<2>(it));
	}

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
	outVertices.clear();
    outVertices.resize(vertices.size());
    for (auto const& it : vertices)
	{
		if ((unsigned int)it.second >= vertices.size())
		{
			continue;
		}

        outVertices[it.second] = it.first;
	}
}

void WorldConverter::IndexVertices(ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct> & outVertices, std::vector<unsigned int> & outIndices)
{
	std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
	unsigned int index = 0;

    for (unsigned int i=0; i<numInputVertices; i++)
    {
        std::set<std::pair<ExVertexStruct, int>>::iterator it = vertices.find(std::make_pair(input[i], 0/*this value doesn't matter*/));
        if (it!=vertices.end()) outIndices.push_back(it->second);
        else
        {
            vertices.insert(std::make_pair(input[i], index));
            outIndices.push_back(index++);
        }
    }

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
	outVertices.clear();
    outVertices.resize(vertices.size());
    for (auto const& it : vertices)
        outVertices[it.second] = it.first;
}

struct CmpClassSkel // class comparing vertices in the set
{
    bool operator() (const std::pair<ExSkelVertexStruct, int> & p1, const std::pair<ExSkelVertexStruct, int> & p2) const
    {
		for(int i=0;i<4;i++)
		{
			if (fabs(p1.first.Position[i].x-p2.first.Position[i].x) > eps) return p1.first.Position[i].x < p2.first.Position[i].x;
			if (fabs(p1.first.Position[i].y-p2.first.Position[i].y) > eps) return p1.first.Position[i].y < p2.first.Position[i].y;
			if (fabs(p1.first.Position[i].z-p2.first.Position[i].z) > eps) return p1.first.Position[i].z < p2.first.Position[i].z;
		}

		if (fabs(p1.first.TexCoord.x-p2.first.TexCoord.x) > eps) return p1.first.TexCoord.x < p2.first.TexCoord.x;
		if (fabs(p1.first.TexCoord.y-p2.first.TexCoord.y) > eps) return p1.first.TexCoord.y < p2.first.TexCoord.y;

        return false;
    }
};

void WorldConverter::IndexVertices(ExSkelVertexStruct* input, unsigned int numInputVertices, std::vector<ExSkelVertexStruct> & outVertices, std::vector<VERTEX_INDEX> & outIndices)
{
	std::set<std::pair<ExSkelVertexStruct, int>, CmpClassSkel> vertices;
	int index = 0;

    for (unsigned int i=0; i<numInputVertices; i++)
    {
        std::set<std::pair<ExSkelVertexStruct, int>>::iterator it = vertices.find(std::make_pair(input[i], 0/*this value doesn't matter*/));
        if (it!=vertices.end()) outIndices.push_back(it->second);
        else
        {
            vertices.insert(std::make_pair(input[i], index));
            outIndices.push_back(index++);
        }
    }

    // Notice that the vertices in the set are not sorted by the index
    // so you'll have to rearrange them like this:
    outVertices.resize(vertices.size());
    for (auto const& it : vertices)
        outVertices[it.second] = it.first;
}

/** Computes vertex normals for a mesh with face normals */
void WorldConverter::GenerateVertexNormals(std::vector<ExVertexStruct> & vertices, std::vector<VERTEX_INDEX> & indices)
{
	std::vector<Vector3> normals(vertices.size(), Vector3(0, 0, 0));

	for(unsigned int i=0;i<indices.size(); i+=3)
	{
		Vector3 v[3] = {vertices[indices[i]].Position, vertices[indices[i+1]].Position, vertices[indices[i+2]].Position};
		Vector3 normal = XMVector3Cross(v[1] - v[0], v[2] - v[0]);

		for (int j = 0; j < 3; ++j)
		{
			Vector3 a = v[(j+1) % 3] - v[j];
			Vector3 b = v[(j+2) % 3] - v[j];
			float weight = acos(a.Dot(b) / (a.Length() * b.Length()));
			normals[indices[(i+j)]] += weight * normal;
		}
	}

	// Normalize everything and store it into the vertices
	for(unsigned int i=0;i<normals.size(); i++)
	{
		normals[i].Normalize(vertices[i].Normal);
	}
}

ExVertexStruct TessTriLerpVertex(ExVertexStruct& a, ExVertexStruct& b, float w)
{
	ExVertexStruct v;
	v.Position = XMVectorLerp(a.Position, b.Position, w);
	v.Normal = XMVectorLerp(a.Normal, b.Normal, w);
	v.TexCoord = XMVectorLerp(a.TexCoord, b.TexCoord, w);
	v.TexCoord2 = XMVectorLerp(a.TexCoord2, b.TexCoord2, w);

	v.Color = a.Color;
	return v;
}

/** Outputs 4 new triangles for 1 input triangle */
static void TessSingleTri(ExVertexStruct* tri, std::vector<ExVertexStruct> & tesselated)
{
	ExVertexStruct half[3];
	half[0] = TessTriLerpVertex(tri[0], tri[1], 0.5f);
	half[1] = TessTriLerpVertex(tri[1], tri[2], 0.5f);
	half[2] = TessTriLerpVertex(tri[0], tri[2], 0.5f);

	tesselated.push_back(tri[0]);
	tesselated.push_back(half[0]);
	tesselated.push_back(half[2]);

	tesselated.push_back(half[0]);
	tesselated.push_back(tri[1]);
	tesselated.push_back(half[1]);

	tesselated.push_back(half[2]);
	tesselated.push_back(half[0]);
	tesselated.push_back(half[1]);

	tesselated.push_back(half[2]);
	tesselated.push_back(half[1]);
	tesselated.push_back(tri[2]);
}

/** Tesselates the given triangle and adds the values to the list */
void WorldConverter::TesselateTriangle(ExVertexStruct* tri, std::vector<ExVertexStruct> & tesselated, int amount)
{
	if (amount == 0)
	{
		tesselated.push_back(tri[0]);
		tesselated.push_back(tri[1]);
		tesselated.push_back(tri[2]);
		return;
	}

	std::vector<ExVertexStruct> tv;
	TessSingleTri(tri, tv);

	for(int i=0; i < 4 * 3; i+=3)
	{
		TesselateTriangle(&tv[i], tesselated, amount - 1);
	}
}

/** Marks the edges of the mesh */
void WorldConverter::MarkEdges(std::vector<ExVertexStruct> & vertices, std::vector<VERTEX_INDEX> & indices)
{

}

/** Builds a big vertexbuffer from the world sections */
void WorldConverter::WrapVertexBuffers(const std::list<std::vector<ExVertexStruct>*> & vertexBuffers, 
										const std::list<std::vector<VERTEX_INDEX>*> & indexBuffers, 
										std::vector<ExVertexStruct> & outVertices, 
										std::vector<unsigned int> & outIndices, 
										std::vector<unsigned int> & outOffsets)
{
	std::vector<unsigned int> vxOffsets;
	vxOffsets.push_back(0);

	// Pack vertices
	for(auto const& itv : vertexBuffers)
	{
		outVertices.insert(outVertices.end(), itv->begin(), itv->end());

		vxOffsets.push_back(vxOffsets.back() + itv->size());
	}

	// Pack indices
	outOffsets.push_back(0);
	int off = 0;
	for(auto const& iti : indexBuffers)
	{
		auto const& end = iti->end();
		for(auto& vi = iti->begin(); vi != end;++vi)
		{
			outIndices.push_back(*vi + vxOffsets[off]);
		}
		off++;

		outOffsets.push_back(outOffsets.back() + iti->size());
	}
}

/** Caches a mesh */
void WorldConverter::CacheMesh(const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry, const std::string & file) {
	FILE* f = fopen(file.c_str(), "wb");
	// Write version
	int Version = 1;
	fwrite(&Version, sizeof(Version), 1, f);

	// Write num textures
	size_t numTextures = geometry.size();
	fwrite(&numTextures, sizeof(numTextures), 1, f);

	for (auto const& it : geometry) {
		// Save texture name
		uint8_t numTxNameChars = static_cast<uint8_t>(it.first.size());
		fwrite(&numTxNameChars, sizeof(numTxNameChars), 1, f);
		fwrite(&it.first[0], numTxNameChars, 1, f);

		// Save num submeshes
		uint8_t numSubmeshes = static_cast<uint8_t>(it.second.size());
		fwrite(&numSubmeshes, sizeof(numSubmeshes), 1, f);

		for (uint8_t i = 0; i < numSubmeshes; i++) {
			// Save vertices
			size_t numVertices = it.second[i].first.size();
			fwrite(&numVertices, sizeof(numVertices), 1, f);
			fwrite(&it.second[i].first[0], sizeof(ExVertexStruct) * it.second[i].first.size(), 1, f);

			// Save indices
			size_t numIndices = it.second[i].second.size();
			fwrite(&numIndices, sizeof(numIndices), 1, f);
			fwrite(&it.second[i].second[0], sizeof(VERTEX_INDEX) * it.second[i].second.size(), 1, f);
		}
	}

	fclose(f);
}

/** Updates a quadmark info */
void WorldConverter::UpdateQuadMarkInfo(QuadMarkInfo* info, zCQuadMark* mark, const float3 & position)
{
	zCMesh* mesh = mark->GetQuadMesh();

	zCPolygon** polys = mesh->GetPolygons();
	int numPolys = mesh->GetNumPolygons();

	std::vector<ExVertexStruct> quadVertices;
	for(int i=0;i<numPolys;i++)
	{
		zCPolygon* poly = polys[i];

		// Extract poly vertices
		std::vector<ExVertexStruct> polyVertices;
		for(int v=0;v<poly->GetNumPolyVertices();v++)
		{
			ExVertexStruct t;
			t.Position = poly->getVertices()[v]->Position;
			t.TexCoord = poly->getFeatures()[v]->texCoord;
			t.Normal = poly->getFeatures()[v]->normal;
			t.Color = poly->getFeatures()[v]->lightStatic;

			t.TexCoord.x = std::min(1.0f, std::max(0.0f, t.TexCoord.x));
			t.TexCoord.y = std::min(1.0f, std::max(0.0f, t.TexCoord.y));

			polyVertices.push_back(t);
		}		

		// Make triangles
		std::vector<ExVertexStruct> finalVertices;
		TriangleFanToList(&polyVertices[0], polyVertices.size(), &finalVertices);

		// Add to main vector
		quadVertices.insert(quadVertices.end(), finalVertices.begin(), finalVertices.end());
	}

	if (quadVertices.empty())
		return;

	delete info->Mesh; info->Mesh = nullptr;
	Engine::GraphicsEngine->CreateVertexBuffer(&info->Mesh);

	// Init and fill it
	info->Mesh->Init(&quadVertices[0], quadVertices.size() * sizeof(ExVertexStruct));
	info->NumVertices = quadVertices.size();

	info->Position = position;
}

/** Turns a MeshInfo into PNAEN */
void WorldConverter::CreatePNAENInfoFor(MeshInfo * mesh, bool softNormals)
{
	delete mesh->MeshIndexBufferPNAEN;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshIndexBufferPNAEN);

	delete mesh->MeshVertexBuffer;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshVertexBuffer);

	mesh->VerticesPNAEN = mesh->Vertices;

	MeshModifier::ComputePNAEN18Indices(mesh->VerticesPNAEN, mesh->Indices, mesh->IndicesPNAEN, true, softNormals);
	mesh->MeshIndexBufferPNAEN->Init(&mesh->IndicesPNAEN[0], mesh->IndicesPNAEN.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	mesh->MeshVertexBuffer->Init(&mesh->VerticesPNAEN[0], mesh->VerticesPNAEN.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
}


void WorldConverter::CreatePNAENInfoFor(WorldMeshInfo * mesh, bool softNormals)
{
	delete mesh->MeshIndexBufferPNAEN;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshIndexBufferPNAEN);

	delete mesh->MeshVertexBuffer;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshVertexBuffer);

	mesh->VerticesPNAEN = mesh->Vertices;

	MeshModifier::ComputePNAEN18Indices(mesh->VerticesPNAEN, mesh->Indices, mesh->IndicesPNAEN, true, softNormals);
	mesh->MeshIndexBufferPNAEN->Init(&mesh->IndicesPNAEN[0], mesh->IndicesPNAEN.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	mesh->MeshVertexBuffer->Init(&mesh->VerticesPNAEN[0], mesh->VerticesPNAEN.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
}

/** Turns a MeshInfo into PNAEN */
void WorldConverter::CreatePNAENInfoFor(SkeletalMeshInfo * mesh, MeshInfo * bindPoseMesh, bool softNormals)
{
	delete mesh->MeshIndexBufferPNAEN;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshIndexBufferPNAEN);

	delete mesh->MeshVertexBuffer;
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshVertexBuffer);

	delete bindPoseMesh->MeshIndexBufferPNAEN;
	Engine::GraphicsEngine->CreateVertexBuffer(&bindPoseMesh->MeshIndexBufferPNAEN);

	delete bindPoseMesh->MeshVertexBuffer;
	Engine::GraphicsEngine->CreateVertexBuffer(&bindPoseMesh->MeshVertexBuffer);

	bindPoseMesh->VerticesPNAEN = bindPoseMesh->Vertices;

	MeshModifier::ComputePNAEN18Indices(bindPoseMesh->VerticesPNAEN, mesh->Indices, mesh->IndicesPNAEN, true, softNormals);
	bindPoseMesh->IndicesPNAEN = mesh->IndicesPNAEN;

	mesh->MeshIndexBufferPNAEN->Init(&mesh->IndicesPNAEN[0], mesh->IndicesPNAEN.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	bindPoseMesh->MeshIndexBufferPNAEN->Init(&bindPoseMesh->IndicesPNAEN[0], bindPoseMesh->IndicesPNAEN.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	
	for(unsigned int i=0;i<mesh->Vertices.size();i++)
	{
		// Transfer the normals, in case they changed
		mesh->Vertices[i].Normal = bindPoseMesh->VerticesPNAEN[i].Normal;
	}

	mesh->MeshVertexBuffer->Init(&mesh->Vertices[0], mesh->Vertices.size() * sizeof(ExSkelVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	bindPoseMesh->MeshVertexBuffer->Init(&bindPoseMesh->VerticesPNAEN[0], bindPoseMesh->VerticesPNAEN.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
}

/** Converts ExVertexStruct into a zCPolygon*-Attay */
void WorldConverter::ConvertExVerticesTozCPolygons(const std::vector<ExVertexStruct> & vertices, const std::vector<VERTEX_INDEX> & indices, zCMaterial * material, std::vector<zCPolygon *> & polyArray)
{
	for(size_t i=0;i<indices.size();i+=3)
	{
		// Create and init polyong
		zCPolygon* poly = new zCPolygon();
		poly->Constructor();
		poly->AllocVertPointers(3);
		poly->AllocVertData();
		poly->SetMaterial(material);

		// Fill data
		zCVertex** vx = poly->getVertices();
		for(int v=0;v<3;v++)
		{
			
			vx[v]->MyIndex = v;
			vx[v]->TransformedIndex = 0;
			vx[v]->Position = vertices[indices[i + v]].Position;

			poly->getFeatures()[v]->lightStatic = 0xFFFFFFFF;
			poly->getFeatures()[v]->normal = vertices[indices[i + v]].Normal;
			poly->getFeatures()[v]->texCoord = vertices[indices[i + v]].TexCoord;
		}

		zCVertex* vtmp = vx[1];
		zCVertFeature* ftmp = poly->getFeatures()[1];

		vx[1] = vx[2];
		poly->getFeatures()[1] = poly->getFeatures()[2];

		vx[2] = vtmp;
		poly->getFeatures()[2] = ftmp;

		poly->CalcNormal();

		// Add to array
		polyArray.push_back(poly);
	}
}

/** Tesselates the given mesh the given amount of times */
void WorldConverter::TesselateMesh(WorldMeshInfo * mesh, int amount)
{
	// Copy old vertices so we can directly write to the vectors again
	std::vector<ExVertexStruct> vxOld = mesh->Vertices;
	std::vector<unsigned short> ixOld = mesh->Indices;
	
	// Tesselate if the outcome would still be in 16-bit range
	if (amount > 1 && mesh->Vertices.size() + (mesh->Indices.size() / 3) < 0x0000FFFF)
	{
		std::vector<ExVertexStruct> meshTess;
		for(unsigned int i=0;i<mesh->Indices.size();i+=3)
		{
			ExVertexStruct vx[3];
			vx[0] = mesh->Vertices[mesh->Indices[i]];
			vx[1] = mesh->Vertices[mesh->Indices[i+1]];
			vx[2] = mesh->Vertices[mesh->Indices[i+2]];

		
			std::vector<ExVertexStruct> triTess;
			WorldConverter::TesselateTriangle(vx, triTess, 1);

			// Append
			for(unsigned int v=0;v<triTess.size();v++)
			{
				meshTess.push_back(triTess[v]);
			}
		}
	
		mesh->Vertices.clear();
		mesh->Indices.clear();

		// Index
		WorldConverter::IndexVertices(&meshTess[0], meshTess.size(), mesh->Vertices, mesh->Indices);
	}

	MeshModifier::ComputePNAEN18Indices(mesh->Vertices, mesh->Indices, mesh->IndicesPNAEN, true, true);
	if (mesh->Vertices.size() >= 0xFFFF)
	{
		// Too large
		return;
	}


	// Cleanup
	delete mesh->MeshVertexBuffer;
	delete mesh->MeshIndexBuffer;
	delete mesh->MeshIndexBufferPNAEN;

	// Recreate the buffers
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshVertexBuffer);
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshIndexBufferPNAEN);
	Engine::GraphicsEngine->CreateVertexBuffer(&mesh->MeshIndexBuffer);

	// Init and fill them
	mesh->MeshVertexBuffer->Init(&mesh->Vertices[0], mesh->Vertices.size() * sizeof(ExVertexStruct), D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	mesh->MeshIndexBufferPNAEN->Init(&mesh->IndicesPNAEN[0], mesh->IndicesPNAEN.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);
	mesh->MeshIndexBuffer->Init(&mesh->Indices[0], mesh->Indices.size() * sizeof(VERTEX_INDEX), D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE);

	mesh->TesselationSettings.buffer.VT_TesselationFactor = 1.0f;
	mesh->TesselationSettings.buffer.VT_DisplacementStrength = 0.5f;
	mesh->TesselationSettings.UpdateConstantbuffer();

	// Mark dirty
	mesh->SaveInfo = true;
}