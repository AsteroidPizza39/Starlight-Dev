#include <manager/TerrainMgr.h>

#include <util/FileUtil.h>
#include <file/game/zstd/ZStdBackend.h>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace application::manager
{
	namespace
	{
		bool IsValidSarcPayload(const std::vector<unsigned char>& Bytes)
		{
			return Bytes.size() >= 4
				&& Bytes[0] == 'S' && Bytes[1] == 'A' && Bytes[2] == 'R' && Bytes[3] == 'C';
		}

		bool TryInitializeHghtArchive(const std::string& Path, application::file::game::terrain::HghtArchive& Archive)
		{
			if (!application::util::FileUtil::FileExists(Path))
				return false;

			const std::vector<unsigned char> Compressed = application::util::FileUtil::ReadFile(Path);
			if (Compressed.empty())
				return false;

			const std::vector<unsigned char> Decompressed =
				application::file::game::ZStdBackend::Decompress(Compressed);
			if (!IsValidSarcPayload(Decompressed))
			{
				application::util::Logger::Warning("TerrainMgr", "Skipping invalid HGHT archive: %s", Path.c_str());
				return false;
			}

			Archive.Initialize(Path);
			return true;
		}

		bool TryInitializeMateArchive(const std::string& Path, application::file::game::terrain::MateArchive& Archive,
			application::file::game::terrain::TerrainSceneFile& SceneFile)
		{
			if (!application::util::FileUtil::FileExists(Path))
				return false;

			const std::vector<unsigned char> Compressed = application::util::FileUtil::ReadFile(Path);
			if (Compressed.empty())
				return false;

			const std::vector<unsigned char> Decompressed =
				application::file::game::ZStdBackend::Decompress(Compressed);
			if (!IsValidSarcPayload(Decompressed))
			{
				application::util::Logger::Warning("TerrainMgr", "Skipping invalid MATE archive: %s", Path.c_str());
				return false;
			}

			Archive.Initialize(Path, &SceneFile);
			return true;
		}

		void MergeHghtOverlay(application::file::game::terrain::HghtArchive& Base,
			const application::file::game::terrain::HghtArchive& Overlay)
		{
			for (const auto& [Name, File] : Overlay.mHeightMaps)
			{
				Base.mHeightMaps[Name] = File;
				Base.mHeightMaps[Name].mModified = false;
			}
		}

		void MergeMateOverlay(application::file::game::terrain::MateArchive& Base,
			const application::file::game::terrain::MateArchive& Overlay)
		{
			for (const auto& [Name, File] : Overlay.mMateFiles)
			{
				Base.mMateFiles[Name] = File;
				Base.mMateFiles[Name].mModified = false;
			}
		}

		void TryLoadArchivePack(const std::string& TerrainSceneName, const std::string& ArchiveKey,
			TerrainMgr::TerrainScene::ArchivePack& ArchivePack, application::file::game::terrain::TerrainSceneFile& SceneFile)
		{
			const std::string ArchiveBasePath = "TerrainArc/" + TerrainSceneName + "/" + ArchiveKey;
			const std::string OverlayHghtPath = application::util::FileUtil::GetRomFSFilePath(ArchiveBasePath + ".hght.ta.zs");
			const std::string OverlayMatePath = application::util::FileUtil::GetRomFSFilePath(ArchiveBasePath + ".mate.ta.zs");
			const std::string VanillaHghtPath = application::util::FileUtil::GetRomFSFilePath(ArchiveBasePath + ".hght.ta.zs", false);
			const std::string VanillaMatePath = application::util::FileUtil::GetRomFSFilePath(ArchiveBasePath + ".mate.ta.zs", false);

			application::file::game::terrain::HghtArchive OverlayHght;
			const bool HasHghtOverlay = OverlayHghtPath != VanillaHghtPath
				&& TryInitializeHghtArchive(OverlayHghtPath, OverlayHght);

			if (TryInitializeHghtArchive(VanillaHghtPath, ArchivePack.mHghtArchive))
			{
				if (HasHghtOverlay)
					MergeHghtOverlay(ArchivePack.mHghtArchive, OverlayHght);
			}
			else if (HasHghtOverlay)
			{
				ArchivePack.mHghtArchive = std::move(OverlayHght);
			}

			application::file::game::terrain::MateArchive OverlayMate;
			const bool HasMateOverlay = OverlayMatePath != VanillaMatePath
				&& TryInitializeMateArchive(OverlayMatePath, OverlayMate, SceneFile);

			if (TryInitializeMateArchive(VanillaMatePath, ArchivePack.mMateArchive, SceneFile))
			{
				if (HasMateOverlay)
					MergeMateOverlay(ArchivePack.mMateArchive, OverlayMate);
			}
			else if (HasMateOverlay)
			{
				ArchivePack.mMateArchive = std::move(OverlayMate);
			}
		}
	}

	std::unordered_map<std::string, TerrainMgr::TerrainScene> TerrainMgr::gTerrainScenes;
    std::unordered_map<std::string, application::gl::TerrainRenderer> TerrainMgr::gTerrainRenderers;

	TerrainMgr::TerrainScene* TerrainMgr::GetTerrainScene(const std::string& TerrainSceneName)
	{
		if (!gTerrainScenes.contains(TerrainSceneName))
		{
			gTerrainScenes[TerrainSceneName].mTerrainSceneFile.Initialize(application::util::FileUtil::ReadFile(application::util::FileUtil::GetRomFSFilePath("TerrainArc/" + TerrainSceneName + ".tscb")));
		}

		return &gTerrainScenes[TerrainSceneName];
	}

    bool TerrainMgr::TryGetArchivePackKey(const std::string& HghtName, std::string& OutKey)
    {
        if (HghtName.empty())
            return false;

        try
        {
            const uint64_t NameInt = static_cast<uint64_t>(std::floor(std::stoull(HghtName, nullptr, 16) / 4) * 4);

            std::stringstream Stream;
            Stream << std::setfill('0') << std::setw(16) << std::uppercase
                << std::hex << NameInt;
            std::string Name = Stream.str();
            if (Name.length() < 9)
                return false;

            OutKey = Name.substr(Name.length() - 9);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    TerrainMgr::TerrainScene::ArchivePack* TerrainMgr::GetArchivePack(const std::string& Name, TerrainMgr::TerrainScene& Scene, const std::string& TerrainSceneName)
    {
        std::string ArchiveKey;
        if (!TryGetArchivePackKey(Name, ArchiveKey))
        {
            application::util::Logger::Error("TerrainMgr", "Failed to parse archive key from HGHT name: %s", Name.c_str());
            return nullptr;
        }

        if (TerrainSceneName.empty())
        {
            application::util::Logger::Error("TerrainMgr", "Cannot load archive pack %s without a terrain scene name", ArchiveKey.c_str());
            return nullptr;
        }

        if (!Scene.mArchives.contains(ArchiveKey))
        {
            TryLoadArchivePack(TerrainSceneName, ArchiveKey, Scene.mArchives[ArchiveKey], Scene.mTerrainSceneFile);
        }
        return &Scene.mArchives[ArchiveKey];
    }

    bool TerrainMgr::PrepareArchivePackForSave(const std::string& TerrainSceneName, const std::string& ArchiveKey, TerrainMgr::TerrainScene::ArchivePack& ArchivePack, application::file::game::terrain::TerrainSceneFile& SceneFile)
    {
        const std::string ArchiveBasePath = "TerrainArc/" + TerrainSceneName + "/" + ArchiveKey;

        bool HasModifiedHght = false;
        for (const auto& [Name, File] : ArchivePack.mHghtArchive.mHeightMaps)
        {
            if (File.mModified)
            {
                HasModifiedHght = true;
                break;
            }
        }

        bool HasModifiedMate = false;
        for (const auto& [Name, File] : ArchivePack.mMateArchive.mMateFiles)
        {
            if (File.mModified)
            {
                HasModifiedMate = true;
                break;
            }
        }

        const std::string VanillaHghtPath = application::util::FileUtil::GetRomFSFilePath(
            ArchiveBasePath + ".hght.ta.zs", false);
        if (HasModifiedHght)
        {
            if (!application::util::FileUtil::FileExists(VanillaHghtPath))
            {
                application::util::Logger::Error("TerrainMgr",
                    "Cannot save HGHT archive %s: vanilla file missing at %s. Set RomFS to the 1.2.1 dump.",
                    ArchiveKey.c_str(), VanillaHghtPath.c_str());
                return false;
            }

            application::file::game::terrain::HghtArchive BaseHght;
            BaseHght.Initialize(VanillaHghtPath);
            for (auto& [Name, File] : ArchivePack.mHghtArchive.mHeightMaps)
            {
                if (File.mModified)
                    BaseHght.mHeightMaps[Name] = std::move(File);
            }
            ArchivePack.mHghtArchive = std::move(BaseHght);
        }

        const std::string VanillaMatePath = application::util::FileUtil::GetRomFSFilePath(
            ArchiveBasePath + ".mate.ta.zs", false);
        if (HasModifiedMate)
        {
            if (!application::util::FileUtil::FileExists(VanillaMatePath))
            {
                application::util::Logger::Error("TerrainMgr",
                    "Cannot save MATE archive %s: vanilla file missing at %s. Set RomFS to the 1.2.1 dump.",
                    ArchiveKey.c_str(), VanillaMatePath.c_str());
                return false;
            }

            application::file::game::terrain::MateArchive BaseMate;
            BaseMate.Initialize(VanillaMatePath, &SceneFile);
            for (auto& [Name, File] : ArchivePack.mMateArchive.mMateFiles)
            {
                if (File.mModified)
                    BaseMate.mMateFiles[Name] = std::move(File);
            }
            ArchivePack.mMateArchive = std::move(BaseMate);
        }

        return true;
    }

	application::gl::TerrainRenderer* TerrainMgr::GetTerrainRenderer(const std::string& SceneName, const std::string& SectionName)
    {
        std::string Key = SceneName + "_" + SectionName;
        if (!gTerrainRenderers.contains(Key))
        {
            gTerrainRenderers[Key].LoadData(SceneName, SectionName);
        }
        return &gTerrainRenderers[Key];
	}

    std::string TerrainMgr::GenerateHghtNameForArea(application::manager::TerrainMgr::TerrainScene* Scene, application::file::game::terrain::TerrainSceneFile::ResArea* Area)
    {
        float LODLevel = Scene->mTerrainSceneFile.mTerrainScene.mAreas[0].mObj.mScale;
        int LODCount = 0;
        while (LODLevel != Area->mScale)
        {
            LODLevel = LODLevel / 2.0f;
            LODCount++;
        }

        unsigned int Mask = 0;
        unsigned int Shift = 0;

        application::file::game::terrain::TerrainSceneFile::ResArea* CurrentArea = Area;
        while (CurrentArea != nullptr)
        {
            float HigherLOD = CurrentArea->mScale * 2.0f;
            if (HigherLOD != Scene->mTerrainSceneFile.mTerrainScene.mAreas[0].mObj.mScale)
            {
                glm::vec2 MiddlePointCurrent(CurrentArea->mX * 1000.0f * 0.5f, CurrentArea->mZ * 1000.0f * 0.5f);

                application::file::game::terrain::TerrainSceneFile::ResArea* ParentArea = nullptr;

                for (auto& PossibleParentAreaPtr : Scene->mTerrainSceneFile.mTerrainScene.mAreas)
                {
                    application::file::game::terrain::TerrainSceneFile::ResArea& PossibleParentArea = PossibleParentAreaPtr.mObj;

                    if (PossibleParentArea.mScale != HigherLOD)
                        continue;

                    const float TileSectionWidthHalf = 250.0f * PossibleParentArea.mScale;
                    glm::vec2 MiddlePointParent(PossibleParentArea.mX * 1000.0f * 0.5f, PossibleParentArea.mZ * 1000.0f * 0.5f);

                    if (MiddlePointCurrent.x > MiddlePointParent.x - TileSectionWidthHalf && MiddlePointCurrent.x < MiddlePointParent.x + TileSectionWidthHalf &&
                        MiddlePointCurrent.y > MiddlePointParent.y - TileSectionWidthHalf && MiddlePointCurrent.y < MiddlePointParent.y + TileSectionWidthHalf)
                    {
                        ParentArea = &PossibleParentArea;
                        break;
                    }
                }

                if (ParentArea == nullptr)
                {
                    application::util::Logger::Error("TerrainMgr", "Failed to generate HGHT name, ParentArea was a nullptr");
                    break;
                }

                glm::vec2 MiddlePointParent(ParentArea->mX * 1000.0f * 0.5f, ParentArea->mZ * 1000.0f * 0.5f);

                enum AreaPosType
                {
                    TopLeft = 0,
                    TopRight = 1,
                    BottomLeft = 2,
                    BottomRight = 3
                };

                AreaPosType Type = AreaPosType::TopLeft;
                bool Left = MiddlePointCurrent.x < MiddlePointParent.x;
                bool Top = MiddlePointCurrent.y < MiddlePointParent.y;

                if (Top && Left) Type = AreaPosType::TopLeft;
                else if (!Top && Left) Type = AreaPosType::BottomLeft;
                else if (!Top && !Left) Type = AreaPosType::BottomRight;
                else if (Top && !Left) Type = AreaPosType::TopRight;

                Mask |= (Type << Shift);
                Shift += 2;

                CurrentArea = ParentArea;
            }
            else
            {
                Mask |= (0 << Shift);
                break;
            }
        }

        std::string Filename;
        Filename.resize(10);
        snprintf(&Filename[0], 11, "%X%X%08X", (int)log2f(Scene->mTerrainSceneFile.mTerrainScene.mAreaScale), LODCount, Mask);

        return Filename;
    }
}
