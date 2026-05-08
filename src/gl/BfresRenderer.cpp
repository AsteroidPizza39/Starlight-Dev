#include "BfresRenderer.h"

#include <util/Logger.h>
#include <util/FileUtil.h>
#include <manager/TexToGoFileMgr.h>
#include <manager/TextureMgr.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace application::gl
{
	namespace
	{
		bool TryResolveMaterialByIndex(
			application::file::game::bfres::BfresFile::Model& Model,
			uint32_t MaterialIndex,
			application::file::game::bfres::BfresFile::Material** OutMaterial,
			std::string* OutMaterialKey = nullptr)
		{
			for (auto& [MaterialKey, MaterialNode] : Model.Materials.mNodes)
			{
				if (MaterialNode.mIndex != MaterialIndex)
				{
					continue;
				}

				*OutMaterial = &MaterialNode.mValue;
				if (OutMaterialKey != nullptr)
				{
					*OutMaterialKey = MaterialKey;
				}
				return true;
			}

			*OutMaterial = nullptr;
			if (OutMaterialKey != nullptr)
			{
				OutMaterialKey->clear();
			}
			return false;
		}

		bool IsLikelyAlbedoTextureName(const std::string& TextureName)
		{
			std::string Lower = TextureName;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
				{
					return static_cast<char>(std::tolower(C));
				});

			// Typical Nintendo naming for base color maps in this project.
			return Lower.find("alb") != std::string::npos || Lower.find("albedo") != std::string::npos || Lower.find("basecolor") != std::string::npos;
		}

		/** Substring buckets for gsys_pass: lower draw order draws first within opaque / transparent queues. Seal must draw after no_setting when geometry overlaps. */
		uint16_t GsysPassSortKeyFromRaw(const std::string& PassRaw)
		{
			std::string Lower = PassRaw;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
				{
					return static_cast<char>(std::tolower(C));
				});
			if (Lower.find("seal") != std::string::npos)
			{
				return 200;
			}
			if (Lower.find("no_setting") != std::string::npos)
			{
				return 0;
			}
			return 100;
		}

		bool GsysPassHasSealSubstring(const std::string& PassRaw)
		{
			std::string Lower = PassRaw;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
				{
					return static_cast<char>(std::tolower(C));
				});
			return Lower.find("seal") != std::string::npos;
		}

		bool GsysPassHasNoSettingSubstring(const std::string& PassRaw)
		{
			std::string Lower = PassRaw;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
				{
					return static_cast<char>(std::tolower(C));
				});
			return Lower.find("no_setting") != std::string::npos;
		}

		// Euclidean distance squared from P to triangle ABC (Real-Time Collision Detection closest-point construction).
		float PointTriangleDistanceSq(const glm::vec3& P, const glm::vec3& A, const glm::vec3& B, const glm::vec3& C)
		{
			const glm::vec3 AB = B - A;
			const glm::vec3 AC = C - A;
			const glm::vec3 AP = P - A;
			const float d1 = glm::dot(AB, AP);
			const float d2 = glm::dot(AC, AP);
			if (d1 <= 0.f && d2 <= 0.f)
			{
				return glm::length2(P - A);
			}
			const glm::vec3 BP = P - B;
			const float d3 = glm::dot(AB, BP);
			const float d4 = glm::dot(AC, BP);
			if (d3 >= 0.f && d4 <= d3)
			{
				return glm::length2(P - B);
			}
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
			{
				const float V = d1 / (d1 - d3);
				return glm::length2(P - (A + V * AB));
			}
			const glm::vec3 CP = P - C;
			const float d5 = glm::dot(AB, CP);
			const float d6 = glm::dot(AC, CP);
			if (d6 >= 0.f && d5 <= d6)
			{
				return glm::length2(P - C);
			}
			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
			{
				const float W = d2 / (d2 - d6);
				return glm::length2(P - (A + W * AC));
			}
			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
			{
				const float W = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				return glm::length2(P - (B + W * (C - B)));
			}
			const float denom = 1.f / (va + vb + vc);
			const float v = vb * denom;
			const float w = vc * denom;
			return glm::length2(P - (A + AB * v + AC * w));
		}

		void AppendTrianglesForShape(
			const application::file::game::bfres::BfresFile::Shape& Shape,
			std::vector<std::array<glm::vec3, 3>>& OutTriangles)
		{
			if (Shape.Meshes.empty())
			{
				return;
			}
			const std::vector<uint32_t> Indices = Shape.Meshes[0].GetIndices();
			const size_t VertexCount = Shape.Vertices.size();
			for (size_t t = 0; t + 2 < Indices.size(); t += 3)
			{
				const uint32_t Ia = Indices[t];
				const uint32_t Ib = Indices[t + 1];
				const uint32_t Ic = Indices[t + 2];
				if (Ia >= VertexCount || Ib >= VertexCount || Ic >= VertexCount)
				{
					continue;
				}
				OutTriangles.push_back(
				{
					glm::vec3(Shape.Vertices[Ia]),
					glm::vec3(Shape.Vertices[Ib]),
					glm::vec3(Shape.Vertices[Ic])
				});
			}
		}

		/** Mark seal mats for depth bias: distance is mesh-only (each vertex vs no_setting triangles in Shape.Vertices space). Ignores actor/object translate & instance matrices. Threshold 0.005 in those mesh units. */
		void ApplyGsysSealNoSettingProximityMarkers(
			const std::vector<const application::file::game::bfres::BfresFile::Shape*>& ShapesByDrawIndex,
			std::vector<BfresRenderer::Material>& Materials)
		{
			if (ShapesByDrawIndex.size() != Materials.size())
			{
				return;
			}
			std::vector<std::array<glm::vec3, 3>> NoSettingTriangles;
			for (size_t i = 0; i < Materials.size(); ++i)
			{
				if (!GsysPassHasNoSettingSubstring(Materials[i].mGsysPass))
				{
					continue;
				}
				AppendTrianglesForShape(*ShapesByDrawIndex[i], NoSettingTriangles);
			}
			if (NoSettingTriangles.empty())
			{
				return;
			}
			// World/placement matrices are applied only at Draw(); this epsilon is purely mesh vertex geometry.
			constexpr float SealNoSettingEpsilonSq = 0.005f * 0.005f;
			for (size_t si = 0; si < Materials.size(); ++si)
			{
				if (!GsysPassHasSealSubstring(Materials[si].mGsysPass))
				{
					continue;
				}
				const application::file::game::bfres::BfresFile::Shape& SealShape = *ShapesByDrawIndex[si];
				bool Marked = false;
				for (const glm::vec4& PosH : SealShape.Vertices)
				{
					const glm::vec3 P(PosH.x, PosH.y, PosH.z);
					for (const auto& Tri : NoSettingTriangles)
					{
						if (PointTriangleDistanceSq(P, Tri[0], Tri[1], Tri[2]) <= SealNoSettingEpsilonSq)
						{
							Materials[si].mGsysSealProximityDepthBias = true;
							Marked = true;
							break;
						}
					}
					if (Marked)
					{
						break;
					}
				}
			}
		}

		// Slopes near 0 keeps bias mostly constant; pulls seal slightly nearer (GL_LESS wins) vs coplanar no_setting draws.
		constexpr GLfloat SealNoSettingProximityPolygonOffsetFactor = 0.f;
		constexpr GLfloat SealNoSettingProximityPolygonOffsetUnits = -30.f;

		application::file::game::texture::TexToGoFile* ResolveTexToGoByName(application::file::game::bfres::BfresFile* BfresFile, const std::string& TextureName)
		{
			const std::string Path = (BfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + TextureName + ".txtg") : (BfresFile->mTexDir + "/" + TextureName + ".txtg"));
			if (!application::util::FileUtil::FileExists(Path))
			{
				return nullptr;
			}

			return application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH);
		}
	}

	std::unordered_map<application::file::game::bfres::BfresFile::BfresAttribFormat, application::gl::BfresRenderer::FormatInfo> BfresRenderer::gFormatList =
	{
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_32_32_32_32_Single, {4, false, GL_FLOAT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_32_32_32_Single, {3, false, GL_FLOAT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_32_32_Single, {2, false, GL_FLOAT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_32_Single, {1, false, GL_FLOAT} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_16_16_Single, {4, false, GL_HALF_FLOAT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_Single, {2, false, GL_HALF_FLOAT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_Single, {2, false, GL_HALF_FLOAT} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_SNorm, {2, true, GL_SHORT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_UNorm, {2, true, GL_UNSIGNED_SHORT} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_10_10_10_2_SNorm, {4, true, GL_INT_2_10_10_10_REV} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_10_10_10_2_UNorm, {4, true, GL_UNSIGNED_INT_2_10_10_10_REV} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_8_8_SNorm, {4, true, GL_BYTE} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_8_8_UNorm, {4, true, GL_UNSIGNED_BYTE} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_UNorm, {2, true, GL_UNSIGNED_BYTE} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_SNorm, {2, true, GL_BYTE} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_UNorm, {1, true, GL_UNSIGNED_BYTE} },

		//Ints
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_10_10_10_2_UInt, {4, true, GL_UNSIGNED_INT} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_8_8_UInt, {4, false, GL_UNSIGNED_BYTE} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_8_UInt, {2, false, GL_UNSIGNED_BYTE} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_8_UInt, {1, false, GL_UNSIGNED_BYTE} },

		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_16_16_UInt, {4, false, GL_UNSIGNED_SHORT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_16_UInt, {2, false, GL_UNSIGNED_SHORT} },
		{ application::file::game::bfres::BfresFile::BfresAttribFormat::Format_16_UInt, {1, false, GL_UNSIGNED_SHORT} }
	};

	std::unordered_map<std::string, uint32_t> BfresRenderer::gAttributeLocations =
	{
		{"_p0", 0},
		{"_n0", 1},
		{"_t0", 2},
		{"_u0", 3},
		{"_instanceMatrix", 4}
	};

	BfresRenderer::BfresRenderer(application::file::game::bfres::BfresFile* BfresFile)
	{
		Initialize(BfresFile);
	}

	void BfresRenderer::LoadFallbackTexture(Material& Material)
	{
		Material.mAlbedoTexture = application::manager::TextureMgr::GetTexToGoSurfaceTexture(&application::manager::TexToGoFileMgr::GetTexture("TexturedCube.txtg")->GetSurface(0));
	}

	void BfresRenderer::Initialize(application::file::game::bfres::BfresFile* BfresFile)
	{
		mBfresFile = BfresFile;
		mIndexBuffers.clear();
		mShapeVAOs.clear();
		mShapeBuffers.clear();
		mMaterials.clear();
		mOpaqueObjects.clear();
		mTransparentObjects.clear();
		mIsDiscard = false;

		const uint32_t ModelCount = mBfresFile != nullptr ? static_cast<uint32_t>(mBfresFile->Models.mNodes.size()) : 0u;
		application::util::Logger::Info("LoadDebug", "BfresRenderer::Initialize begin BfresFile=%p ModelCount=%u", static_cast<const void*>(mBfresFile), ModelCount);

		mInstanceMatrix.GenBuffer(GL_ARRAY_BUFFER);

		for (auto& [ModelKey, ModelVal] : mBfresFile->Models.mNodes)
		{
			application::util::Logger::Info("LoadDebug", "BfresRenderer::Initialize Model=%s ShapeCount=%u MaterialCount=%u",
				ModelKey.c_str(),
				static_cast<uint32_t>(ModelVal.mValue.Shapes.mNodes.size()),
				static_cast<uint32_t>(ModelVal.mValue.Materials.mNodes.size()));
			application::file::game::bfres::BfresFile::Model& Model = ModelVal.mValue;
			mIndexBuffers.resize(Model.Shapes.mNodes.size());
			mShapeVAOs.resize(Model.Shapes.mNodes.size());
			mShapeBuffers.resize(Model.Shapes.mNodes.size());
			mMaterials.resize(Model.Shapes.mNodes.size());
			uint16_t ShapeIndex = 0;
			std::vector<const application::file::game::bfres::BfresFile::Shape*> ShapesByDrawIndex;
			ShapesByDrawIndex.reserve(Model.Shapes.mNodes.size());
			for (auto& [ShapeKey, ShapeVal] : Model.Shapes.mNodes)
			{
				auto SkipCurrentShape = [this]()
					{
						mIndexBuffers.resize(mIndexBuffers.size() - 1);
						mShapeVAOs.resize(mShapeVAOs.size() - 1);
						mShapeBuffers.resize(mShapeBuffers.size() - 1);
						mMaterials.resize(mMaterials.size() - 1);
					};

				application::file::game::bfres::BfresFile::Shape& Shape = ShapeVal.mValue;
				application::file::game::bfres::BfresFile::Material* ShapeMaterial = nullptr;
				if (!TryResolveMaterialByIndex(Model, Shape.MaterialIndex, &ShapeMaterial) || ShapeMaterial == nullptr)
				{
					application::util::Logger::Warning("BfresRenderer", "Skipping shape due to invalid material link (model=%s, shape=%s, shapeIndex=%u, materialIndex=%u, materialCount=%u)",
						ModelKey.c_str(),
						ShapeKey.c_str(),
						ShapeIndex,
						Shape.MaterialIndex,
						static_cast<uint32_t>(Model.Materials.mNodes.size()));
					SkipCurrentShape();
					continue;
				}
				mMaterials[ShapeIndex].mMaterialName = ShapeMaterial->Name;

				if (ShapeMaterial->Textures.empty())
				{
					application::util::Logger::Warning("BfresRenderer", "Skipping shape because material has no textures (model=%s, shape=%s, shapeIndex=%u, material=%s, materialIndex=%u)",
						ModelKey.c_str(),
						ShapeKey.c_str(),
						ShapeIndex,
						ShapeMaterial->Name.c_str(),
						Shape.MaterialIndex);
					SkipCurrentShape();
					continue;
				}
				if (Shape.Meshes.empty())
				{
					application::util::Logger::Warning("BfresRenderer", "Skipping shape because it has no meshes (model=%s, shape=%s, shapeIndex=%u, material=%s)",
						ModelKey.c_str(),
						ShapeKey.c_str(),
						ShapeIndex,
						ShapeMaterial->Name.c_str());
					SkipCurrentShape();
					continue;
				}
				if (Shape.Buffer.Buffers.empty())
				{
					application::util::Logger::Warning("BfresRenderer", "Skipping shape because it has no vertex buffers (model=%s, shape=%s, shapeIndex=%u, material=%s)",
						ModelKey.c_str(),
						ShapeKey.c_str(),
						ShapeIndex,
						ShapeMaterial->Name.c_str());
					SkipCurrentShape();
					continue;
				}

				mIndexBuffers[ShapeIndex].first.GenBuffer(GL_ELEMENT_ARRAY_BUFFER);
				mIndexBuffers[ShapeIndex].first.SetData<unsigned char>(Shape.Meshes[0].IndexBuffer);
				mIndexBuffers[ShapeIndex].second = Shape.Meshes[0].IndexCount;

				mShapeBuffers[ShapeIndex].resize(Shape.Buffer.Buffers.size() + 1);

				for (auto& [RenderInfoKey, RenderInfoVal] : ShapeMaterial->RenderInfos.mNodes)
				{
					if (RenderInfoVal.mValue.Name == "gsys_alpha_test_enable")
					{
						mMaterials[ShapeIndex].mEnableAlphaTest = RenderInfoVal.mValue.GetValueStrings()[0] == "true";
					}
					else if (RenderInfoVal.mValue.Name == "gsys_render_state_mode")
					{

						if (RenderInfoVal.mValue.GetValueStrings()[0] == "opaque")
						{
							mMaterials[ShapeIndex].mRenderStateMode = RenderStateMode::OPAQUE;
						}
						else if (RenderInfoVal.mValue.GetValueStrings()[0] == "mask")
						{
							mMaterials[ShapeIndex].mRenderStateMode = RenderStateMode::MASK;
						}
						else
						{
							mMaterials[ShapeIndex].mRenderStateMode = RenderStateMode::TRANSLUCENT;
						}
					}
					else if (RenderInfoVal.mValue.Name == "gsys_render_state_display_face")
					{

						if (RenderInfoVal.mValue.GetValueStrings()[0] == "front")
						{
							mMaterials[ShapeIndex].mRenderStateDisplayFace = GL_BACK;
						}
						else if (RenderInfoVal.mValue.GetValueStrings()[0] == "back")
						{
							mMaterials[ShapeIndex].mRenderStateDisplayFace = GL_FRONT;
						}
						//else if (RenderInfoVal.Value.GetValueStrings()[0] == "both")
						//{
							//mMaterials[ShapeIndex].mRenderStateDisplayFace = GL_NONE;
						//}
						else if (RenderInfoVal.mValue.GetValueStrings()[0] == "none" || RenderInfoVal.mValue.GetValueStrings()[0] == "both")
						{
							mMaterials[ShapeIndex].mRenderStateDisplayFace = GL_NONE;
						}
						else
						{
							application::util::Logger::Warning("BfresRenderer", "Unknown gsys_render_state_display_face value: %s", RenderInfoVal.mValue.GetValueStrings()[0].c_str());
						}
					}
					else if (RenderInfoVal.mValue.Name == "gsys_pass")
					{
						const std::vector<std::string>& PassStrings = RenderInfoVal.mValue.GetValueStrings();
						if (!PassStrings.empty())
						{
							mMaterials[ShapeIndex].mGsysPass = PassStrings[0];
							mMaterials[ShapeIndex].mGsysPassSortKey = GsysPassSortKeyFromRaw(PassStrings[0]);
						}
					}
				}
				for (auto& [ParametersKey, ParametersVal] : ShapeMaterial->ShaderParams.mNodes)
				{
					if (ParametersVal.mValue.Name == "p_texture_array_index0")
					{
						mMaterials[ShapeIndex].mTextureArrayIndex = (uint32_t)std::get<float>(ParametersVal.mValue.DataValue);
					}
				}

				mIsDiscard = mIsDiscard || mMaterials[ShapeIndex].mEnableAlphaTest;

				uint32_t AlbedoCount = 0;
				std::vector<application::file::game::texture::TexToGoFile*> Textures;
				for (auto& [Key, Val] : ShapeMaterial->Samplers.mNodes)
				{
					if (Key.starts_with("_a") && Key.length() == 3)
					{
						if (AlbedoCount >= ShapeMaterial->Textures.size())
						{
							application::util::Logger::Warning("BfresRenderer", "Sampler/texture count mismatch, stopping sampler albedo scan (model=%s, shape=%s, shapeIndex=%u, material=%s, materialIndex=%u, samplerCount=%u, textureCount=%u)",
								ModelKey.c_str(),
								ShapeKey.c_str(),
								ShapeIndex,
								ShapeMaterial->Name.c_str(),
								Shape.MaterialIndex,
								static_cast<uint32_t>(ShapeMaterial->Samplers.mNodes.size()),
								static_cast<uint32_t>(ShapeMaterial->Textures.size()));
							break;
						}

						const std::string& TextureName = ShapeMaterial->Textures[AlbedoCount];
						const std::string Path = (mBfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + TextureName + ".txtg") : (mBfresFile->mTexDir + "/" + TextureName + ".txtg"));
						if (application::util::FileUtil::FileExists(Path))
						{
							Textures.push_back(application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH));
							AlbedoCount++;
						}
					}
				}

				if (AlbedoCount == 0)
				{
					const std::string Path = mBfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + ShapeMaterial->Textures.front() + ".txtg") : (mBfresFile->mTexDir + "/" + ShapeMaterial->Textures.front() + ".txtg");
					if (!application::util::FileUtil::FileExists(Path))
					{
						application::util::Logger::Warning("BfresRenderer", "Using fallback albedo texture because first material texture is missing (model=%s, shape=%s, shapeIndex=%u, material=%s, missingTexture=%s)",
							ModelKey.c_str(),
							ShapeKey.c_str(),
							ShapeIndex,
							ShapeMaterial->Name.c_str(),
							ShapeMaterial->Textures.front().c_str());
						LoadFallbackTexture(mMaterials[ShapeIndex]);
					}
					else
					{
						AlbedoCount = 1;
						Textures.push_back(application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH));
					}
				}

				//Tries to find first not transparent texture, if it even exists
				if (AlbedoCount > 1)
				{
					for (uint32_t i = 0; i < AlbedoCount; i++)
					{
						if (Textures[i]->GetPolishedFormat() == application::file::game::texture::TextureFormat::Format::BC1_UNORM)
						{
							AlbedoCount = i + 1;
							break;
						}
					}
				}

				if (AlbedoCount > 0)
				{
					mMaterials[ShapeIndex].mAlbedoTexToGoFile = Textures[AlbedoCount - 1];
					mMaterials[ShapeIndex].mAlbedoTexture = application::manager::TextureMgr::GetTexToGoSurfaceTexture(&(Textures[AlbedoCount - 1]->GetSurface(mMaterials[ShapeIndex].mTextureArrayIndex)), GL_TEXTURE0, false, GL_NEAREST);
				}
				else if (mMaterials[ShapeIndex].mAlbedoTexture == nullptr)
				{
					LoadFallbackTexture(mMaterials[ShapeIndex]);
				}

				// Collect all material texture channels as replacement candidates.
				// Model variation FMABs often swap bound texture channels rather than texture array layers.
				for (const std::string& TextureName : ShapeMaterial->Textures)
				{
					if (!IsLikelyAlbedoTextureName(TextureName))
					{
						continue;
					}

					const std::string Path = (mBfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + TextureName + ".txtg") : (mBfresFile->mTexDir + "/" + TextureName + ".txtg"));
					if (!application::util::FileUtil::FileExists(Path))
					{
						continue;
					}

					application::file::game::texture::TexToGoFile* Candidate = application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH);
					if (std::find(mMaterials[ShapeIndex].mTextureCandidates.begin(), mMaterials[ShapeIndex].mTextureCandidates.end(), Candidate) == mMaterials[ShapeIndex].mTextureCandidates.end())
					{
						mMaterials[ShapeIndex].mTextureCandidates.push_back(Candidate);
					}
				}

				if (mMaterials[ShapeIndex].mAlbedoTexToGoFile != nullptr)
				{
					const auto Iter = std::find(mMaterials[ShapeIndex].mTextureCandidates.begin(), mMaterials[ShapeIndex].mTextureCandidates.end(), mMaterials[ShapeIndex].mAlbedoTexToGoFile);
					if (Iter != mMaterials[ShapeIndex].mTextureCandidates.end())
					{
						mMaterials[ShapeIndex].mDefaultTextureCandidateIndex = static_cast<uint16_t>(std::distance(mMaterials[ShapeIndex].mTextureCandidates.begin(), Iter));
					}
					else
					{
						mMaterials[ShapeIndex].mTextureCandidates.push_back(mMaterials[ShapeIndex].mAlbedoTexToGoFile);
						mMaterials[ShapeIndex].mDefaultTextureCandidateIndex = static_cast<uint16_t>(mMaterials[ShapeIndex].mTextureCandidates.size() - 1);
					}
				}

				mMaterials[ShapeIndex].mIndexFormat = Shape.Meshes[0].IndexFormat == application::file::game::bfres::BfresFile::BfresIndexFormat::UInt32 ? GL_UNSIGNED_INT : (Shape.Meshes[0].IndexFormat == application::file::game::bfres::BfresFile::BfresIndexFormat::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE);

				mShapeBuffers[ShapeIndex][0].GenBuffer(GL_ARRAY_BUFFER);
				mShapeBuffers[ShapeIndex][0].SetData<glm::vec4>(Shape.Vertices);

				uint8_t ShapeBufferIndex = 1;
				for (size_t i = 1; i < Shape.Buffer.Buffers.size(); i++)
				{
					mShapeBuffers[ShapeIndex][ShapeBufferIndex].GenBuffer(GL_ARRAY_BUFFER);
					mShapeBuffers[ShapeIndex][ShapeBufferIndex].SetData<unsigned char>(Shape.Buffer.Buffers[i].Data);
					ShapeBufferIndex++;
				}

				mShapeVAOs[ShapeIndex] = application::gl::VertexArrayObject(mShapeBuffers[ShapeIndex], mIndexBuffers[ShapeIndex].first);

				mShapeVAOs[ShapeIndex].mBuffers[ShapeBufferIndex] = mInstanceMatrix;

				for (auto& [AttributeKey, AttributeVal] : Shape.Buffer.Attributes.mNodes)
				{
					if (!gAttributeLocations.contains(AttributeVal.mValue.Name))
						continue;

					if (AttributeVal.mValue.Name != "_p0")
					{
						GLenum Format = gFormatList[AttributeVal.mValue.Format].mType;
						int32_t Count = gFormatList[AttributeVal.mValue.Format].mCount;
						int32_t Stride = Shape.Buffer.Buffers[AttributeVal.mValue.BufferIndex].Stride;
						bool Normalized = gFormatList[AttributeVal.mValue.Format].mNormalized;

						mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations[AttributeVal.mValue.Name], Count, Format, Normalized, Stride, AttributeVal.mValue.Offset, AttributeVal.mValue.BufferIndex);
					}
					else
					{
						mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations[AttributeVal.mValue.Name], 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), 0, 0);
					}
				}

				mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations["_instanceMatrix"], 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 0, Shape.Buffer.Buffers.size(), 1);
				mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations["_instanceMatrix"] + 1, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 1 * sizeof(glm::vec4), Shape.Buffer.Buffers.size(), 1);
				mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations["_instanceMatrix"] + 2, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 2 * sizeof(glm::vec4), Shape.Buffer.Buffers.size(), 1);
				mShapeVAOs[ShapeIndex].AddAttribute(gAttributeLocations["_instanceMatrix"] + 3, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 3 * sizeof(glm::vec4), Shape.Buffer.Buffers.size(), 1);

				/*
				mShapeVAOs[ShapeIndex].AddAttribute(mAttributeLocations["_boneTransformationMatrix"], 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 0, Shape.Buffer.Buffers.size() + 1, 0);
				mShapeVAOs[ShapeIndex].AddAttribute(mAttributeLocations["_boneTransformationMatrix"] + 1, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 1 * sizeof(glm::vec4), Shape.Buffer.Buffers.size() + 1, 0);
				mShapeVAOs[ShapeIndex].AddAttribute(mAttributeLocations["_boneTransformationMatrix"] + 2, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 2 * sizeof(glm::vec4), Shape.Buffer.Buffers.size() + 1, 0);
				mShapeVAOs[ShapeIndex].AddAttribute(mAttributeLocations["_boneTransformationMatrix"] + 3, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), 3 * sizeof(glm::vec4), Shape.Buffer.Buffers.size() + 1, 0);
				*/

				(mMaterials[ShapeIndex].mRenderStateMode == RenderStateMode::OPAQUE ? mOpaqueObjects : mTransparentObjects).push_back(ShapeIndex);

				ShapesByDrawIndex.push_back(&Shape);

				ShapeIndex++;
			}

			ApplyGsysSealNoSettingProximityMarkers(ShapesByDrawIndex, mMaterials);
		}

		auto SortDrawableIndicesByGsysPass = [this](std::vector<uint16_t>& Indices)
			{
				std::stable_sort(Indices.begin(), Indices.end(),
					[this](uint16_t Left, uint16_t Right)
					{
						const uint16_t KL = Left < static_cast<uint16_t>(mMaterials.size()) ? mMaterials[Left].mGsysPassSortKey : static_cast<uint16_t>(100);
						const uint16_t KR = Right < static_cast<uint16_t>(mMaterials.size()) ? mMaterials[Right].mGsysPassSortKey : static_cast<uint16_t>(100);
						if (KL != KR)
						{
							return KL < KR;
						}

						return Left < Right;
					});
			};
		SortDrawableIndicesByGsysPass(mOpaqueObjects);
		SortDrawableIndicesByGsysPass(mTransparentObjects);

		application::util::Logger::Info("LoadDebug", "BfresRenderer::Initialize computing bounding sphere BfresFile=%p", static_cast<const void*>(mBfresFile));

		//Calculating sphere radius
		{
			float MaxDistSq = 0.0f;

			for (auto& [ModelKey, ModelVal] : mBfresFile->Models.mNodes)
			{
				application::file::game::bfres::BfresFile::Model& Model = ModelVal.mValue;
				for (auto& [ShapeKey, ShapeVal] : Model.Shapes.mNodes)
				{
					application::file::game::bfres::BfresFile::Shape& Shape = ShapeVal.mValue;
					if (Shape.Meshes.empty())
					{
						application::util::Logger::Warning("BfresRenderer", "Skipping bounding-sphere shape with no meshes (model=%s, shape=%s)",
							ModelKey.c_str(), ShapeKey.c_str());
						continue;
					}
					if (Shape.Vertices.empty())
					{
						application::util::Logger::Warning("BfresRenderer", "Skipping bounding-sphere shape with no vertices (model=%s, shape=%s)",
							ModelKey.c_str(), ShapeKey.c_str());
						continue;
					}
					const size_t VertexCount = Shape.Vertices.size();
					for (const uint32_t& Index : Shape.Meshes[0].GetIndices()) {
						if (Index >= VertexCount)
						{
							continue;
						}
						// Using length2 to avoid an expensive sqrt until the end.
						const glm::vec3& Vertex = Shape.Vertices[Index];
						float DistSq = glm::length2(Vertex);
						if (DistSq > MaxDistSq)
							MaxDistSq = DistSq;
					}
				}
			}

			mSphereBoundingBoxRadius = std::sqrtf(MaxDistSq);
		}
		application::util::Logger::Info("LoadDebug", "BfresRenderer::Initialize end BfresFile=%p", static_cast<const void*>(mBfresFile));
	}

	void BfresRenderer::Draw(std::vector<glm::mat4>& ModelMatrices, int32_t TextureArrayIndexOverride, const std::unordered_map<std::string, std::string>* MaterialAlbedoTextureOverrides)
	{
		mInstanceMatrix.SetData<glm::mat4>(ModelMatrices);

		glDisable(GL_CULL_FACE);

		const auto SealProximityPolygonBiasBeforeDraw = [this](uint16_t Idx)
			{
				if (!mMaterials[Idx].mGsysSealProximityDepthBias)
				{
					return;
				}
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(SealNoSettingProximityPolygonOffsetFactor, SealNoSettingProximityPolygonOffsetUnits);
			};
		const auto SealProximityPolygonBiasAfterDraw = [this](uint16_t Idx)
			{
				if (!mMaterials[Idx].mGsysSealProximityDepthBias)
				{
					return;
				}
				glPolygonOffset(0.f, 0.f);
				glDisable(GL_POLYGON_OFFSET_FILL);
			};

		for (uint16_t i : mOpaqueObjects)
		{
			mShapeVAOs[i].Enable();
			mShapeVAOs[i].Use();

			application::gl::Texture* AlbedoTexture = mMaterials[i].mAlbedoTexture;
			application::file::game::texture::TexToGoFile* SelectedTexToGo = mMaterials[i].mAlbedoTexToGoFile;
			if (MaterialAlbedoTextureOverrides != nullptr)
			{
				if (const auto OverrideIter = MaterialAlbedoTextureOverrides->find(mMaterials[i].mMaterialName); OverrideIter != MaterialAlbedoTextureOverrides->end())
				{
					if (application::file::game::texture::TexToGoFile* OverrideTex = ResolveTexToGoByName(mBfresFile, OverrideIter->second); OverrideTex != nullptr)
					{
						SelectedTexToGo = OverrideTex;
					}
				}
			}
			if (TextureArrayIndexOverride >= 0 && mMaterials[i].mTextureCandidates.size() > 1)
			{
				const size_t CandidateIndex = std::min(static_cast<size_t>(TextureArrayIndexOverride), mMaterials[i].mTextureCandidates.size() - 1);
				SelectedTexToGo = mMaterials[i].mTextureCandidates[CandidateIndex];
			}

			if (SelectedTexToGo != nullptr)
			{
				AlbedoTexture = application::manager::TextureMgr::GetTexToGoSurfaceTexture(&SelectedTexToGo->GetSurface(mMaterials[i].mTextureArrayIndex), GL_TEXTURE0, false, GL_NEAREST);
			}
			AlbedoTexture->Bind();

			SealProximityPolygonBiasBeforeDraw(i);
			glDrawElementsInstanced(GL_TRIANGLES, mIndexBuffers[i].second, mMaterials[i].mIndexFormat, 0, ModelMatrices.size());
			SealProximityPolygonBiasAfterDraw(i);
		}

		for (uint16_t i : mTransparentObjects)
		{
			mShapeVAOs[i].Enable();
			mShapeVAOs[i].Use();

			application::gl::Texture* AlbedoTexture = mMaterials[i].mAlbedoTexture;
			application::file::game::texture::TexToGoFile* SelectedTexToGo = mMaterials[i].mAlbedoTexToGoFile;
			if (MaterialAlbedoTextureOverrides != nullptr)
			{
				if (const auto OverrideIter = MaterialAlbedoTextureOverrides->find(mMaterials[i].mMaterialName); OverrideIter != MaterialAlbedoTextureOverrides->end())
				{
					if (application::file::game::texture::TexToGoFile* OverrideTex = ResolveTexToGoByName(mBfresFile, OverrideIter->second); OverrideTex != nullptr)
					{
						SelectedTexToGo = OverrideTex;
					}
				}
			}
			if (TextureArrayIndexOverride >= 0 && mMaterials[i].mTextureCandidates.size() > 1)
			{
				const size_t CandidateIndex = std::min(static_cast<size_t>(TextureArrayIndexOverride), mMaterials[i].mTextureCandidates.size() - 1);
				SelectedTexToGo = mMaterials[i].mTextureCandidates[CandidateIndex];
			}

			if (SelectedTexToGo != nullptr)
			{
				AlbedoTexture = application::manager::TextureMgr::GetTexToGoSurfaceTexture(&SelectedTexToGo->GetSurface(mMaterials[i].mTextureArrayIndex), GL_TEXTURE0, false, GL_NEAREST);
			}
			AlbedoTexture->Bind();

			SealProximityPolygonBiasBeforeDraw(i);
			glDrawElementsInstanced(GL_TRIANGLES, mIndexBuffers[i].second, mMaterials[i].mIndexFormat, 0, ModelMatrices.size());
			SealProximityPolygonBiasAfterDraw(i);
		}
	}

	void BfresRenderer::Delete()
	{
		mInstanceMatrix.Delete();
		for (application::gl::VertexArrayObject& VAO : mShapeVAOs)
		{
			VAO.Delete();
		}
		for (std::vector<BufferObject>& BufferVec : mShapeBuffers)
		{
			for (BufferObject& Buffer : BufferVec)
			{
				Buffer.Delete();
			}
		}
		for (auto& [Buffer, Value] : mIndexBuffers)
		{
			Buffer.Delete();
		}

		mShapeVAOs.clear();
		mIndexBuffers.clear();
		mShapeBuffers.clear();
	}
}