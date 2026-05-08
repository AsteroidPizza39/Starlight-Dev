#pragma once

#include <glad/glad.h>
#include <file/game/bfres/BfresFile.h>
#include <unordered_map>
#include <gl/BufferObject.h>
#include <gl/Texture.h>
#include <gl/VertexArrayObject.h>
#include <file/game/texture/TexToGoFile.h>

namespace application::gl
{
	class BfresRenderer
	{
	public:
		struct FormatInfo
		{
			int32_t mCount;
			bool mNormalized;
			GLenum mType;
		};

		enum class RenderStateMode : uint8_t
		{
			OPAQUE = 0,
			TRANSLUCENT = 1,
			MASK = 2
		};

		struct Material
		{
			bool mEnableAlphaTest = false;
			RenderStateMode mRenderStateMode = RenderStateMode::OPAQUE;
			uint16_t mTextureArrayIndex = 0;
			std::string mMaterialName;

			GLenum mRenderStateDisplayFace = GL_NONE;
			GLenum mIndexFormat = GL_UNSIGNED_INT;

			std::string mGsysPass;
			uint16_t mGsysPassSortKey = 100;
			/** Seal vs no_setting coplanar hint from mesh proximity (vertices/triangles in BFRES vertex space), not actor origin or draw instance transforms; polygon offset when true. */
			bool mGsysSealProximityDepthBias = false;

			application::gl::Texture* mAlbedoTexture = nullptr;
			application::file::game::texture::TexToGoFile* mAlbedoTexToGoFile = nullptr;
			std::vector<application::file::game::texture::TexToGoFile*> mTextureCandidates;
			uint16_t mDefaultTextureCandidateIndex = 0;
		};

		struct DrawElementsIndirectCommand
		{
			uint32_t mCount;
			uint32_t mInstanceCount;
			uint32_t mFirstIndex;
			int32_t mBaseVertex;
			uint32_t mBaseInstance;
		};

		static std::unordered_map<application::file::game::bfres::BfresFile::BfresAttribFormat, application::gl::BfresRenderer::FormatInfo> gFormatList;
		static std::unordered_map<std::string, uint32_t> gAttributeLocations;

		BfresRenderer() = default;
		BfresRenderer(application::file::game::bfres::BfresFile* BfresFile);

		void LoadFallbackTexture(Material& Material);
		void Initialize(application::file::game::bfres::BfresFile* BfresFile);
		void Draw(std::vector<glm::mat4>& ModelMatrices, int32_t TextureArrayIndexOverride = -1, const std::unordered_map<std::string, std::string>* MaterialAlbedoTextureOverrides = nullptr);
		void Delete();

		application::file::game::bfres::BfresFile* mBfresFile = nullptr;
		application::gl::BufferObject mInstanceMatrix;
		std::vector<std::pair<application::gl::BufferObject, uint32_t>> mIndexBuffers;
		std::vector<std::vector<BufferObject>> mShapeBuffers;
		std::vector<application::gl::VertexArrayObject> mShapeVAOs;
		std::vector<Material> mMaterials;
		std::vector<uint16_t> mOpaqueObjects;
		std::vector<uint16_t> mTransparentObjects;
		bool mIsDiscard = false;
		bool mIsSystemModelTransparent = false;
		float mSphereBoundingBoxRadius = 0.0f;
	};
}