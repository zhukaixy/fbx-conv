/*******************************************************************************
 * Copyright 2011 See AUTHORS file.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/
/** @author Xoppa */
#ifdef _MSC_VER
#pragma once
#endif //_MSC_VER
#ifndef FBXCONV_READERS_FBXCONVERTER_H
#define FBXCONV_READERS_FBXCONVERTER_H

#include <fbxsdk.h>
#include "../Settings.h"
#include "Reader.h"
#include <sstream>
#include <map>
#include <algorithm>
#include "util.h"
#include "FbxMeshInfo.h"
#include "../log/log.h"

using namespace fbxconv::modeldata;

namespace fbxconv {
namespace readers {
	struct TextureFileInfo {
		std::string path;
		// The uv bounds of this texture that are actually used (x1, y1, x2, y2)
		float bounds[4];
		// The number of nodes that use this texture
		unsigned int nodeCount;
		// The material textures that reference this texture
		std::vector<Material::Texture *> textures;
		TextureFileInfo() : nodeCount(0) {
			memset(bounds, -1, sizeof(float) * 4);
		}
	};

	typedef void (*TextureInfoCallback)(std::map<std::string, TextureFileInfo> &textures);

	bool FbxConverter_ImportCB(void *pArgs, float pPercentage, const char *pStatus);

	class FbxConverter : public Reader {
	public:
		FbxScene *scene;
		FbxManager *manager;

		// Resources (will be disposed)
		std::vector<FbxMeshInfo *> meshInfos;

		// Helper maps/lists, resources in those will not be disposed
		std::map<FbxGeometry *, FbxMeshInfo *> fbxMeshMap;
		std::map<FbxSurfaceMaterial *, Material *> materialsMap;
		std::map<std::string, TextureFileInfo> textureFiles;
		std::map<FbxMeshInfo *, std::vector<std::vector<MeshPart *> > > meshParts;
		std::map<const FbxNode *, Node *> nodeMap;

		Settings *settings;
		fbxconv::log::Log *log;
		TextureInfoCallback textureCallback;

		/** Temp array for transforming uvs, needs to be better defined. */
		Matrix3<float> uvTransforms[8];
		/** The original axis system the FBX file used (always converted defaultUpAxis, defaultFrontAxis and defaultCoordSystem) */
		FbxAxisSystem axisSystem;
		/** The original system units the FBX file used */
		FbxSystemUnit systemUnits;
		static const FbxAxisSystem::EUpVector defaultUpAxis = FbxAxisSystem::eYAxis;
		static const FbxAxisSystem::EFrontVector defaultFrontAxis = FbxAxisSystem::eParityOdd;
		static const FbxAxisSystem::ECoordSystem defaultCoordSystem = FbxAxisSystem::eRightHanded;

		//const char * const &filename, 
		//const bool &packColors = false, const unsigned int &maxVertexCount = (1<<15)-1, const unsigned int &maxIndexCount = (1<<15)-1,
			//const unsigned int &maxVertexBoneCount = 8, const bool &forceMaxVertexBoneCount = false, const unsigned int &maxNodePartBoneCount = (1 << 15)-1, 
			//const bool &flipV = false

		FbxConverter(fbxconv::log::Log *log, TextureInfoCallback textureCallback) 
			:	log(log), scene(0), textureCallback(textureCallback) {

			manager = FbxManager::Create();
			manager->SetIOSettings(FbxIOSettings::Create(manager, IOSROOT));
			manager->GetIOSettings()->SetBoolProp(IMP_FBX_GLOBAL_SETTINGS, true);
		}

		bool importCallback(float pPercentage, const char *pStatus) {
			log->progress(log::pSourceLoadFbxImport, pPercentage, pStatus);
			return true;
		}

		bool load(Settings *settings) {
			this->settings = settings;

			FbxImporter* const &importer = FbxImporter::Create(manager, "");

			if (settings->verbose)
				importer->SetProgressCallback(FbxConverter_ImportCB, this);

			importer->ParseForGlobalSettings(true);
			importer->ParseForStatistics(true);

			if (importer->Initialize(settings->inFile.c_str(), -1, manager->GetIOSettings())) {
				importer->GetAxisInfo(&axisSystem, &systemUnits);
				scene = FbxScene::Create(manager,"__FBX_SCENE__");
				importer->Import(scene);
			} else {
				log->error(fbxconv::log::eSourceLoadFbxSdk, "Unknown");
            }

			importer->Destroy();

			if (scene) {
				FbxAxisSystem axis(defaultUpAxis, defaultFrontAxis, defaultCoordSystem);
				axis.ConvertScene(scene);
			}
			if (scene)
				checkNodes();
			if (scene)
				prefetchMeshes();
			if (scene)
				fetchMaterials();
			if (scene)
				fetchTextureBounds();
			return !(scene == 0);
		}

		virtual ~FbxConverter() {
			for (std::vector<FbxMeshInfo *>::iterator itr = meshInfos.begin(); itr != meshInfos.end(); ++itr)
				delete (*itr);
			manager->Destroy();
		}

		/** Check all the nodes within the scene for any incompatibility issues. */
		void checkNodes() {
			FbxNode * root = scene->GetRootNode();
			for (int i = 0; i < root->GetChildCount(); i++)
				checkNode(root->GetChild(i));
		}

		/** Recursively check the node for any incompatibility issues. */
		void checkNode(FbxNode * const &node) {
			FbxTransform::EInheritType inheritType;
			node->GetTransformationInheritType(inheritType);
			if (inheritType == FbxTransform::eInheritRrSs) {
				log->warning(log::wSourceLoadFbxNodeRrSs, node->GetName());
				node->SetTransformationInheritType(FbxTransform::eInheritRSrs);
			}
			for (int i = 0; i < node->GetChildCount(); i++)
				checkNode(node->GetChild(i));
		}

		virtual bool convert(Model * const &model) {
			if (!scene) {
				log->error(log::eSourceLoadGeneral);
				return false;
			}
			if (textureCallback)
				textureCallback(textureFiles);
			for (int i = 0; i < 8; i++) {
				uvTransforms[i].idt();
				if (settings->flipV)
					uvTransforms[i].translate(0.f, 1.f).scale(1.f, -1.f);
			}

			for (std::map<FbxSurfaceMaterial *, Material *>::iterator it = materialsMap.begin(); it != materialsMap.end(); ++it) {
				model->materials.push_back(it->second);
				for (std::vector<Material::Texture *>::iterator tt = it->second->textures.begin(); tt != it->second->textures.end(); ++tt)
					(*tt)->path = textureFiles[(*tt)->path].path;
			}
			addMesh(model);
			addNode(model);
			for (std::vector<Node *>::iterator itr = model->nodes.begin(); itr != model->nodes.end(); ++itr)
				updateNode(model, *itr);
			addAnimations(model, scene);
			return true;
		}

		// Only recusively adds the node, doesnt extract any information
		void addNode(Model * const &model, Node * const &parent = 0, FbxNode * const &node = 0) {
			if (node == 0) {
				FbxNode * root = scene->GetRootNode();
				for (int i = 0; i < root->GetChildCount(); i++)
					addNode(model, parent, root->GetChild(i));
				return;
			}

			if (model->getNode(node->GetName())) {
				log->warning(log::wSourceConvertFbxDuplicateNodeId, node->GetName());
				return;
			}
			Node *n = new Node(node->GetName());
			n->source = node;
			nodeMap[node] = n;
			if (parent == 0)
				model->nodes.push_back(n);
			else
				parent->children.push_back(n);

			for (int i = 0; i < node->GetChildCount(); i++)
				addNode(model, n, node->GetChild(i));
		}

		void updateNode(Model * const &model, Node * const &node) {
			FbxAMatrix &m = node->source->EvaluateLocalTransform();
			set<3>(node->transform.translation, m.GetT().mData);
			set<4>(node->transform.rotation, m.GetQ().mData);
			set<3>(node->transform.scale, m.GetS().mData);
			for(int i= 0; i < 4; i++)
			{
				for(int j = 0; j < 4; j++)
				node->transforms[i*4 + j] = m.Get(i,j); 
			}

			if (fbxMeshMap.find(node->source->GetGeometry()) != fbxMeshMap.end()) {
				FbxMeshInfo *meshInfo = fbxMeshMap[node->source->GetGeometry()];
				std::vector<std::vector<MeshPart *> > &parts = meshParts[meshInfo];
				const int matCount = node->source->GetMaterialCount();
				for (int i = 0; i < matCount && i < parts.size(); i++) {
					Material *material = materialsMap[node->source->GetMaterial(i)];
					for (int j = 0; j < parts[i].size(); j++) {
						NodePart *nodePart = new NodePart();
						node->parts.push_back(nodePart);
						nodePart->material = material;
						nodePart->meshPart = parts[i][j];
						for (int k = 0; k < nodePart->meshPart->sourceBones.size(); k++) {
							if (nodeMap.find(nodePart->meshPart->sourceBones[k]->GetLink()) != nodeMap.end()) {
								std::pair<Node*, FbxAMatrix> p;
								p.first = nodeMap[nodePart->meshPart->sourceBones[k]->GetLink()];
								getBindPose(node->source, nodePart->meshPart->sourceBones[k], p.second);
								nodePart->bones.push_back(p);
							} else {
								log->warning(log::wSourceConvertFbxInvalidBone, node->id.c_str(), nodePart->meshPart->sourceBones[k]->GetLink()->GetName());
							}
						}

						nodePart->uvMapping.resize(meshInfo->uvCount);
						for (unsigned int k = 0; k < meshInfo->uvCount; k++) {
							for (std::vector<Material::Texture *>::iterator it = material->textures.begin(); it != material->textures.end(); ++it) {
								FbxFileTexture *texture = (*it)->source;
								TextureFileInfo &info = textureFiles[texture->GetFileName()];
								if (meshInfo->uvMapping[k] == texture->UVSet.Get().Buffer()) {
									nodePart->uvMapping[k].push_back(*it);
								}
							}
						}
					}
				}
			}

			for (std::vector<Node *>::iterator itr = node->children.begin(); itr != node->children.end(); ++itr)
				updateNode(model, *itr);
		}

		FbxAMatrix convertMatrix(const FbxMatrix& mat)
		{
			FbxVector4 trans, shear, scale;
			FbxQuaternion rot;
			double sign;
			mat.GetElements(trans, rot, shear, scale, sign);
			FbxAMatrix ret;
			ret.SetT(trans);
			ret.SetQ(rot);
			ret.SetS(scale);
			return ret;
		}

		// Get the geometry offset to a node. It is never inherited by the children.
		FbxAMatrix GetGeometry(FbxNode* pNode)
		{
			const FbxVector4 lT = pNode->GetGeometricTranslation(FbxNode::eSourcePivot);
			const FbxVector4 lR = pNode->GetGeometricRotation(FbxNode::eSourcePivot);
			const FbxVector4 lS = pNode->GetGeometricScaling(FbxNode::eSourcePivot);

			return FbxAMatrix(lT, lR, lS);
		}

		void getBindPose(FbxNode * target, FbxCluster *cluster, FbxAMatrix &out) {
			if (cluster->GetLinkMode() == FbxCluster::eAdditive)
				log->warning(log::wSourceConvertFbxAdditiveBones, target->GetName());

			FbxAMatrix reference;
			cluster->GetTransformMatrix(reference);
			FbxAMatrix refgem = GetGeometry(target);
			reference *= refgem;
			FbxAMatrix init;
			cluster->GetTransformLinkMatrix(init);
			FbxAMatrix relinit = init.Inverse() * reference;
			out = relinit;//.Inverse();
			//out = init;//init.Inverse();
		}

		// Iterate throught the nodes (from the leaves up) and the meshes it references. This might help that meshparts that are closer together are more likely to be merged
		// Note that in the end this is just another way of adding all items in meshInfos.
		void addMesh(Model * const &model, FbxNode * node = 0) {
			if (node == 0)
				node = scene->GetRootNode();
			const int childCount = node->GetChildCount();
			for (int i = 0; i < childCount; i++)
				addMesh(model, node->GetChild(i));

			if (fbxMeshMap.find(node->GetGeometry()) != fbxMeshMap.end())
				addMesh(model, fbxMeshMap[node->GetGeometry()], node);
		}

		void addMesh(Model * const &model, FbxMeshInfo * const &meshInfo, FbxNode * const &node) {
			if (meshParts.find(meshInfo) != meshParts.end())
				return;

			Mesh *mesh = findReusableMesh(model, meshInfo->attributes, meshInfo->polyCount * 3);
			if (mesh == 0) {
				mesh = new Mesh();
				model->meshes.push_back(mesh);
				mesh->attributes = meshInfo->attributes;
				mesh->vertexSize = mesh->attributes.size();
			}

			std::vector<std::vector<MeshPart *> > &parts = meshParts[meshInfo];
			parts.resize(meshInfo->meshPartCount);
			int idx = 0;
			for (int i = 0; i < meshInfo->meshPartCount; i++) {
				const int n = meshInfo->partBones[i].size();
				const int m = n == 0 ? 1 : n;
				parts[i].resize(m);
				for (int j = 0; j < m; j++) {
					std::stringstream ss;
					ss << meshInfo->id.c_str() <<  "_part" << (++idx);
					MeshPart *part = new MeshPart();
					part->id = ss.str();
					part->primitiveType = PRIMITIVETYPE_TRIANGLES;
					parts[i][j] = part;
					mesh->parts.push_back(part);
					if (j < n)
						for (int k = 0; k < meshInfo->partBones[i][j].size(); k++)
							part->sourceBones.push_back(meshInfo->getBone(meshInfo->partBones[i][j][k]));
				}
			}

			float *vertex = new float[mesh->vertexSize];
			unsigned int pidx = 0;
			for (unsigned int poly = 0; poly < meshInfo->polyCount; poly++) {
				unsigned int ps = meshInfo->mesh->GetPolygonSize(poly);
				MeshPart * const &part = parts[meshInfo->polyPartMap[poly]][meshInfo->polyPartBonesMap[poly]];
				Material * const &material = materialsMap[node->GetMaterial(meshInfo->polyPartMap[poly])];

				for (unsigned int i = 0; i < ps; i++) {
					const unsigned int v = meshInfo->mesh->GetPolygonVertex(poly, i);
					meshInfo->getVertex(vertex, poly, pidx, v, uvTransforms);
					part->indices.push_back(mesh->add(vertex));
					pidx++;
				}
			}
			delete[] vertex;
		}

		Mesh *findReusableMesh(Model * const &model, const Attributes &attributes, const unsigned int &vertexCount) {
			for (std::vector<Mesh *>::iterator itr = model->meshes.begin(); itr != model->meshes.end(); ++itr)
				if ((*itr)->attributes == attributes && 
					((*itr)->vertices.size() / (*itr)->vertexSize) + vertexCount <= settings->maxVertexCount && 
					(*itr)->indexCount() + vertexCount <= settings->maxIndexCount)
					return (*itr);
			return 0;
		}

		void fetchTextureBounds(FbxNode *node = 0) {
			if (node == 0)
				node = scene->GetRootNode();
			const int childCount = node->GetChildCount();
			for (int i = 0; i < childCount; i++)
				fetchTextureBounds(node->GetChild(i));

			FbxGeometry *geometry = node->GetGeometry();
			if (fbxMeshMap.find(geometry) == fbxMeshMap.end())
				return;
			FbxMeshInfo *meshInfo = fbxMeshMap[geometry];
			const int matCount = node->GetMaterialCount();
			for (int i = 0; i < matCount; i++) {
				FbxSurfaceMaterial *material = node->GetMaterial(i);
				Material *mat = materialsMap[material];
				for (std::vector<Material::Texture *>::iterator it = mat->textures.begin(); it != mat->textures.end(); ++it) {
					FbxFileTexture *texture = (*it)->source;
					TextureFileInfo &info = textureFiles[texture->GetFileName()];
					for (unsigned int k = 0; k < meshInfo->uvCount; k++) {
						if (meshInfo->uvMapping[k] == texture->UVSet.Get().Buffer()) {
							const int idx = 4 * (i * meshInfo->uvCount + k);
							if (*(int*)&info.bounds[0] == -1 || meshInfo->partUVBounds[idx] < info.bounds[0])
								info.bounds[0] = meshInfo->partUVBounds[idx];
							if (*(int*)&info.bounds[1] == -1 || meshInfo->partUVBounds[idx+1] < info.bounds[1])
								info.bounds[1] = meshInfo->partUVBounds[idx+1];
							if (*(int*)&info.bounds[2] == -1 || meshInfo->partUVBounds[idx+2] > info.bounds[2])
								info.bounds[2] = meshInfo->partUVBounds[idx+2];
							if (*(int*)&info.bounds[3] == -1 || meshInfo->partUVBounds[idx+3] > info.bounds[3])
								info.bounds[3] = meshInfo->partUVBounds[idx+3];
							info.nodeCount++;
							break;
						}
					}
				}
			}
		}

		const char *getGeometryName(const FbxGeometry * const &g) {
			static char buff[512];
			const char *name = g->GetName();
			if (name && strlen(name) > 0)
				return name;
			int c = g->GetNodeCount();
			strcpy(buff, "shape(");
			int idx = strlen(buff);
			for (int i = 0; i < c; i++) {
				const char *v = g->GetNode(i)->GetName();
				const int l = strlen(v);
				if (idx + l >= sizeof(buff))
					break;
				if (i > 0)
					buff[idx++] = ',';
				strcpy(&buff[idx], v);
				idx += l;
			}
			buff[idx++] = ')';
			buff[idx] = '\0';
			return buff;
		}

		void prefetchMeshes() {
			int cnt = scene->GetGeometryCount();
			FbxGeometryConverter converter(manager);
			for (int i = 0; i < cnt; i++) {
				FbxGeometry * const geometry = scene->GetGeometry(i);
				if (fbxMeshMap.find(geometry) == fbxMeshMap.end()) {
					FbxMesh *mesh;
					if (geometry->Is<FbxMesh>() && ((FbxMesh*)geometry)->IsTriangleMesh())
						mesh = (FbxMesh*)geometry;
					else {
						log->status(log::sSourceConvertFbxTriangulate, getGeometryName(geometry), geometry->GetClassId().GetName());
						//printf("Triangulating %s geometry\n", geometry->GetClassId().GetName());
						FbxNodeAttribute * const attr = converter.Triangulate(geometry, false);
						if (attr->Is<FbxMesh>())
							mesh = (FbxMesh*)attr;
						else {
							log->warning(log::wSourceConvertFbxCantTriangulate, geometry->GetClassId().GetName());
							continue;
						}
					}
					int indexCount = (mesh->GetPolygonCount() * 3);
					log->info(log::iSourceConvertFbxMeshInfo, getGeometryName(geometry), mesh->GetPolygonCount(), indexCount, mesh->GetControlPointsCount());
					if (indexCount > settings->maxIndexCount)
						log->warning(log::wSourceConvertFbxExceedsIndices, indexCount, settings->maxIndexCount);
					FbxMeshInfo * const info = new FbxMeshInfo(log, mesh, settings->packColors, settings->maxVertexBonesCount, settings->forceMaxVertexBoneCount, settings->maxNodePartBonesCount);
					meshInfos.push_back(info);
					fbxMeshMap[geometry] = info;
					if (info->bonesOverflow)
						log->warning(log::wSourceConvertFbxExceedsBones);
					if (info->elementMaterialCount <= 0) {
						log->error(log::eSourceConvertFbxNoMaterial, getGeometryName(geometry));
						scene = 0;
						break;
					}
				}
			}
		}

		void fetchMaterials() {
			int cnt = scene->GetMaterialCount();
			for (int i = 0; i < cnt; i++) {
				FbxSurfaceMaterial * const &material = scene->GetMaterial(i);
				if (materialsMap.find(material) == materialsMap.end())
					materialsMap[material] = createMaterial(material);
			}
		}

		Material *createMaterial(FbxSurfaceMaterial * const &material) {	
			Material * const result = new Material();
			result->source = material;
			result->id = material->GetName();

			if ((!material->Is<FbxSurfaceLambert>()) || GetImplementation(material, FBXSDK_IMPLEMENTATION_HLSL) || GetImplementation(material, FBXSDK_IMPLEMENTATION_CGFX)) {
				printf("Skipping unsupported material: %s, replacing it by a red diffuse color, because:\n", result->id.c_str());
				if (!material->Is<FbxSurfaceLambert>())
					printf("- Material must extend FbxSurfaceLambert\n");
				if (GetImplementation(material, FBXSDK_IMPLEMENTATION_HLSL))
					printf("- HLSL shading implementation not supported");
				if (GetImplementation(material, FBXSDK_IMPLEMENTATION_CGFX))
					printf("- CgFX shading implementation not supported");
				result->diffuse[0] = 1.f;
				result->diffuse[1] = 0.f;
				result->diffuse[2] = 0.f;
				return result;
			}

			FbxSurfaceLambert * const &lambert = (FbxSurfaceLambert *)material;
			set<3>(result->ambient, lambert->Ambient.Get().mData);
			set<3>(result->diffuse, lambert->Diffuse.Get().mData);
			set<3>(result->emissive, lambert->Emissive.Get().mData);

			addTextures(result->textures, lambert->Ambient, Material::Texture::Ambient);
			addTextures(result->textures, lambert->Diffuse, Material::Texture::Diffuse);
			addTextures(result->textures, lambert->Emissive, Material::Texture::Emissive);
			addTextures(result->textures, lambert->Bump, Material::Texture::Bump);
			addTextures(result->textures, lambert->NormalMap, Material::Texture::Normal);
			FbxDouble factor = lambert->TransparencyFactor.Get();
			FbxDouble3 color = lambert->TransparentColor.Get();
			FbxDouble trans = (color[0] * factor + color[1] * factor + color[2] * factor) / 3.0;
			result->opacity = 1.f - (float)trans;

			if (!material->Is<FbxSurfacePhong>())
				return result;

			FbxSurfacePhong * const &phong = (FbxSurfacePhong *)material;

			set<3>(result->specular, phong->Specular.Get().mData);
			result->shininess = (float)phong->Shininess.Get();

			addTextures(result->textures, phong->Specular, Material::Texture::Specular);
			addTextures(result->textures, phong->Reflection, Material::Texture::Reflection);
			return result;
		}

		inline void addTextures(std::vector<Material::Texture *> &textures, const FbxProperty &prop,  const Material::Texture::Usage &usage) {
			const unsigned int n = prop.GetSrcObjectCount<FbxFileTexture>();
			for (unsigned int i = 0; i < n; i++)
				add_if_not_null(textures, createTexture(prop.GetSrcObject<FbxFileTexture>(i), usage));
		}

		Material::Texture *createTexture(FbxFileTexture * const &texture, const Material::Texture::Usage &usage = Material::Texture::Unknown) {
			if (texture == 0)
				return 0;
			Material::Texture * const result = new Material::Texture();
			result->source = texture;
			result->id = texture->GetName();
			result->path = texture->GetFileName();
			set<2>(result->uvTranslation, texture->GetUVTranslation().mData);
			set<2>(result->uvScale, texture->GetUVScaling().mData);
			result->usage = usage;
			if (textureFiles.find(result->path) == textureFiles.end())
				textureFiles[result->path].path = result->path;
			textureFiles[result->path].textures.push_back(result);
			return result;
		}

		/** Add the animations if any */
		void addAnimations(Model * const &model, const FbxScene * const &source) {
			const unsigned int animCount = source->GetSrcObjectCount<FbxAnimStack>();
			for (unsigned int i = 0; i < animCount; i++)
				addAnimation(model, source->GetSrcObject<FbxAnimStack>(i));
		}

		/** Add the specified animation to the model */
		void addAnimation(Model *const &model, FbxAnimStack * const &animStack) {
			static std::vector<Keyframe *> frames;
			static std::map<FbxNode *, AnimInfo> affectedNodes;
			affectedNodes.clear();

			FbxTimeSpan animTimeSpan = animStack->GetLocalTimeSpan();
			float animStart = (float)(animTimeSpan.GetStart().GetMilliSeconds());
			float animStop = (float)(animTimeSpan.GetStop().GetMilliSeconds());
			if (animStop <= animStart)
				animStop = 999999999.0f;

			// Could also use animStack->GetLocalTimeSpan and animStack->BakeLayers, but its not guaranteed to be correct
			const int layerCount = animStack->GetMemberCount<FbxAnimLayer>();
			for (int l = 0; l < layerCount; l++) {
				FbxAnimLayer *layer = animStack->GetMember<FbxAnimLayer>(l);
				// For each layer check which node is affected and within what time frame and rate
				const int curveNodeCount = layer->GetSrcObjectCount<FbxAnimCurveNode>();
				for (int n = 0; n < curveNodeCount; n++) {
					FbxAnimCurveNode *curveNode = layer->GetSrcObject<FbxAnimCurveNode>(n);
					// Check which properties on this curve are changed
					const int nc = curveNode->GetDstPropertyCount();
					for (int o = 0; o < nc; o++) {
						FbxProperty prop = curveNode->GetDstProperty(o);
						FbxNode *node = static_cast<FbxNode *>(prop.GetFbxObject());
						if (node) {
							FbxString propName = prop.GetName();
							// Only add translation, scaling or rotation
							if ((!node->LclTranslation.IsValid() || propName != node->LclTranslation.GetName()) && 
								(!node->LclScaling.IsValid() || propName != node->LclScaling.GetName()) &&
								(!node->LclRotation.IsValid() || propName != node->LclRotation.GetName()))
								continue;
							FbxAnimCurve *curve;
							AnimInfo ts;
							ts.translate = propName == node->LclTranslation.GetName();
							ts.rotate = propName == node->LclRotation.GetName();
							ts.scale = propName == node->LclScaling.GetName();
							if (curve = prop.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_X))
								updateAnimTime(curve, ts, animStart, animStop);
							if (curve = prop.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Y))
								updateAnimTime(curve, ts, animStart, animStop);
							if (curve = prop.GetCurve(layer, FBXSDK_CURVENODE_COMPONENT_Z))
								updateAnimTime(curve, ts, animStart, animStop);
							//if (ts.start < ts.stop)
								affectedNodes[node] += ts;
						}
					}
				}
			}

			if (affectedNodes.empty())
				return;

			Animation *animation = new Animation();
			model->animations.push_back(animation);
			animation->id = animStack->GetName();
			animStack->GetScene()->SetCurrentAnimationStack(animStack);

			// Add the NodeAnimations to the Animation
			for (std::map<FbxNode *, AnimInfo>::const_iterator itr = affectedNodes.begin(); itr != affectedNodes.end(); itr++) {
				Node *node = model->getNode((*itr).first->GetName());
				if (!node)
					continue;
				frames.clear();
				NodeAnimation *nodeAnim = new NodeAnimation();
				nodeAnim->node = node;
				nodeAnim->translate = (*itr).second.translate;
				nodeAnim->rotate = (*itr).second.rotate;
				nodeAnim->scale = (*itr).second.scale;
				const float stepSize = (*itr).second.framerate <= 0.f ? (*itr).second.stop - (*itr).second.start : 1000.f / (*itr).second.framerate;
				const float last = (*itr).second.stop + stepSize * 0.5f;
				
				FbxTime fbxTime;
				float time;
				// Calculate all keyframes upfront
				for (time = (*itr).second.start; time <= last; time += stepSize) {
					time = std::min(time, (*itr).second.stop);
					fbxTime.SetMilliSeconds((FbxLongLong)time);
					Keyframe *kf = new Keyframe();
					kf->time = (time - animStart);
					FbxAMatrix *m = &(*itr).first->EvaluateLocalTransform(fbxTime);
					FbxVector4 v = m->GetT();
					kf->translation[0] = (float)v.mData[0];
					kf->translation[1] = (float)v.mData[1];
					kf->translation[2] = (float)v.mData[2];
					FbxQuaternion q = m->GetQ();
					kf->rotation[0] = (float)q.mData[0];
					kf->rotation[1] = (float)q.mData[1];
					kf->rotation[2] = (float)q.mData[2];
					kf->rotation[3] = (float)q.mData[3];
					v = m->GetS();
					kf->scale[0] = (float)v.mData[0];
					kf->scale[1] = (float)v.mData[1];
					kf->scale[2] = (float)v.mData[2];
					frames.push_back(kf);
				}
				

				animation->length = frames[frames.size()-1]->time;
				animation->length /= 1000;
				// Only add keyframes really needed
				addKeyframes(nodeAnim, frames);
				if (nodeAnim->rotate || nodeAnim->scale || nodeAnim->translate)
					animation->nodeAnimations.push_back(nodeAnim);
				else
					delete nodeAnim;
			}
		}

		inline void updateAnimTime(FbxAnimCurve *const &curve, AnimInfo &ts, const float &animStart, const float &animStop) {
			FbxTimeSpan fts;
			curve->GetTimeInterval(fts);
			const FbxTime start = fts.GetStart();
			const FbxTime stop = fts.GetStop();
			ts.start = std::max(animStart, std::min(ts.start, (float)(start.GetMilliSeconds())));
			ts.stop = std::min(animStop, std::max(ts.stop, (float)stop.GetMilliSeconds()));
			// Could check the number and type of keys (ie curve->KeyGetInterpolation) to lower the framerate
			ts.framerate = std::max(ts.framerate, (float)stop.GetFrameRate(FbxTime::eDefaultMode));
		}

		void addKeyframes(NodeAnimation *const &anim, std::vector<Keyframe *> &keyframes) {
			bool translate = false, rotate = false, scale = false;
			// Check which components are actually changed
			for (std::vector<Keyframe *>::const_iterator itr = keyframes.begin(); itr != keyframes.end(); ++itr) {
				if (!translate && !cmp(anim->node->transform.translation, (*itr)->translation, 3))
					translate = true;
				if (!rotate && !cmp(anim->node->transform.rotation, (*itr)->rotation, 3))
					rotate = true;
				if (!scale && !cmp(anim->node->transform.scale, (*itr)->scale, 3))
					scale = true;
			}
			// This allows to only export the values actual needed
			anim->translate = translate;
			anim->rotate = rotate;
			anim->scale = scale;
			for (std::vector<Keyframe *>::const_iterator itr = keyframes.begin(); itr != keyframes.end(); ++itr) {
				(*itr)->hasRotation = rotate;
				(*itr)->hasScale = scale;
				(*itr)->hasTranslation = translate;
			}

			if (!keyframes.empty()) {
				anim->keyframes.push_back(keyframes[0]);
				const int last = (int)keyframes.size()-1;
				float max = keyframes[last]->time;
				Keyframe *k1 = keyframes[0], *k2, *k3;
				for (int i = 1; i < last; i++) {
					k2 = keyframes[i];
					k3 = keyframes[i+1];
					// Check if the middle keyframe can be calculated by information, if so dont add it
					if ((translate && !isLerp(k1->translation, k1->time, k2->translation, k2->time, k3->translation, k3->time, 3)) ||
						(rotate && !isLerp(k1->rotation, k1->time, k2->rotation, k2->time, k3->rotation, k3->time, 3)) || // FIXME use slerp for quaternions
						(scale && !isLerp(k1->scale, k1->time, k2->scale, k2->time, k3->scale, k3->time, 3))) {

							k2->time /= max;
							anim->keyframes.push_back(k2);
							k1 = k2;
					} else
						delete k2;
				}
				if (last > 0)
				{	
					keyframes[last]->time = 1;
					anim->keyframes.push_back(keyframes[last]);
				}

			}
		}

		inline bool cmp(const float &v1, const float &v2, const float &epsilon = 0.000001) {
			const double d = v1 - v2;
			return ((d < 0.f) ? -d : d) < epsilon;
		}

		inline bool cmp(const float *v1, const float *v2, const unsigned int &count) {
			for (unsigned int i = 0; i < count; i++)
				if (!cmp(v1[i],v2[i]))
					return false;
			return true;
		}

		inline bool isLerp(const float *v1, const float &t1, const float *v2, const float &t2, const float *v3, const float &t3, const int size) {
			const double d = (t2 - t1) / (t3 - t1);
			for (int i = 0; i < size; i++)
				if (!cmp(v2[i], v1[i] + d * (v3[i] - v1[i])))
					return false;
			return true;
		}

		template<int n> inline static void set(float * const &dest, const FbxDouble * const &source) {
			for (int i = 0; i < n; i++)
				dest[i] = (float)source[i];
		}

		template<int n> inline static void set(double * const &dest, const FbxDouble * const &source) {
			for (int i = 0; i < n; i++)
				dest[i] = source[i];
		}

		template<class T> inline static void add_if_not_null(std::vector<T *> &dst, T * const &value) {
			if (value != 0)
				dst.push_back(value);
		}
	};

	bool FbxConverter_ImportCB(void *pArgs, float pPercentage, const char *pStatus) {
		return ((FbxConverter*)pArgs)->importCallback(pPercentage, pStatus);
	}
} }
#endif //FBXCONV_READERS_FBXCONVERTER_H