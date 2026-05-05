#include "BfresRenderer.h"

#include <util/Logger.h>
#include <util/FileUtil.h>
#include <manager/TexToGoFileMgr.h>
#include <manager/TextureMgr.h>
#include <algorithm>

namespace application::gl
{
	namespace
	{
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

		mInstanceMatrix.GenBuffer(GL_ARRAY_BUFFER);

		for (auto& [ModelKey, ModelVal] : mBfresFile->Models.mNodes)
		{
			application::file::game::bfres::BfresFile::Model& Model = ModelVal.mValue;
			mIndexBuffers.resize(Model.Shapes.mNodes.size());
			mShapeVAOs.resize(Model.Shapes.mNodes.size());
			mShapeBuffers.resize(Model.Shapes.mNodes.size());
			mMaterials.resize(Model.Shapes.mNodes.size());
			uint16_t ShapeIndex = 0;
			for (auto& [ShapeKey, ShapeVal] : Model.Shapes.mNodes)
			{
				application::file::game::bfres::BfresFile::Shape& Shape = ShapeVal.mValue;
				mMaterials[ShapeIndex].mMaterialName = Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Name;

				if (Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures.empty())
				{
					mIndexBuffers.resize(mIndexBuffers.size() - 1);
					mShapeVAOs.resize(mShapeVAOs.size() - 1);
					mShapeBuffers.resize(mShapeBuffers.size() - 1);
					mMaterials.resize(mMaterials.size() - 1);
					continue;
				}

				mIndexBuffers[ShapeIndex].first.GenBuffer(GL_ELEMENT_ARRAY_BUFFER);
				mIndexBuffers[ShapeIndex].first.SetData<unsigned char>(Shape.Meshes[0].IndexBuffer);
				mIndexBuffers[ShapeIndex].second = Shape.Meshes[0].IndexCount;

				mShapeBuffers[ShapeIndex].resize(Shape.Buffer.Buffers.size() + 1);

				for (auto& [RenderInfoKey, RenderInfoVal] : Model.Materials.mNodes[Model.Materials.GetKey(Shape.MaterialIndex)].mValue.RenderInfos.mNodes)
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
				}
				for (auto& [ParametersKey, ParametersVal] : Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.ShaderParams.mNodes)
				{
					if (ParametersVal.mValue.Name == "p_texture_array_index0")
					{
						mMaterials[ShapeIndex].mTextureArrayIndex = (uint32_t)std::get<float>(ParametersVal.mValue.DataValue);
					}
				}

				mIsDiscard = mIsDiscard || mMaterials[ShapeIndex].mEnableAlphaTest;

				uint32_t AlbedoCount = 0;
				std::vector<application::file::game::texture::TexToGoFile*> Textures;
				for (auto& [Key, Val] : Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Samplers.mNodes)
				{
					if (Key.starts_with("_a") && Key.length() == 3)
					{
						const std::string Path = (mBfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures[AlbedoCount] + ".txtg") : (mBfresFile->mTexDir + "/" + Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures[AlbedoCount] + ".txtg"));
						if (application::util::FileUtil::FileExists(Path))
						{
							Textures.push_back(application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH));
							AlbedoCount++;
						}
					}
				}

				if (AlbedoCount == 0)
				{
					const std::string Path = mBfresFile->mTexDir == "" ? application::util::FileUtil::GetRomFSFilePath("TexToGo/" + Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures.front() + ".txtg") : (mBfresFile->mTexDir + "/" + Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures.front() + ".txtg");
					if (!application::util::FileUtil::FileExists(Path))
					{
						mIndexBuffers.resize(mIndexBuffers.size() - 1);
						mShapeVAOs.resize(mShapeVAOs.size() - 1);
						mShapeBuffers.resize(mShapeBuffers.size() - 1);
						mMaterials.resize(mMaterials.size() - 1);
						continue;
					}
					AlbedoCount = 1;
					Textures.push_back(application::manager::TexToGoFileMgr::GetTexture(Path, application::manager::TexToGoFileMgr::AccesMode::PATH));
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
				else
				{
					LoadFallbackTexture(mMaterials[ShapeIndex]);
				}

				// Collect all material texture channels as replacement candidates.
				// Model variation FMABs often swap bound texture channels rather than texture array layers.
				for (const std::string& TextureName : Model.Materials.GetByIndex(Shape.MaterialIndex).mValue.Textures)
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

				ShapeIndex++;
			}
		}

		//Calculating sphere radius
		{
			float MaxDistSq = 0.0f;

			for (auto& [ModelKey, ModelVal] : mBfresFile->Models.mNodes)
			{
				application::file::game::bfres::BfresFile::Model& Model = ModelVal.mValue;
				for (auto& [ShapeKey, ShapeVal] : Model.Shapes.mNodes)
				{
					for (const uint32_t& Index : ShapeVal.mValue.Meshes[0].GetIndices()) {
						// Using length2 to avoid an expensive sqrt until the end.
						const glm::vec3& Vertex = ShapeVal.mValue.Vertices[Index];
						float DistSq = glm::length2(Vertex);
						if (DistSq > MaxDistSq)
							MaxDistSq = DistSq;
					}
				}
			}

			mSphereBoundingBoxRadius = std::sqrtf(MaxDistSq);
		}
	}

	void BfresRenderer::Draw(std::vector<glm::mat4>& ModelMatrices, int32_t TextureArrayIndexOverride, const std::unordered_map<std::string, std::string>* MaterialAlbedoTextureOverrides)
	{
		mInstanceMatrix.SetData<glm::mat4>(ModelMatrices);

		glDisable(GL_CULL_FACE);

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

			glDrawElementsInstanced(GL_TRIANGLES, mIndexBuffers[i].second, mMaterials[i].mIndexFormat, 0, ModelMatrices.size());
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

			glDrawElementsInstanced(GL_TRIANGLES, mIndexBuffers[i].second, mMaterials[i].mIndexFormat, 0, ModelMatrices.size());
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