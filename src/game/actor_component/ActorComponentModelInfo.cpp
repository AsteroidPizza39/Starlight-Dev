#include "ActorComponentModelInfo.h"

#include <file/game/byml/BymlFile.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <filesystem>

namespace application::game::actor_component
{
	ActorComponentModelInfo::ActorComponentModelInfo(application::game::ActorPack& ActorPack) : mActorPack(&ActorPack)
	{
		std::string ModelInfoPath = "";
		for (application::file::game::SarcFile::Entry& Entry : ActorPack.mPack.GetEntries())
		{
			if (Entry.mName.rfind("Component/ModelInfo/", 0) == 0)
			{
				ModelInfoPath = Entry.mName;
			}
		}

		application::file::game::byml::BymlFile ModelInfoByml(ActorPack.mPack.GetEntry(ModelInfoPath).mBytes);
		if (ModelInfoByml.GetNodes().empty())
		{
			return;
		}

		if (ModelInfoByml.HasChild("ModelProjectName")) mModelProjectName = ModelInfoByml.GetNode("ModelProjectName")->GetValue<std::string>();
		if (ModelInfoByml.HasChild("FmdbName")) mFmdbName = ModelInfoByml.GetNode("FmdbName")->GetValue<std::string>();
		if (ModelInfoByml.HasChild("EnableModelBake")) mEnableModelBake = ModelInfoByml.GetNode("EnableModelBake")->GetValue<bool>();
		if (ModelInfoByml.HasChild("ModelVariationFmabName")) mModelVariationFmabName = ModelInfoByml.GetNode("ModelVariationFmabName")->GetValue<std::string>();
		if (ModelInfoByml.HasChild("ModelVariationFmabFrame")) mModelVariationFmabFrame = ModelInfoByml.GetNode("ModelVariationFmabFrame")->GetValue<float>();
		if (ModelInfoByml.HasChild("ModelVariationAnims"))
		{
			application::file::game::byml::BymlFile::Node* Anims = ModelInfoByml.GetNode("ModelVariationAnims");
			for (application::file::game::byml::BymlFile::Node& AnimNode : Anims->GetChildren())
			{
				if (AnimNode.GetType() != application::file::game::byml::BymlFile::Type::Dictionary)
				{
					continue;
				}

				if (AnimNode.HasChild("Fmab"))
				{
					const std::string FmabPath = AnimNode.GetChild("Fmab")->GetValue<std::string>();
					mModelVariationFmabPath = FmabPath;
					const std::string FmabName = std::filesystem::path(FmabPath).stem().string();
					if (!FmabName.empty())
					{
						mModelVariationFmabName = FmabName;
					}
				}

				if (AnimNode.HasChild("Frame"))
				{
					mModelVariationFmabFrame = AnimNode.GetChild("Frame")->GetValue<float>();
				}

				// Use first declared variation animation (game-side behavior for this field).
				break;
			}
		}
	}

	const std::string ActorComponentModelInfo::GetDisplayNameImpl()
	{
		return "ModelInfo";
	}

	const std::string ActorComponentModelInfo::GetInternalNameImpl()
	{
		return "engine__component__ModelInfo";
	}

	bool ActorComponentModelInfo::ContainsComponent(application::game::ActorPack& Pack)
	{
		for (application::file::game::SarcFile::Entry& Entry : Pack.mPack.GetEntries())
		{
			if (Entry.mName.rfind("Component/ModelInfo/", 0) == 0)
			{
				return true;
			}
		}

		return false;
	}

	void ActorComponentModelInfo::DrawEditingMenu()
	{
		if(mModelProjectName.has_value()) ImGui::InputText("ModelProjectName", &mModelProjectName.value());
		if(mFmdbName.has_value()) ImGui::InputText("FmdbName", &mFmdbName.value());
		if(mEnableModelBake.has_value()) ImGui::Checkbox("Enable Model Bake", &mEnableModelBake.value());
		if(mModelVariationFmabName.has_value()) ImGui::InputText("ModelVariationFmabName", &mModelVariationFmabName.value());
		if(mModelVariationFmabFrame.has_value()) ImGui::InputFloat("ModelVariationFmabFrame", &mModelVariationFmabFrame.value());
		if(mModelVariationFmabPath.has_value()) ImGui::InputText("ModelVariationFmabPath", &mModelVariationFmabPath.value());
	}
}