#include "BancEntity.h"

#include <util/Math.h>
#include <util/FileUtil.h>
#include <manager/ActorPackMgr.h>
#include <manager/BfresFileMgr.h>
#include <manager/BfresRendererMgr.h>
#include <manager/MergedActorMgr.h>
#include <game/actor_component/ActorComponentModelInfo.h>
#include <file/game/zstd/ZStdBackend.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <cstring>
#include <unordered_set>

namespace application::game
{
	void BancEntity::NotifyRotateEditedByUser()
	{
		mRotateUserEdited = true;
	}

	void BancEntity::SyncRotateInspectorEulerFromRadians()
	{
		mRotateInspectorEulerDeg = glm::degrees(mRotate);
	}

	void BancEntity::InitializeRotationPersistenceFromCurrent()
	{
		mRotateSerializedRadiansSnap = mRotate;
		mRotateUserEdited = false;
		SyncRotateInspectorEulerFromRadians();
	}

	namespace
	{
		std::string ToLowerCopy(const std::string& Value)
		{
			std::string Lower = Value;
			std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C)
				{
					return static_cast<char>(std::tolower(C));
				});
			return Lower;
		}

		std::string TrimAsciiCopy(const std::string& Value)
		{
			if (Value.empty())
			{
				return Value;
			}

			size_t Start = 0;
			while (Start < Value.size() && std::isspace(static_cast<unsigned char>(Value[Start])) != 0)
			{
				Start++;
			}

			if (Start == Value.size())
			{
				return "";
			}

			size_t End = Value.size();
			while (End > Start && std::isspace(static_cast<unsigned char>(Value[End - 1])) != 0)
			{
				End--;
			}

			return Value.substr(Start, End - Start);
		}

		std::string NormalizeBoneNameForLookup(const std::string& Value)
		{
			return ToLowerCopy(TrimAsciiCopy(Value));
		}

		bool ContainsInsensitive(const std::string& Value, const std::string& Needle)
		{
			return ToLowerCopy(Value).find(ToLowerCopy(Needle)) != std::string::npos;
		}

		bool IsBymlEntry(const application::file::game::SarcFile::Entry& Entry)
		{
			return Entry.mBytes.size() >= 2 && ((Entry.mBytes[0] == 'Y' && Entry.mBytes[1] == 'B') || (Entry.mBytes[0] == 'B' && Entry.mBytes[1] == 'Y'));
		}

		bool IsBymlBytes(const std::vector<unsigned char>& Bytes)
		{
			return Bytes.size() >= 2 && ((Bytes[0] == 'Y' && Bytes[1] == 'B') || (Bytes[0] == 'B' && Bytes[1] == 'Y'));
		}

		bool IsLikelyZStd(const std::vector<unsigned char>& Bytes)
		{
			return Bytes.size() >= 4 && Bytes[0] == 0x28 && Bytes[1] == 0xB5 && Bytes[2] == 0x2F && Bytes[3] == 0xFD;
		}

		bool LooksLikeModelPath(const std::string& Value)
		{
			return ContainsInsensitive(Value, ".bfres") || ContainsInsensitive(Value, ".mc") || ContainsInsensitive(Value, ".fmdb");
		}

		std::string NormalizePath(std::string Path)
		{
			std::replace(Path.begin(), Path.end(), '\\', '/');
			return Path;
		}

		std::string ConvertWorkFmdbPathToBfresName(const std::string& Path)
		{
			std::string Normalized = NormalizePath(Path);
			if (Normalized.empty())
			{
				return "";
			}

			if (ContainsInsensitive(Normalized, ".bfres") || ContainsInsensitive(Normalized, ".mc"))
			{
				return Normalized;
			}

			if (!ContainsInsensitive(Normalized, ".fmdb"))
			{
				return "";
			}

			if (ContainsInsensitive(Normalized, "work/"))
			{
				Normalized = Normalized.substr(Normalized.find("Work/") + 5);
			}

			const std::string OutputMarker = "/output/";
			const size_t OutputPos = ToLowerCopy(Normalized).find(ToLowerCopy(OutputMarker));
			if (OutputPos != std::string::npos)
			{
				const std::string ProjectPath = Normalized.substr(0, OutputPos);
				const std::string ProjectName = std::filesystem::path(ProjectPath).filename().string();
				const std::string FmdbName = std::filesystem::path(Normalized).stem().string();
				if (!ProjectName.empty() && !FmdbName.empty())
				{
					return ProjectName + "." + FmdbName + ".bfres";
				}
			}

			return std::filesystem::path(Normalized).stem().string() + ".bfres";
		}

		void CollectHornPathCandidates(application::file::game::byml::BymlFile::Node& Node, std::vector<std::string>& Candidates)
		{
			if (Node.GetType() == application::file::game::byml::BymlFile::Type::Dictionary)
			{
				if (Node.HasChild("ModelProjectName") && Node.HasChild("FmdbName"))
				{
					const std::string ProjectName = Node.GetChild("ModelProjectName")->GetValue<std::string>();
					const std::string FmdbName = Node.GetChild("FmdbName")->GetValue<std::string>();
					if (!ProjectName.empty() && !FmdbName.empty())
					{
						Candidates.push_back("Model/" + ProjectName + "." + FmdbName + ".bfres");
					}
				}

				if (Node.HasChild("FilePath"))
				{
					const std::string FilePath = Node.GetChild("FilePath")->GetValue<std::string>();
					if (LooksLikeModelPath(FilePath))
					{
						Candidates.push_back(ConvertWorkFmdbPathToBfresName(FilePath));
					}
				}

				if (Node.HasChild("HornModelPath"))
				{
					const std::string FilePath = Node.GetChild("HornModelPath")->GetValue<std::string>();
					if (LooksLikeModelPath(FilePath))
					{
						Candidates.push_back(ConvertWorkFmdbPathToBfresName(FilePath));
					}
				}
			}

			if (Node.GetType() == application::file::game::byml::BymlFile::Type::StringIndex)
			{
				const std::string Value = Node.GetValue<std::string>();
				if (LooksLikeModelPath(Value))
				{
					Candidates.push_back(ConvertWorkFmdbPathToBfresName(Value));
				}
			}

			for (application::file::game::byml::BymlFile::Node& Child : Node.GetChildren())
			{
				CollectHornPathCandidates(Child, Candidates);
			}
		}

		std::optional<std::string> FindHornTypeToken(const application::game::BancEntity& Entity)
		{
			const std::array<const std::map<std::string, std::variant<std::string, bool, int32_t, int64_t, uint32_t, uint64_t, float, glm::vec3>>*, 3> Sources =
			{
				&Entity.mDynamic,
				&Entity.mExternalParameter,
				&Entity.mPresence
			};

			for (const auto* Source : Sources)
			{
				for (const auto& [Key, Value] : *Source)
				{
					if (!std::holds_alternative<std::string>(Value))
					{
						continue;
					}

					const std::string& StringValue = std::get<std::string>(Value);
					if (StringValue.empty())
					{
						continue;
					}

					if (ContainsInsensitive(Key, "horn") && ContainsInsensitive(Key, "type"))
					{
						return StringValue;
					}
				}
			}

			return std::nullopt;
		}

		std::vector<std::string> SplitActorNameTokens(const std::string& Name)
		{
			std::vector<std::string> Tokens;
			std::string CurrentToken;
			for (char Ch : Name)
			{
				if (std::isalnum(static_cast<unsigned char>(Ch)))
				{
					CurrentToken.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(Ch))));
				}
				else if (!CurrentToken.empty())
				{
					Tokens.push_back(CurrentToken);
					CurrentToken.clear();
				}
			}
			if (!CurrentToken.empty())
			{
				Tokens.push_back(CurrentToken);
			}
			return Tokens;
		}

		std::string GetActorTierToken(const std::string& Gyml)
		{
			const size_t LastUnderscore = Gyml.find_last_of('_');
			if (LastUnderscore == std::string::npos || LastUnderscore + 1 >= Gyml.length())
			{
				return "";
			}
			return ToLowerCopy(Gyml.substr(LastUnderscore + 1));
		}

		int32_t ScoreTextForActorTokens(const std::string& Text, const std::string& Gyml)
		{
			const std::string LowerText = ToLowerCopy(Text);
			int32_t Score = 0;
			for (const std::string& Token : SplitActorNameTokens(Gyml))
			{
				if (Token.length() < 3)
				{
					continue;
				}

				if (LowerText.find(Token) != std::string::npos)
				{
					Score += 1;
				}
			}
			return Score;
		}

		std::string NormalizeWorkGymlPathToBymlPath(std::string Path)
		{
			Path = NormalizePath(Path);
			if (Path.starts_with("?"))
			{
				Path.erase(Path.begin());
			}

			const std::string Lower = ToLowerCopy(Path);
			if (Lower.starts_with("work/"))
			{
				Path = Path.substr(5);
			}

			if (ContainsInsensitive(Path, ".gyml"))
			{
				const size_t DotPos = ToLowerCopy(Path).rfind(".gyml");
				if (DotPos != std::string::npos)
				{
					Path = Path.substr(0, DotPos) + ".bgyml";
				}
			}

			return Path;
		}

		std::string NormalizeActorReferenceToGymlName(std::string Value)
		{
			Value = NormalizePath(Value);
			if (Value.starts_with("?"))
			{
				Value.erase(Value.begin());
			}

			if (ContainsInsensitive(Value, "/"))
			{
				Value = std::filesystem::path(Value).stem().string();
			}
			if (ContainsInsensitive(Value, ".engine__actor__actorparam"))
			{
				const size_t Pos = ToLowerCopy(Value).find(".engine__actor__actorparam");
				if (Pos != std::string::npos)
				{
					Value = Value.substr(0, Pos);
				}
			}
			return Value;
		}

		bool TryReadBymlBytesFromSarc(application::file::game::SarcFile& Pack, const std::string& RelativePath, std::vector<unsigned char>& OutBytes)
		{
			const std::string Normalized = NormalizePath(RelativePath);
			const std::array<std::string, 2> CandidatePaths = { Normalized, Normalized + ".zs" };
			for (const std::string& Candidate : CandidatePaths)
			{
				if (!Pack.HasEntry(Candidate))
				{
					continue;
				}

				OutBytes = Pack.GetEntry(Candidate).mBytes;
				if (!IsBymlBytes(OutBytes) && IsLikelyZStd(OutBytes))
				{
					OutBytes = application::file::game::ZStdBackend::Decompress(OutBytes);
				}
				if (IsBymlBytes(OutBytes))
				{
					return true;
				}
			}

			return false;
		}

		bool TryReadBymlBytesFromDump(const std::string& RelativePath, std::vector<unsigned char>& OutBytes)
		{
			const std::string Normalized = NormalizePath(RelativePath);
			const std::array<std::string, 2> CandidatePaths = { Normalized, Normalized + ".zs" };
			for (const std::string& Candidate : CandidatePaths)
			{
				const std::string AbsolutePath = application::util::FileUtil::GetRomFSFilePath(Candidate);
				if (!application::util::FileUtil::FileExists(AbsolutePath))
				{
					continue;
				}

				OutBytes = application::util::FileUtil::ReadFile(AbsolutePath);
				if (!IsBymlBytes(OutBytes) && IsLikelyZStd(OutBytes))
				{
					OutBytes = application::file::game::ZStdBackend::Decompress(OutBytes);
				}
				if (IsBymlBytes(OutBytes))
				{
					return true;
				}
			}

			return false;
		}

		std::vector<application::file::game::byml::BymlFile> ResolveBymlInheritanceChain(application::game::ActorPack& Pack, const std::string& StartPath)
		{
			std::vector<application::file::game::byml::BymlFile> Chain;
			if (StartPath.empty())
			{
				return Chain;
			}

			std::unordered_set<std::string> Visited;
			std::string CurrentPath = NormalizeWorkGymlPathToBymlPath(StartPath);
			while (!CurrentPath.empty() && !Visited.contains(CurrentPath))
			{
				Visited.insert(CurrentPath);

				std::vector<unsigned char> Bytes;
				bool Loaded = TryReadBymlBytesFromSarc(Pack.mPack, CurrentPath, Bytes);
				if (!Loaded)
				{
					Loaded = TryReadBymlBytesFromDump(CurrentPath, Bytes);
				}
				if (!Loaded)
				{
					break;
				}

				application::file::game::byml::BymlFile Byml(Bytes);
				Chain.push_back(Byml);
				if (!Byml.HasChild("$parent"))
				{
					break;
				}

				CurrentPath = NormalizeWorkGymlPathToBymlPath(Byml.GetNode("$parent")->GetValue<std::string>());
			}

			return Chain;
		}

		std::optional<std::string> FindFirstNonEmptyStringInChain(std::vector<application::file::game::byml::BymlFile>& Chain, const std::string& Key)
		{
			for (application::file::game::byml::BymlFile& Node : Chain)
			{
				if (!Node.HasChild(Key))
				{
					continue;
				}

				const std::string Value = Node.GetNode(Key)->GetValue<std::string>();
				if (!Value.empty())
				{
					return Value;
				}
			}
			return std::nullopt;
		}

		std::optional<float> FindFirstNumericInChain(std::vector<application::file::game::byml::BymlFile>& Chain, const std::string& Key)
		{
			for (application::file::game::byml::BymlFile& Node : Chain)
			{
				if (!Node.HasChild(Key))
				{
					continue;
				}

				application::file::game::byml::BymlFile::Node* ValueNode = Node.GetNode(Key);
				switch (ValueNode->GetType())
				{
				case application::file::game::byml::BymlFile::Type::Float:
					return ValueNode->GetValue<float>();
				case application::file::game::byml::BymlFile::Type::Int32:
					return static_cast<float>(ValueNode->GetValue<int32_t>());
				case application::file::game::byml::BymlFile::Type::UInt32:
					return static_cast<float>(ValueNode->GetValue<uint32_t>());
				case application::file::game::byml::BymlFile::Type::Int64:
					return static_cast<float>(ValueNode->GetValue<int64_t>());
				case application::file::game::byml::BymlFile::Type::UInt64:
					return static_cast<float>(ValueNode->GetValue<uint64_t>());
				default:
					break;
				}
			}

			return std::nullopt;
		}

		std::unordered_map<std::string, std::string> ResolveBlackboardStringInitValues(std::vector<application::file::game::byml::BymlFile>& Chain)
		{
			std::unordered_map<std::string, std::string> Values;
			for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
			{
				application::file::game::byml::BymlFile& Byml = *It;
				if (!Byml.HasChild("BlackboardParamStringArray"))
				{
					continue;
				}

				application::file::game::byml::BymlFile::Node* ArrayNode = Byml.GetNode("BlackboardParamStringArray");
				for (application::file::game::byml::BymlFile::Node& Item : ArrayNode->GetChildren())
				{
					if (!Item.HasChild("BBKey") || !Item.HasChild("InitValConverted"))
					{
						continue;
					}

					const std::string BBKey = Item.GetChild("BBKey")->GetValue<std::string>();
					const std::string InitValConverted = Item.GetChild("InitValConverted")->GetValue<std::string>();
					Values[BBKey] = InitValConverted;
				}
			}
			return Values;
		}

		std::optional<std::string> FindActorRootParamBymlPath(application::game::ActorPack& Pack, const std::string& Gyml)
		{
			const std::string GymlLower = ToLowerCopy(Gyml);
			std::optional<std::string> BestPath = std::nullopt;
			for (const application::file::game::SarcFile::Entry& Entry : Pack.mPack.GetEntries())
			{
				const std::string LowerName = ToLowerCopy(Entry.mName);
				if (!LowerName.starts_with("actor/") || !LowerName.ends_with(".engine__actor__actorparam.bgyml"))
				{
					continue;
				}

				const std::string Stem = ToLowerCopy(std::filesystem::path(Entry.mName).stem().string());
				if (Stem.starts_with(GymlLower))
				{
					return Entry.mName;
				}

				if (!BestPath.has_value())
				{
					BestPath = Entry.mName;
				}
			}
			return BestPath;
		}

		application::gl::BfresRenderer* ResolveHornRenderer(const std::optional<std::string>& HornModelPath);

		application::gl::BfresRenderer* ResolveRendererFromActorGyml(const std::string& Gyml)
		{
			if (Gyml.empty())
			{
				return nullptr;
			}

			application::game::ActorPack* EquipmentPack = application::manager::ActorPackMgr::GetActorPack(Gyml);
			if (EquipmentPack == nullptr)
			{
				return nullptr;
			}

			if (application::game::actor_component::ActorComponentBase* BaseComponent = EquipmentPack->GetComponent(application::game::actor_component::ActorComponentBase::ComponentType::MODEL_INFO); BaseComponent != nullptr)
			{
				application::game::actor_component::ActorComponentModelInfo* ModelInfo = static_cast<application::game::actor_component::ActorComponentModelInfo*>(BaseComponent);
				if (ModelInfo != nullptr && ModelInfo->mModelProjectName.has_value() && ModelInfo->mFmdbName.has_value() &&
					!ModelInfo->mModelProjectName->empty() && !ModelInfo->mFmdbName->empty())
				{
					application::file::game::bfres::BfresFile* File = application::manager::BfresFileMgr::GetBfresFile(
						application::util::FileUtil::GetBfresFilePath(ModelInfo->mModelProjectName.value() + "." + ModelInfo->mFmdbName.value() + ".bfres"));
					application::gl::BfresRenderer* Renderer = application::manager::BfresRendererMgr::GetRenderer(File);
					if (Renderer != nullptr && !Renderer->mBfresFile->mDefaultModel)
					{
						return Renderer;
					}
				}
			}

			std::vector<std::string> FallbackModelPathCandidates;
			for (const application::file::game::SarcFile::Entry& Entry : EquipmentPack->mPack.GetEntries())
			{
				const std::string LowerName = ToLowerCopy(Entry.mName);
				if (!LowerName.starts_with("component/modelinfo/") || !LowerName.ends_with(".bgyml"))
				{
					continue;
				}

				std::vector<unsigned char> Bytes = Entry.mBytes;
				if (!IsBymlBytes(Bytes) && IsLikelyZStd(Bytes))
				{
					Bytes = application::file::game::ZStdBackend::Decompress(Bytes);
				}
				if (!IsBymlBytes(Bytes))
				{
					continue;
				}

				application::file::game::byml::BymlFile ModelInfoByml(Bytes);
				for (application::file::game::byml::BymlFile::Node& RootNode : ModelInfoByml.GetNodes())
				{
					CollectHornPathCandidates(RootNode, FallbackModelPathCandidates);
				}
			}

			for (const std::string& Candidate : FallbackModelPathCandidates)
			{
				application::gl::BfresRenderer* Renderer = ResolveHornRenderer(std::optional<std::string>(Candidate));
				if (Renderer != nullptr && !Renderer->mBfresFile->mDefaultModel)
				{
					return Renderer;
				}
			}

			return nullptr;
		}

		std::optional<std::string> FindEquipmentUserParamPathInPack(application::game::ActorPack& Pack, const std::string& Gyml)
		{
			const std::string GymlLower = ToLowerCopy(Gyml);
			const std::array<std::string, 9> TierTokens = { "junior", "middle", "senior", "dark", "fire", "ice", "electric", "curse", "gold" };
			std::optional<std::string> FamilyGymlLower = std::nullopt;
			const size_t LastUnderscore = GymlLower.find_last_of('_');
			if (LastUnderscore != std::string::npos && LastUnderscore + 1 < GymlLower.size())
			{
				const std::string TailToken = GymlLower.substr(LastUnderscore + 1);
				if (std::find(TierTokens.begin(), TierTokens.end(), TailToken) != TierTokens.end())
				{
					FamilyGymlLower = GymlLower.substr(0, LastUnderscore);
				}
			}

			int32_t BestScore = std::numeric_limits<int32_t>::min();
			std::optional<std::string> BestPath = std::nullopt;
			for (const application::file::game::SarcFile::Entry& Entry : Pack.mPack.GetEntries())
			{
				const std::string LowerName = ToLowerCopy(Entry.mName);
				if (!LowerName.starts_with("component/equipmentuserparam/") || !LowerName.ends_with(".bgyml"))
				{
					continue;
				}

				const std::string Stem = ToLowerCopy(std::filesystem::path(Entry.mName).stem().string());
				if (Stem.starts_with(GymlLower))
				{
					return Entry.mName;
				}
				if (FamilyGymlLower.has_value() && Stem.starts_with(FamilyGymlLower.value()))
				{
					return Entry.mName;
				}

				int32_t Score = 0;
				if (ContainsInsensitive(Stem, "enemycommon"))
				{
					Score -= 100;
				}
				for (const std::string& Token : SplitActorNameTokens(Gyml))
				{
					if (Token.length() < 4)
					{
						continue;
					}
					if (Stem.find(Token) != std::string::npos)
					{
						Score += 5;
					}
				}

				if (Score >= BestScore || !BestPath.has_value())
				{
					BestScore = Score;
					BestPath = Entry.mName;
				}
			}
			return BestPath;
		}

		application::file::game::bfres::BfresFile::Skeleton::Bone* FindBoneByName(
			application::file::game::bfres::BfresFile::Model& Model,
			const std::string& PreferredName,
			std::string* ResolvedBoneName = nullptr)
		{
			auto Normalize = [](const std::string& BoneName)
				{
					std::string Result = BoneName;
					Result.erase(std::remove_if(Result.begin(), Result.end(), [](unsigned char Ch) { return std::isspace(Ch) != 0; }), Result.end());
					std::replace(Result.begin(), Result.end(), '-', '_');
					return ToLowerCopy(Result);
				};
			const std::string PreferredNormalized = Normalize(PreferredName);
			for (auto& [BoneName, BoneNode] : Model.ModelSkeleton.Bones.mNodes)
			{
				if (Normalize(BoneName) == PreferredNormalized)
				{
					if (ResolvedBoneName != nullptr)
					{
						*ResolvedBoneName = BoneName;
					}
					return &BoneNode.mValue;
				}
			}
			for (auto& [BoneName, BoneNode] : Model.ModelSkeleton.Bones.mNodes)
			{
				if (Normalize(BoneName).find(PreferredNormalized) != std::string::npos)
				{
					if (ResolvedBoneName != nullptr)
					{
						*ResolvedBoneName = BoneName;
					}
					return &BoneNode.mValue;
				}
			}
			return nullptr;
		}

		glm::mat4 BuildAttachmentCorrectionMatrix(application::file::game::bfres::BfresFile::Model& EquipmentModel, const std::string& SourceBoneName)
		{
			uint32_t BakedBoneIndex = 0;
			for (const auto& [ShapeName, ShapeNode] : EquipmentModel.Shapes.mNodes)
			{
				if (ShapeNode.mValue.BoneIndex >= 0)
				{
					BakedBoneIndex = static_cast<uint32_t>(ShapeNode.mValue.BoneIndex);
					break;
				}
			}

			application::file::game::bfres::BfresFile::Skeleton::Bone& BakedBone = EquipmentModel.ModelSkeleton.Bones.GetByIndex(BakedBoneIndex).mValue;
			application::file::game::bfres::BfresFile::Skeleton::Bone* SourceBone = FindBoneByName(EquipmentModel, SourceBoneName);
			if (SourceBone == nullptr)
			{
				SourceBone = &BakedBone;
			}

			return SourceBone->WorldMatrix * glm::inverse(BakedBone.WorldMatrix);
		}

		std::optional<std::string> ResolveHornModelPath(application::game::ActorPack& Pack, const std::optional<std::string>& HornTypeToken, const std::string& Gyml)
		{
			struct MappingCandidate
			{
				std::string mPath;
				std::string mFmdbFileName;
			};

			struct MappingFileCandidate
			{
				std::string mEntryName;
				std::vector<unsigned char> mBytes;
			};

			std::vector<MappingCandidate> MappingCandidates;
			std::vector<std::string> FallbackCandidates;
			auto CollectMappingArray = [&MappingCandidates, &FallbackCandidates, &HornTypeToken](application::file::game::byml::BymlFile& Byml)
				{
					for (application::file::game::byml::BymlFile::Node& RootNode : Byml.GetNodes())
					{
						if (RootNode.HasChild("HornTypeAndAttachmentMapping"))
						{
							application::file::game::byml::BymlFile::Node* MappingArray = RootNode.GetChild("HornTypeAndAttachmentMapping");
							for (application::file::game::byml::BymlFile::Node& MappingNode : MappingArray->GetChildren())
							{
								if (MappingNode.GetType() != application::file::game::byml::BymlFile::Type::Dictionary || !MappingNode.HasChild("HornModelPath"))
								{
									continue;
								}

								if (HornTypeToken.has_value() && MappingNode.HasChild("HornType"))
								{
									const std::string MappingHornType = MappingNode.GetChild("HornType")->GetValue<std::string>();
									const bool TypeMatches = ContainsInsensitive(MappingHornType, HornTypeToken.value());
									const bool IsDefault = ContainsInsensitive(MappingHornType, "Default");
									if (!TypeMatches && !IsDefault)
									{
										continue;
									}
								}

								const std::string HornModelPath = MappingNode.GetChild("HornModelPath")->GetValue<std::string>();
								if (LooksLikeModelPath(HornModelPath))
								{
									const std::string Converted = ConvertWorkFmdbPathToBfresName(HornModelPath);
									if (!Converted.empty())
									{
										MappingCandidate Candidate;
										Candidate.mPath = Converted;
										Candidate.mFmdbFileName = std::filesystem::path(NormalizePath(HornModelPath)).stem().string();
										MappingCandidates.push_back(Candidate);
									}
								}
							}
						}

						CollectHornPathCandidates(RootNode, FallbackCandidates);
					}
				};

			std::vector<MappingFileCandidate> PackMappingFiles;
			auto AppendMappingFilesFromSarc = [&PackMappingFiles](application::file::game::SarcFile& SourceSarc)
				{
					for (const application::file::game::SarcFile::Entry& Entry : SourceSarc.GetEntries())
					{
						if (!ContainsInsensitive(Entry.mName, "Ecosystem/HornTypeAndAttachmentMappingTable"))
						{
							continue;
						}

						std::vector<unsigned char> MappingBytes = Entry.mBytes;
						if (!IsBymlBytes(MappingBytes) && IsLikelyZStd(MappingBytes))
						{
							MappingBytes = application::file::game::ZStdBackend::Decompress(MappingBytes);
						}

						if (!IsBymlBytes(MappingBytes))
						{
							continue;
						}

						PackMappingFiles.push_back({ Entry.mName, std::move(MappingBytes) });
					}
				};

			AppendMappingFilesFromSarc(Pack.mPack);

			// Certain bokoblin junior variants keep horn mapping data in ResidentCommon.
			// Include that source explicitly so horn model path resolution can succeed.
			if (ToLowerCopy(Gyml).starts_with("enemy_bokoblin_junior"))
			{
				const std::string ResidentCommonPath = application::util::FileUtil::GetRomFSFilePath("Pack/ResidentCommon.pack.zs");
				if (application::util::FileUtil::FileExists(ResidentCommonPath))
				{
					application::file::game::SarcFile ResidentCommonPack(application::file::game::ZStdBackend::Decompress(ResidentCommonPath));
					if (ResidentCommonPack.mLoaded)
					{
						AppendMappingFilesFromSarc(ResidentCommonPack);
					}
				}
			}

			if (!PackMappingFiles.empty())
			{
				// Pick mapping file by BGYML filename relative to current actor pack name.
				// Only this file should define the correct HornModelPath for the actor tier.
				int32_t BestScore = std::numeric_limits<int32_t>::min();
				MappingFileCandidate* BestFile = nullptr;
				for (MappingFileCandidate& FileCandidate : PackMappingFiles)
				{
					const std::string FileStem = std::filesystem::path(FileCandidate.mEntryName).stem().string();
					int32_t Score = ScoreTextForActorTokens(FileStem, Gyml) * 20;
					if (ToLowerCopy(FileStem) == ToLowerCopy(Gyml))
					{
						Score += 1000;
					}

					if (Score >= BestScore)
					{
						BestScore = Score;
						BestFile = &FileCandidate;
					}
				}

				if (BestFile != nullptr)
				{
					application::file::game::byml::BymlFile Byml(BestFile->mBytes);
					CollectMappingArray(Byml);
				}
			}

			if (MappingCandidates.empty())
			{
				static const std::array<std::string, 4> RomFsFallbackPaths =
				{
					"Ecosystem/HornTypeAndAttachmentMappingTable.bgyml",
					"Ecosystem/HornTypeAndAttachmentMappingTable.byml",
					"Ecosystem/HornTypeAndAttachmentMappingTable.bgyml.zs",
					"Ecosystem/HornTypeAndAttachmentMappingTable.byml.zs"
				};

				for (const std::string& FallbackPath : RomFsFallbackPaths)
				{
					const std::string AbsolutePath = application::util::FileUtil::GetRomFSFilePath(FallbackPath);
					if (!application::util::FileUtil::FileExists(AbsolutePath))
					{
						continue;
					}

					std::vector<unsigned char> MappingBytes = application::util::FileUtil::ReadFile(AbsolutePath);
					if (!IsBymlBytes(MappingBytes) && IsLikelyZStd(MappingBytes))
					{
						MappingBytes = application::file::game::ZStdBackend::Decompress(MappingBytes);
					}

					if (!IsBymlBytes(MappingBytes))
					{
						continue;
					}

					application::file::game::byml::BymlFile Byml(MappingBytes);
					CollectMappingArray(Byml);
				}
			}

			MappingCandidates.erase(std::remove_if(MappingCandidates.begin(), MappingCandidates.end(), [](const MappingCandidate& Candidate)
				{
					return Candidate.mPath.empty();
				}), MappingCandidates.end());

			FallbackCandidates.erase(std::remove_if(FallbackCandidates.begin(), FallbackCandidates.end(), [](const std::string& Candidate)
				{
					return Candidate.empty();
				}), FallbackCandidates.end());

			if (!MappingCandidates.empty())
			{
				// Prefer entries that match the current actor name, while still
				// keeping "last entry wins" behavior when scores tie.
				int32_t BestScore = std::numeric_limits<int32_t>::min();
				std::string Selected = MappingCandidates.back().mPath;
				const std::string TierToken = GetActorTierToken(Gyml);
				for (const MappingCandidate& Candidate : MappingCandidates)
				{
					int32_t Score = ScoreTextForActorTokens(Candidate.mPath, Gyml);
					Score += ScoreTextForActorTokens(Candidate.mFmdbFileName, Gyml) * 10;
					if (!TierToken.empty())
					{
						const std::string LowerFmdb = ToLowerCopy(Candidate.mFmdbFileName);
						if (LowerFmdb.ends_with("_" + TierToken))
						{
							Score += 200;
						}
					}

					if (Score >= BestScore)
					{
						BestScore = Score;
						Selected = Candidate.mPath;
					}
				}
				return NormalizePath(Selected);
			}

			if (FallbackCandidates.empty())
			{
				return std::nullopt;
			}

			return NormalizePath(FallbackCandidates.back());
		}

		application::gl::BfresRenderer* ResolveHornRenderer(const std::optional<std::string>& HornModelPath)
		{
			if (!HornModelPath.has_value() || HornModelPath->empty())
			{
				return nullptr;
			}

			const std::string NormalizedPath = NormalizePath(HornModelPath.value());
			application::file::game::bfres::BfresFile* HornFile = nullptr;
			if (ContainsInsensitive(NormalizedPath, ".fmdb"))
			{
				HornFile = application::manager::BfresFileMgr::GetBfresFile(application::util::FileUtil::GetBfresFilePath(ConvertWorkFmdbPathToBfresName(NormalizedPath)));
			}
			else if (ContainsInsensitive(NormalizedPath, ".mc"))
			{
				std::string Stem = std::filesystem::path(NormalizedPath).stem().string();
				if (!ContainsInsensitive(Stem, ".bfres"))
				{
					Stem += ".bfres";
				}
				HornFile = application::manager::BfresFileMgr::GetBfresFile(application::util::FileUtil::GetBfresFilePath(Stem));
			}
			else if (ContainsInsensitive(NormalizedPath, ".bfres"))
			{
				if (NormalizedPath.find('/') != std::string::npos)
				{
					HornFile = application::manager::BfresFileMgr::GetBfresFile(application::util::FileUtil::GetRomFSFilePath(NormalizedPath));
				}
				else
				{
					HornFile = application::manager::BfresFileMgr::GetBfresFile(application::util::FileUtil::GetBfresFilePath(std::filesystem::path(NormalizedPath).stem().string() + ".bfres"));
				}
			}

			return HornFile != nullptr ? application::manager::BfresRendererMgr::GetRenderer(HornFile) : nullptr;
		}

		std::string GetMaterialNameHint(const std::string& MaterialName)
		{
			std::string Lower = ToLowerCopy(MaterialName);
			if (Lower.starts_with("mt_"))
			{
				Lower = Lower.substr(3);
			}
			return Lower;
		}

		std::vector<std::string> GetMaterialSearchTokens(const std::string& MaterialName)
		{
			std::vector<std::string> Tokens;
			const std::string Base = GetMaterialNameHint(MaterialName);
			if (!Base.empty())
			{
				Tokens.push_back(Base);
			}

			return Tokens;
		}

		bool IsLikelyAlbedoName(const std::string& TextureName)
		{
			const std::string Lower = ToLowerCopy(TextureName);
			return Lower.find("alb") != std::string::npos || Lower.find("albedo") != std::string::npos || Lower.find("basecolor") != std::string::npos;
		}

		std::optional<std::unordered_map<std::string, std::string>> ResolveTexturePatternAlbedoOverrides(
			const std::string& ModelProjectName,
			const std::string& AnimationName,
			const int32_t Frame)
		{
			if (ModelProjectName.empty() || AnimationName.empty() || Frame < 0)
			{
				return std::nullopt;
			}

			std::string AnimPath = application::util::FileUtil::GetRomFSFilePath("Model/" + ModelProjectName + ".anim.bfres.zs");
			if (!application::util::FileUtil::FileExists(AnimPath))
			{
				AnimPath = application::util::FileUtil::GetRomFSFilePath("Model/" + ModelProjectName + ".anim.bfres");
			}
			if (!application::util::FileUtil::FileExists(AnimPath))
			{
				return std::nullopt;
			}

			std::vector<unsigned char> AnimBytes = application::util::FileUtil::ReadFile(AnimPath);
			if (AnimBytes.empty())
			{
				return std::nullopt;
			}
			if (AnimPath.ends_with(".zs"))
			{
				AnimBytes = application::file::game::ZStdBackend::Decompress(AnimBytes);
			}
			if (AnimBytes.size() < 0x100)
			{
				return std::nullopt;
			}

			application::file::game::bfres::BfresBinaryVectorReader Reader(AnimBytes, application::file::game::bfres::BfresFile::gExternalBinaryStrings);
			application::file::game::bfres::BfresFile::BinaryHeader BinHeader;
			application::file::game::bfres::BfresFile::ResHeader Header;
			Reader.ReadStruct(&BinHeader, sizeof(BinHeader));
			Reader.ReadStruct(&Header, sizeof(Header));
			if (Header.MaterialAnimOffset == 0 || Header.MaterialAnimDictionarymOffset == 0 || Header.MaterialAnimCount == 0)
			{
				return std::nullopt;
			}

			using ResString = application::file::game::bfres::BfresFile::ResString;
			using ResDictString = application::file::game::bfres::BfresFile::ResDict<ResString>;
			ResDictString MaterialAnimDict = application::file::game::bfres::BfresFile::ReadDictionary<ResString>(Reader, Header.MaterialAnimDictionarymOffset);
			if (MaterialAnimDict.mNodes.empty())
			{
				return std::nullopt;
			}

			auto SafeReadString = [&](uint64_t Offset) -> std::string
				{
					if (Offset == 0 || Offset + 2 > AnimBytes.size())
					{
						return "";
					}
					return Reader.ReadStringOffset(Offset);
				};

			auto ReadArrayQWords = [&](uint64_t Offset, uint32_t Count) -> std::vector<uint64_t>
				{
					std::vector<uint64_t> Result;
					if (Offset == 0 || Count == 0)
					{
						return Result;
					}
					if (Offset + static_cast<uint64_t>(Count) * sizeof(uint64_t) > AnimBytes.size())
					{
						return Result;
					}
					Result.resize(Count);
					std::memcpy(Result.data(), AnimBytes.data() + Offset, static_cast<size_t>(Count) * sizeof(uint64_t));
					return Result;
				};

			auto ReadU32 = [&](uint64_t Offset) -> uint32_t
				{
					if (Offset + sizeof(uint32_t) > AnimBytes.size())
					{
						return 0;
					}
					uint32_t Value = 0;
					std::memcpy(&Value, AnimBytes.data() + Offset, sizeof(uint32_t));
					return Value;
				};

			auto ReadU64 = [&](uint64_t Offset) -> uint64_t
				{
					if (Offset + sizeof(uint64_t) > AnimBytes.size())
					{
						return 0;
					}
					uint64_t Value = 0;
					std::memcpy(&Value, AnimBytes.data() + Offset, sizeof(uint64_t));
					return Value;
				};

			auto IsSaneOffset = [&](uint64_t Offset) -> bool
				{
					return Offset > 0 && Offset < AnimBytes.size();
				};

			const std::string AnimationNameLower = ToLowerCopy(AnimationName);
			uint64_t MatAnimOffset = 0;
			for (uint32_t i = 0; i < MaterialAnimDict.mNodes.size(); i++)
			{
				const std::string Key = MaterialAnimDict.GetKey(i);
				if (ToLowerCopy(Key) == AnimationNameLower)
				{
					MatAnimOffset = Header.MaterialAnimOffset + static_cast<uint64_t>(i) * 0x70;
					break;
				}
			}
			if (MatAnimOffset == 0 || MatAnimOffset + 0x70 > AnimBytes.size())
			{
				return std::nullopt;
			}

			const uint32_t Magic = ReadU32(MatAnimOffset + 0x00);
			const uint64_t MatAnimNameOffset = ReadU64(MatAnimOffset + 0x08);
			const uint64_t BindDataOffset = ReadU64(MatAnimOffset + 0x20);
			const uint64_t TextureValueTableOffset = ReadU64(MatAnimOffset + 0x38);
			const uint64_t PackedCountsA = ReadU64(MatAnimOffset + 0x58);
			const uint64_t PackedCountsB = ReadU64(MatAnimOffset + 0x60);
			const uint64_t PackedCountsC = ReadU64(MatAnimOffset + 0x68);
			if (Magic != 0x41414D46 || BindDataOffset == 0 || TextureValueTableOffset == 0)
			{
				return std::nullopt;
			}

			const uint32_t MaterialCount = static_cast<uint32_t>((PackedCountsB >> 16) & 0xFFFF);
			const uint32_t TexturePatternCount = static_cast<uint32_t>(PackedCountsC & 0xFFFFFFFF);
			const uint32_t TextureValueCount = static_cast<uint32_t>((PackedCountsC >> 32) & 0xFFFFFFFF);
			if (MaterialCount == 0 || TextureValueCount == 0)
			{
				return std::nullopt;
			}

			const std::vector<uint64_t> TextureValueOffsets = ReadArrayQWords(TextureValueTableOffset, TextureValueCount);
			if (TextureValueOffsets.empty())
			{
				return std::nullopt;
			}

			std::vector<std::string> TextureValues;
			TextureValues.reserve(TextureValueOffsets.size());
			for (uint64_t Offset : TextureValueOffsets)
			{
				TextureValues.push_back(SafeReadString(Offset));
			}

			std::unordered_map<std::string, std::string> OverridesByMaterial;

			for (uint32_t MaterialIndex = 0; MaterialIndex < MaterialCount; MaterialIndex++)
			{
				const uint64_t TrackOffset = BindDataOffset + static_cast<uint64_t>(MaterialIndex) * 0x40;
				if (TrackOffset + 0x40 > AnimBytes.size())
				{
					continue;
				}

				struct TrackLayout
				{
					uint64_t mMaterialNameOffset = 0;
					uint64_t mSamplerNameArrayOffset = 0;
					uint64_t mCurveArrayOffset = 0;
					uint32_t mTexturePatternCount = 0;
					bool mValid = false;
				};

				auto IsCurveSlotValid = [&](uint64_t CurveSlotOffset) -> bool
					{
						if (!IsSaneOffset(CurveSlotOffset) || CurveSlotOffset + 0x30 > AnimBytes.size())
						{
							return false;
						}

						const uint64_t KeyFrameBufferOffset = ReadU64(CurveSlotOffset + 0x00);
						const uint64_t ValueBufferOffset = ReadU64(CurveSlotOffset + 0x08);
						const uint64_t PackedCurveTypeAndKeyCount = ReadU64(CurveSlotOffset + 0x10);
						const uint32_t KeyCount = static_cast<uint32_t>((PackedCurveTypeAndKeyCount >> 16) & 0xFFFF);
						if (KeyCount == 0 || KeyCount >= 512)
						{
							return false;
						}
						if (!IsSaneOffset(KeyFrameBufferOffset) || !IsSaneOffset(ValueBufferOffset))
						{
							return false;
						}
						return KeyFrameBufferOffset + KeyCount <= AnimBytes.size() &&
							ValueBufferOffset + KeyCount <= AnimBytes.size();
					};

				auto ResolveTrackLayout = [&]() -> TrackLayout
				{
					TrackLayout BestLayout;
					int32_t BestScore = -1;
					std::array<uint64_t, 8> RawFields{};
					for (uint32_t FieldIndex = 0; FieldIndex < RawFields.size(); FieldIndex++)
					{
						RawFields[FieldIndex] = ReadU64(TrackOffset + static_cast<uint64_t>(FieldIndex) * 8);
					}

					for (uint32_t NameIndex = 0; NameIndex < RawFields.size(); NameIndex++)
					{
						const uint64_t NameOffset = RawFields[NameIndex];
						const std::string MaterialNameCandidate = SafeReadString(NameOffset);
						if (MaterialNameCandidate.empty() || !(MaterialNameCandidate.starts_with("Mt_") || MaterialNameCandidate.starts_with("mt_")))
						{
							continue;
						}

						for (uint32_t SamplerIndexField = 0; SamplerIndexField < RawFields.size(); SamplerIndexField++)
						{
							if (SamplerIndexField == NameIndex)
							{
								continue;
							}

							const uint64_t SamplerArrayOffset = RawFields[SamplerIndexField];
							if (!IsSaneOffset(SamplerArrayOffset) || SamplerArrayOffset + sizeof(uint64_t) > AnimBytes.size())
							{
								continue;
							}

							const std::string FirstSamplerName = SafeReadString(ReadU64(SamplerArrayOffset));
							if (FirstSamplerName.empty() || FirstSamplerName[0] != '_')
							{
								continue;
							}

							for (uint32_t CurveIndexField = 0; CurveIndexField < RawFields.size(); CurveIndexField++)
							{
								if (CurveIndexField == NameIndex || CurveIndexField == SamplerIndexField)
								{
									continue;
								}

								const uint64_t CurveArrayOffset = RawFields[CurveIndexField];
								if (!IsCurveSlotValid(CurveArrayOffset))
								{
									continue;
								}

								uint32_t PatternCount = 0;
								for (uint32_t Probe = 0; Probe < 32; Probe++)
								{
									const uint64_t SamplerSlotOffset = SamplerArrayOffset + static_cast<uint64_t>(Probe) * sizeof(uint64_t);
									const uint64_t CurveSlotOffset = CurveArrayOffset + static_cast<uint64_t>(Probe) * 0x30;
									if (SamplerSlotOffset + sizeof(uint64_t) > AnimBytes.size() || CurveSlotOffset + 0x30 > AnimBytes.size())
									{
										break;
									}

									const std::string SamplerNameProbe = SafeReadString(ReadU64(SamplerSlotOffset));
									if (SamplerNameProbe.empty() || SamplerNameProbe[0] != '_')
									{
										break;
									}
									if (!IsCurveSlotValid(CurveSlotOffset))
									{
										break;
									}

									PatternCount++;
								}

								if (PatternCount == 0)
								{
									continue;
								}

								const int32_t Score = static_cast<int32_t>(PatternCount);
								if (Score > BestScore)
								{
									BestScore = Score;
									BestLayout.mMaterialNameOffset = NameOffset;
									BestLayout.mSamplerNameArrayOffset = SamplerArrayOffset;
									BestLayout.mCurveArrayOffset = CurveArrayOffset;
									BestLayout.mTexturePatternCount = PatternCount;
									BestLayout.mValid = true;
								}
							}
						}
					}

					return BestLayout;
				};

				TrackLayout Layout = ResolveTrackLayout();
				if (!Layout.mValid)
				{
					continue;
				}

				const uint64_t MaterialNameOffset = Layout.mMaterialNameOffset;
				const uint64_t SamplerNameArrayOffset = Layout.mSamplerNameArrayOffset;
				const uint64_t CurveArrayOffset = Layout.mCurveArrayOffset;
				const std::string MaterialName = SafeReadString(MaterialNameOffset);
				if (MaterialName.empty())
				{
					continue;
				}

				const uint32_t MaterialTexturePatternCount = Layout.mTexturePatternCount;
				if (MaterialTexturePatternCount == 0)
				{
					continue;
				}

				const std::vector<uint64_t> SamplerOffsets = ReadArrayQWords(SamplerNameArrayOffset, MaterialTexturePatternCount);
				if (SamplerOffsets.empty())
				{
					continue;
				}

				// Use the same value table the material animation references and choose per-material
				// textures by frame token (middle/senior/curse/etc). This follows FMAB value sets
				// while keeping parser scope minimal for now.
				for (uint32_t SamplerIndex = 0; SamplerIndex < MaterialTexturePatternCount && SamplerIndex < SamplerOffsets.size(); SamplerIndex++)
				{
					const std::string SamplerName = SafeReadString(SamplerOffsets[SamplerIndex]);
					if (SamplerName.empty() || !SamplerName.starts_with("_a"))
					{
						continue;
					}

					// Preferred path: decode this sampler's texture pattern curve and sample
					// the exact ModelInfo frame.
					bool AppliedFromCurve = false;
					const uint64_t CurveOffset = CurveArrayOffset + static_cast<uint64_t>(SamplerIndex) * 0x30;
					if (CurveArrayOffset != 0 && CurveOffset + 0x30 <= AnimBytes.size())
					{
						const uint64_t KeyFrameBufferOffset = ReadU64(CurveOffset + 0x00);
						const uint64_t ValueBufferOffset = ReadU64(CurveOffset + 0x08);
						const uint64_t PackedCurveTypeAndKeyCount = ReadU64(CurveOffset + 0x10);
						const uint64_t PackedFlags = ReadU64(CurveOffset + 0x20);
						const uint32_t KeyCount = static_cast<uint32_t>((PackedCurveTypeAndKeyCount >> 16) & 0xFFFF);
						const int32_t ValueBaseIndex = static_cast<int32_t>((PackedFlags >> 32) & 0xFF);

						if (KeyCount > 0 &&
							KeyCount < 512 &&
							KeyFrameBufferOffset + KeyCount <= AnimBytes.size() &&
							ValueBufferOffset + KeyCount <= AnimBytes.size())
						{
							std::vector<std::pair<int32_t, int32_t>> KeyValuePairs;
							KeyValuePairs.reserve(KeyCount);
							for (uint32_t KeyIndex = 0; KeyIndex < KeyCount; KeyIndex++)
							{
								const int32_t KeyFrame = static_cast<int32_t>(AnimBytes[KeyFrameBufferOffset + KeyIndex]);
								const int8_t ValueDelta = static_cast<int8_t>(AnimBytes[ValueBufferOffset + KeyIndex]);
								const int32_t ValueIndex = ValueBaseIndex + static_cast<int32_t>(ValueDelta);
								if (ValueIndex < 0 || static_cast<size_t>(ValueIndex) >= TextureValues.size())
								{
									continue;
								}
								KeyValuePairs.emplace_back(KeyFrame, ValueIndex);
							}

							if (!KeyValuePairs.empty())
							{
								std::sort(KeyValuePairs.begin(), KeyValuePairs.end(), [](const auto& Left, const auto& Right)
									{
										return Left.first < Right.first;
									});

								const int32_t TargetFrame = std::max(0, Frame);
								int32_t SelectedValueIndex = KeyValuePairs.front().second;
								for (const auto& [KeyFrame, ValueIndex] : KeyValuePairs)
								{
									if (KeyFrame > TargetFrame)
									{
										break;
									}
									SelectedValueIndex = ValueIndex;
								}

								if (SelectedValueIndex >= 0 && static_cast<size_t>(SelectedValueIndex) < TextureValues.size())
								{
									OverridesByMaterial[MaterialName] = TextureValues[static_cast<size_t>(SelectedValueIndex)];
									AppliedFromCurve = true;
								}
							}
						}
					}

					if (AppliedFromCurve)
					{
						continue;
					}

					const std::vector<std::string> MaterialTokens = GetMaterialSearchTokens(MaterialName);
					std::vector<std::string> StrictCandidates;
					for (const std::string& TextureName : TextureValues)
					{
						if (!IsLikelyAlbedoName(TextureName))
						{
							continue;
						}
						const std::string LowerTex = ToLowerCopy(TextureName);
						bool MatchesMaterial = MaterialTokens.empty();
						for (const std::string& Token : MaterialTokens)
						{
							if (LowerTex.find(Token) != std::string::npos)
							{
								MatchesMaterial = true;
								break;
							}
						}

						if (!MatchesMaterial)
						{
							continue;
						}
						StrictCandidates.push_back(TextureName);
					}

					std::vector<std::string> Candidates = StrictCandidates;
					const std::string MaterialHint = GetMaterialNameHint(MaterialName);

					// Limited alias fallback for families where the material is named "skin" but
					// textures are named "...Body_Alb". Keep this narrow to avoid cross-material bleed.
					if (Candidates.empty() && MaterialHint == "skin")
					{
						for (const std::string& TextureName : TextureValues)
						{
							if (!IsLikelyAlbedoName(TextureName))
							{
								continue;
							}

							if (ToLowerCopy(TextureName).find("body") != std::string::npos)
							{
								Candidates.push_back(TextureName);
							}
						}
					}

					// Final fallback for hashed/shared pools when no semantic match exists.
					if (Candidates.empty())
					{
						for (const std::string& TextureName : TextureValues)
						{
							if (IsLikelyAlbedoName(TextureName))
							{
								Candidates.push_back(TextureName);
							}
						}
					}

					if (Candidates.empty())
					{
						continue;
					}

					// Follow the exact ModelInfo frame index deterministically, without relying
					// on naming conventions (red/blue/middle/etc).
					const size_t FrameIndex = static_cast<size_t>(std::max(0, Frame));
					const std::string Selected = Candidates[std::min(FrameIndex, Candidates.size() - 1)];

					OverridesByMaterial[MaterialName] = Selected;
				}
			}

			return OverridesByMaterial.empty() ? std::nullopt : std::optional<std::unordered_map<std::string, std::string>>(std::move(OverridesByMaterial));
		}

	}

	std::vector<const char*> BancEntity::BakedFluid::gFluidShapeTypes = 
	{
		"Box",
		"Cylinder"
	};
	std::vector<const char*> BancEntity::BakedFluid::gFluidMaterialTypes =
	{
		"Water",
		"Lava",
		"Drift Sand",
		"Warm Water (No Damage)",
		"Ice Water (Immediate Damage)",
		"Abyss Water (Immediately Drowns)"
	};
	std::vector<std::pair<uint32_t, BancEntity::FluidMaterialType>> BancEntity::BakedFluid::gFluidMaterialTypeMap =
	{
		{38, BancEntity::FluidMaterialType::WATER},
		{41, BancEntity::FluidMaterialType::LAVA},
		{3, BancEntity::FluidMaterialType::DRIFT_SAND},
		{39, BancEntity::FluidMaterialType::WARM_WATER},
		{40, BancEntity::FluidMaterialType::ICE_WATER},
		{42, BancEntity::FluidMaterialType::ABYSS_WATER}
	};

	BancEntity::FluidMaterialType BancEntity::BakedFluid::ToMaterialType(uint32_t Type)
	{
		for (auto& [Id, Material] : BancEntity::BakedFluid::gFluidMaterialTypeMap)
		{
			if (Id == Type)
				return Material;
		}

		application::util::Logger::Warning("BancEntity", "Could not match type %u with internal fluid type", Type);
		return BancEntity::BakedFluid::gFluidMaterialTypeMap[0].second;
	}

	uint32_t BancEntity::BakedFluid::ToInternalIdentifier(BancEntity::FluidMaterialType Type)
	{
		for (auto& [Id, Material] : BancEntity::BakedFluid::gFluidMaterialTypeMap)
		{
			if (Material == Type)
				return Id;
		}

		return BancEntity::BakedFluid::gFluidMaterialTypeMap[0].first;
	}

	void BancEntity::ParseDynamicTypeParameter(application::file::game::byml::BymlFile::Node& Node, const std::string& Key, std::map<std::string, std::variant<std::string, bool, int32_t, int64_t, uint32_t, uint64_t, float, glm::vec3>>& Map)
	{
		if (Node.HasChild(Key))
		{
			for (application::file::game::byml::BymlFile::Node& DynamicNode : Node.GetChild(Key)->GetChildren())
			{
				std::variant<std::string, bool, int32_t, int64_t, uint32_t, uint64_t, float, glm::vec3> Data;

				switch (DynamicNode.GetType())
				{
				case application::file::game::byml::BymlFile::Type::StringIndex:
				{
					Data = DynamicNode.GetValue<std::string>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::Bool:
				{
					Data = DynamicNode.GetValue<bool>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::Int32:
				{
					Data = DynamicNode.GetValue<int32_t>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::Int64:
				{
					Data = DynamicNode.GetValue<int64_t>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::UInt32:
				{
					Data = DynamicNode.GetValue<uint32_t>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::UInt64:
				{
					Data = DynamicNode.GetValue<uint64_t>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::Float:
				{
					Data = DynamicNode.GetValue<float>();
					break;
				}
				case application::file::game::byml::BymlFile::Type::Array:
				{
					glm::vec3 Vec;
					Vec.x = DynamicNode.GetChild(0)->GetValue<float>();
					Vec.y = DynamicNode.GetChild(1)->GetValue<float>();
					Vec.z = DynamicNode.GetChild(2)->GetValue<float>();
					Data = Vec;
					break;
				}
				default:
				{
					application::util::Logger::Error("BancEntity", "Invalid dynamic data type");
					break;
				}
				}
				Map.insert({ DynamicNode.GetKey(), Data });
			}
		}
	}

	template <typename T>
	application::file::game::byml::BymlFile::Node GenerateBymlNode(application::file::game::byml::BymlFile::Type Type, const std::string& Key, const T& Value)
	{
		application::file::game::byml::BymlFile::Node Node(Type, Key);
		Node.SetValue<T>(Value);
		return Node;
	}

	void GenerateBymlVectorNode(application::file::game::byml::BymlFile::Node& Root, const std::string& Key, const glm::vec3& Vector, const glm::vec3& DefaultValues, bool SkipDefaultVectorCheck)
	{
		if (Vector.x != DefaultValues.x || Vector.y != DefaultValues.y || Vector.z != DefaultValues.z || SkipDefaultVectorCheck)
		{
			application::file::game::byml::BymlFile::Node VectorRoot(application::file::game::byml::BymlFile::Type::Array, Key);

			VectorRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Float, "0", Vector.x));
			VectorRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Float, "1", Vector.y));
			VectorRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Float, "2", Vector.z));

			Root.AddChild(VectorRoot);
		}
	}

	void BancEntity::WriteDynamicTypeParameter(application::file::game::byml::BymlFile::Node& Node, const std::string& Key, std::map<std::string, std::variant<std::string, bool, int32_t, int64_t, uint32_t, uint64_t, float, glm::vec3>>& Map, bool SkipEmptyCheck)
	{
		if (Map.empty() && !SkipEmptyCheck)
			return;

		application::file::game::byml::BymlFile::Node Root(application::file::game::byml::BymlFile::Type::Dictionary, Key);

		for (auto const& [Key, Value] : Map)
		{
			application::file::game::byml::BymlFile::Type EntryType = application::file::game::byml::BymlFile::Type::Null;

			if(std::holds_alternative<std::string>(Value)) EntryType = application::file::game::byml::BymlFile::Type::StringIndex;
			if(std::holds_alternative<float>(Value)) EntryType = application::file::game::byml::BymlFile::Type::Float;
			if(std::holds_alternative<uint32_t>(Value)) EntryType = application::file::game::byml::BymlFile::Type::UInt32;
			if(std::holds_alternative<uint64_t>(Value)) EntryType = application::file::game::byml::BymlFile::Type::UInt64;
			if(std::holds_alternative<int32_t>(Value)) EntryType = application::file::game::byml::BymlFile::Type::Int32;
			if(std::holds_alternative<int64_t>(Value)) EntryType = application::file::game::byml::BymlFile::Type::Int64;
			if(std::holds_alternative<bool>(Value)) EntryType = application::file::game::byml::BymlFile::Type::Bool;
			if(std::holds_alternative<glm::vec3>(Value)) EntryType = application::file::game::byml::BymlFile::Type::Array;

			if (EntryType == application::file::game::byml::BymlFile::Type::Null)
			{
				application::util::Logger::Warning("BancEntity", "Invalid dynamic parameter type");
				continue;
			}

			application::file::game::byml::BymlFile::Node Entry(EntryType, Key);

			if (EntryType != application::file::game::byml::BymlFile::Type::Array)
			{
				switch (EntryType)
				{
				case application::file::game::byml::BymlFile::Type::StringIndex:
					Entry.SetValue<std::string>(std::get<std::string>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::Float:
					Entry.SetValue<float>(std::get<float>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::UInt32:
					Entry.SetValue<uint32_t>(std::get<uint32_t>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::UInt64:
					Entry.SetValue<uint64_t>(std::get<uint64_t>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::Int32:
					Entry.SetValue<int32_t>(std::get<int32_t>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::Int64:
					Entry.SetValue<int64_t>(std::get<int64_t>(Value));
					break;
				case application::file::game::byml::BymlFile::Type::Bool:
					Entry.SetValue<bool>(std::get<bool>(Value));
					break;
				}
				Root.AddChild(Entry);
			}
			else
			{
				GenerateBymlVectorNode(Root, Key, std::get<glm::vec3>(Value), glm::vec3(0, 0, 0), true);
			}
		}

		Node.AddChild(Root);
	}

	bool BancEntity::FromByml(application::file::game::byml::BymlFile::Node& Node)
	{
		if (!Node.HasChild("Gyaml")) return false;

		mGyml = Node.GetChild("Gyaml")->GetValue<std::string>();

		if (Node.HasChild("Hash")) mHash = Node.GetChild("Hash")->GetValue<uint64_t>();
		if (Node.HasChild("SRTHash")) mSRTHash = Node.GetChild("SRTHash")->GetValue<uint32_t>();

		if (Node.HasChild("Translate")) mTranslate = Node.GetChild("Translate")->GetValue<glm::vec3>();
		if (Node.HasChild("Rotate")) mRotate = Node.GetChild("Rotate")->GetValue<glm::vec3>();
		if (Node.HasChild("Scale")) mScale = Node.GetChild("Scale")->GetValue<glm::vec3>();

		if (Node.HasChild("Bakeable")) mBakeable = Node.GetChild("Bakeable")->GetValue<bool>();
		if (Node.HasChild("IsPhysicsStable")) mIsPhysicsStable = Node.GetChild("IsPhysicsStable")->GetValue<bool>();
		if (Node.HasChild("MoveRadius")) mMoveRadius = Node.GetChild("MoveRadius")->GetValue<float>();
		if (Node.HasChild("ExtraCreateRadius")) mExtraCreateRadius = Node.GetChild("ExtraCreateRadius")->GetValue<float>();
		if (Node.HasChild("IsForceActive")) mIsForceActive = Node.GetChild("IsForceActive")->GetValue<bool>();
		if (Node.HasChild("IsInWater")) mIsInWater = Node.GetChild("IsInWater")->GetValue<bool>();
		if (Node.HasChild("TurnActorNearEnemy")) mTurnActorNearEnemy = Node.GetChild("TurnActorNearEnemy")->GetValue<bool>();
		if (Node.HasChild("Name")) mName = Node.GetChild("Name")->GetValue<std::string>();
		if (Node.HasChild("Version")) mVersion = Node.GetChild("Version")->GetValue<int32_t>();

		if (Node.HasChild("Links"))
		{
			mLinks.reserve(Node.GetChild("Links")->GetChildren().size());

			for (application::file::game::byml::BymlFile::Node& LinksChild : Node.GetChild("Links")->GetChildren())
			{
				BancEntity::Link EntityLink;
				if (LinksChild.HasChild("Dst")) EntityLink.mDest = LinksChild.GetChild("Dst")->GetValue<uint64_t>();
				if (LinksChild.HasChild("Gyaml")) EntityLink.mGyml = LinksChild.GetChild("Gyaml")->GetValue<std::string>();
				if (LinksChild.HasChild("Name")) EntityLink.mName = LinksChild.GetChild("Name")->GetValue<std::string>();
				if (LinksChild.HasChild("Src")) EntityLink.mSrc = LinksChild.GetChild("Src")->GetValue<uint64_t>();

				mLinks.push_back(EntityLink);
			}
		}

		if (Node.HasChild("Rails"))
		{
			mRails.reserve(Node.GetChild("Rails")->GetChildren().size());

			for (application::file::game::byml::BymlFile::Node& RailsChild : Node.GetChild("Rails")->GetChildren())
			{
				BancEntity::Rail EntityRail;
				if (RailsChild.HasChild("Dst")) EntityRail.mDest = RailsChild.GetChild("Dst")->GetValue<uint64_t>();
				if (RailsChild.HasChild("Gyaml")) EntityRail.mGyml = RailsChild.GetChild("Gyaml")->GetValue<std::string>();
				if (RailsChild.HasChild("Name")) EntityRail.mName = RailsChild.GetChild("Name")->GetValue<std::string>();

				mRails.push_back(EntityRail);
			}
		}
		
		ParseDynamicTypeParameter(Node, "Dynamic", mDynamic);
		ParseDynamicTypeParameter(Node, "ExternalParameter", mExternalParameter);
		ParseDynamicTypeParameter(Node, "Presence", mPresence);
		
		if (Node.HasChild("Phive"))
		{
			application::file::game::byml::BymlFile::Node* PhiveNode = Node.GetChild("Phive");
			if (PhiveNode->HasChild("Rails"))
			{
				mPhive.mRails.reserve(PhiveNode->GetChild("Rails")->GetChildren().size());

				for (application::file::game::byml::BymlFile::Node& RailNode : PhiveNode->GetChild("Rails")->GetChildren())
				{
					Phive::Rail Rail;
					if (RailNode.HasChild("IsClosed")) Rail.mIsClosed = RailNode.GetChild("IsClosed")->GetValue<bool>();
					if (RailNode.HasChild("Type")) Rail.mType = RailNode.GetChild("Type")->GetValue<std::string>();

					if (RailNode.HasChild("Nodes"))
					{
						for (application::file::game::byml::BymlFile::Node& BymlNode : RailNode.GetChild("Nodes")->GetChildren())
						{
							for (application::file::game::byml::BymlFile::Node& SubNode : BymlNode.GetChildren())
							{
								Phive::Rail::Node PhiveRailNode;
								PhiveRailNode.mKey = SubNode.GetKey();

								PhiveRailNode.mValue.x = SubNode.GetChild(0)->GetValue<float>();
								PhiveRailNode.mValue.y = SubNode.GetChild(1)->GetValue<float>();
								PhiveRailNode.mValue.z = SubNode.GetChild(2)->GetValue<float>();

								Rail.mNodes.push_back(PhiveRailNode);
							}
						}
					}

					mPhive.mRails.push_back(Rail);
				}
			}

			if (PhiveNode->HasChild("RopeHeadLink"))
			{
				Phive::RopeLink RopeLink;
				if (PhiveNode->GetChild("RopeHeadLink")->HasChild("ID")) RopeLink.mID = PhiveNode->GetChild("RopeHeadLink")->GetChild("ID")->GetValue<uint64_t>();
				if (PhiveNode->GetChild("RopeHeadLink")->HasChild("Owners"))
				{
					for (application::file::game::byml::BymlFile::Node& OwnerNode : PhiveNode->GetChild("RopeHeadLink")->GetChild("Owners")->GetChildren())
					{
						RopeLink.mOwners.push_back(OwnerNode.GetChild("Refer")->GetValue<uint64_t>());
					}
				}
				if (PhiveNode->GetChild("RopeHeadLink")->HasChild("Refers"))
				{
					for (application::file::game::byml::BymlFile::Node& ReferNode : PhiveNode->GetChild("RopeHeadLink")->GetChild("Refers")->GetChildren())
					{
						RopeLink.mRefers.push_back(ReferNode.GetChild("Owner")->GetValue<uint64_t>());
					}
				}
				mPhive.mRopeHeadLink = RopeLink;
			}

			if (PhiveNode->HasChild("RopeTailLink"))
			{
				Phive::RopeLink RopeLink;
				if (PhiveNode->GetChild("RopeTailLink")->HasChild("ID")) RopeLink.mID = PhiveNode->GetChild("RopeTailLink")->GetChild("ID")->GetValue<uint64_t>();
				if (PhiveNode->GetChild("RopeTailLink")->HasChild("Owners"))
				{
					for (application::file::game::byml::BymlFile::Node& OwnerNode : PhiveNode->GetChild("RopeTailLink")->GetChild("Owners")->GetChildren())
					{
						RopeLink.mOwners.push_back(OwnerNode.GetChild("Refer")->GetValue<uint64_t>());
					}
				}
				if (PhiveNode->GetChild("RopeTailLink")->HasChild("Refers"))
				{
					for (application::file::game::byml::BymlFile::Node& ReferNode : PhiveNode->GetChild("RopeTailLink")->GetChild("Refers")->GetChildren())
					{
						RopeLink.mRefers.push_back(ReferNode.GetChild("Owner")->GetValue<uint64_t>());
					}
				}
				mPhive.mRopeTailLink = RopeLink;
			}

			ParseDynamicTypeParameter(*PhiveNode, "Placement", mPhive.mPlacement);
			
			if (PhiveNode->HasChild("ConstraintLink"))
			{
				application::file::game::byml::BymlFile::Node* ConstraintLinkNode = PhiveNode->GetChild("ConstraintLink");
				Phive::ConstraintLink ConstraintLink;

				if (ConstraintLinkNode->HasChild("ID")) ConstraintLink.mID = ConstraintLinkNode->GetChild("ID")->GetValue<uint64_t>();

				if (ConstraintLinkNode->HasChild("Refers"))
				{
					for (application::file::game::byml::BymlFile::Node& ReferNode : ConstraintLinkNode->GetChild("Refers")->GetChildren())
					{
						Phive::ConstraintLink::Refer Refer;
						if (ReferNode.HasChild("Owner")) Refer.mOwner = ReferNode.GetChild("Owner")->GetValue<uint64_t>();
						if (ReferNode.HasChild("Type")) Refer.mType = ReferNode.GetChild("Type")->GetValue<std::string>();
						ConstraintLink.mRefers.push_back(Refer);
					}
				}
				if (ConstraintLinkNode->HasChild("Owners"))
				{
					for (application::file::game::byml::BymlFile::Node& OwnersNode : ConstraintLinkNode->GetChild("Owners")->GetChildren())
					{
						Phive::ConstraintLink::Owner Owner;
						ParseDynamicTypeParameter(OwnersNode, "AliasData", Owner.mAliasData);
						ParseDynamicTypeParameter(OwnersNode, "BreakableData", Owner.mBreakableData);
						ParseDynamicTypeParameter(OwnersNode, "ClusterData", Owner.mClusterData);
						ParseDynamicTypeParameter(OwnersNode, "UserData", Owner.mUserData);

						if (OwnersNode.HasChild("OwnerPose"))
						{
							if (OwnersNode.GetChild("OwnerPose")->HasChild("Rotate"))
							{
								Owner.mOwnerPose.mRotate.x = OwnersNode.GetChild("OwnerPose")->GetChild("Rotate")->GetChild(0)->GetValue<float>();
								Owner.mOwnerPose.mRotate.y = OwnersNode.GetChild("OwnerPose")->GetChild("Rotate")->GetChild(1)->GetValue<float>();
								Owner.mOwnerPose.mRotate.z = OwnersNode.GetChild("OwnerPose")->GetChild("Rotate")->GetChild(2)->GetValue<float>();
								Owner.mOwnerPose.mRotate = application::util::Math::RadiansToDegrees(Owner.mOwnerPose.mRotate);
							}
							if (OwnersNode.GetChild("OwnerPose")->HasChild("Trans"))
							{
								Owner.mOwnerPose.mTranslate.x = OwnersNode.GetChild("OwnerPose")->GetChild("Trans")->GetChild(0)->GetValue<float>();
								Owner.mOwnerPose.mTranslate.y = OwnersNode.GetChild("OwnerPose")->GetChild("Trans")->GetChild(1)->GetValue<float>();
								Owner.mOwnerPose.mTranslate.z = OwnersNode.GetChild("OwnerPose")->GetChild("Trans")->GetChild(2)->GetValue<float>();
							}
						}
						ParseDynamicTypeParameter(OwnersNode, "ParamData", Owner.mParamData);

						if (OwnersNode.HasChild("PivotData"))
						{
							application::file::game::byml::BymlFile::Node* PivotDataNode = OwnersNode.GetChild("PivotData");
							if (PivotDataNode->HasChild("Axis")) Owner.mPivotData.mAxis = PivotDataNode->GetChild("Axis")->GetValue<int32_t>();
							if (PivotDataNode->HasChild("AxisA")) Owner.mPivotData.mAxisA = PivotDataNode->GetChild("AxisA")->GetValue<int32_t>();
							if (PivotDataNode->HasChild("AxisB")) Owner.mPivotData.mAxisB = PivotDataNode->GetChild("AxisB")->GetValue<int32_t>();

							if (PivotDataNode->HasChild("Pivot"))
							{
								glm::vec3 Vec;
								Vec.x = PivotDataNode->GetChild("Pivot")->GetChild(0)->GetValue<float>();
								Vec.y = PivotDataNode->GetChild("Pivot")->GetChild(1)->GetValue<float>();
								Vec.z = PivotDataNode->GetChild("Pivot")->GetChild(2)->GetValue<float>();
								Owner.mPivotData.mPivot = Vec;
							}
							if (PivotDataNode->HasChild("PivotA"))
							{
								glm::vec3 Vec;
								Vec.x = PivotDataNode->GetChild("PivotA")->GetChild(0)->GetValue<float>();
								Vec.y = PivotDataNode->GetChild("PivotA")->GetChild(1)->GetValue<float>();
								Vec.z = PivotDataNode->GetChild("PivotA")->GetChild(2)->GetValue<float>();
								Owner.mPivotData.mPivotA = Vec;
							}
							if (PivotDataNode->HasChild("PivotB"))
							{
								glm::vec3 Vec;
								Vec.x = PivotDataNode->GetChild("PivotB")->GetChild(0)->GetValue<float>();
								Vec.y = PivotDataNode->GetChild("PivotB")->GetChild(1)->GetValue<float>();
								Vec.z = PivotDataNode->GetChild("PivotB")->GetChild(2)->GetValue<float>();
								Owner.mPivotData.mPivotB = Vec;
							}
						}
						if (OwnersNode.HasChild("Refer")) Owner.mRefer = OwnersNode.GetChild("Refer")->GetValue<uint64_t>();
						if (OwnersNode.HasChild("ReferPose"))
						{
							if (OwnersNode.GetChild("ReferPose")->HasChild("Rotate"))
							{
								Owner.mReferPose.mRotate.x = OwnersNode.GetChild("ReferPose")->GetChild("Rotate")->GetChild(0)->GetValue<float>();
								Owner.mReferPose.mRotate.y = OwnersNode.GetChild("ReferPose")->GetChild("Rotate")->GetChild(1)->GetValue<float>();
								Owner.mReferPose.mRotate.z = OwnersNode.GetChild("ReferPose")->GetChild("Rotate")->GetChild(2)->GetValue<float>();
								Owner.mReferPose.mRotate = application::util::Math::RadiansToDegrees(Owner.mReferPose.mRotate);
							}
							if (OwnersNode.GetChild("ReferPose")->HasChild("Trans"))
							{
								Owner.mReferPose.mTranslate.x = OwnersNode.GetChild("ReferPose")->GetChild("Trans")->GetChild(0)->GetValue<float>();
								Owner.mReferPose.mTranslate.y = OwnersNode.GetChild("ReferPose")->GetChild("Trans")->GetChild(1)->GetValue<float>();
								Owner.mReferPose.mTranslate.z = OwnersNode.GetChild("ReferPose")->GetChild("Trans")->GetChild(2)->GetValue<float>();
							}
						}
						if (OwnersNode.HasChild("Type")) Owner.mType = OwnersNode.GetChild("Type")->GetValue<std::string>();
						ConstraintLink.mOwners.push_back(Owner);
					}
				}

				mPhive.mConstraintLink = ConstraintLink;
			}
		}

		if (mDynamic.contains("BancPath"))
		{
			mMergedActorChildren = application::manager::MergedActorMgr::GetMergedActor(std::get<std::string>(mDynamic["BancPath"]));
		}

		InitializeRotationPersistenceFromCurrent();
		return GenerateBancEntityInfo();
	}

	bool BancEntity::FromGyml(const std::string& Gyml)
	{
		if (!application::util::FileUtil::FileExists(application::util::FileUtil::GetRomFSFilePath("Pack/Actor/" + Gyml + ".pack.zs")))
			return false;

		mGyml = Gyml;
		 
		InitializeRotationPersistenceFromCurrent();
		return GenerateBancEntityInfo();
	}

	bool BancEntity::GenerateBancEntityInfo()
	{
		application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo begin Gyml=%s", mGyml.c_str());
		mActorPack = application::manager::ActorPackMgr::GetActorPack(mGyml);
		application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo ActorPack=%p Gyml=%s", static_cast<const void*>(mActorPack), mGyml.c_str());
		mHornBfresRenderer = nullptr;
		mHornAttachmentMatrix = glm::mat4(1.0f);
		mHornModelCorrectionMatrix = glm::mat4(1.0f);
		mHasHornAttachment = false;
		mTexturePatternFrame = -1;
		mTexturePatternAnimationName.clear();
		mTexturePatternModelProjectName.clear();
		mTexturePatternOverrideKey.clear();
		mTexturePatternAlbedoOverridesByMaterial.clear();
		mEquipmentAttachments.clear();

		if (mActorPack == nullptr)
			return false;


		mBfresRenderer = nullptr;
		bool HasResolvedMainModel = false;
		if (application::game::actor_component::ActorComponentBase* BaseComponent = mActorPack->GetComponent(application::game::actor_component::ActorComponentBase::ComponentType::MODEL_INFO); BaseComponent != nullptr)
		{
			application::game::actor_component::ActorComponentModelInfo* ModelInfoComponent = static_cast<application::game::actor_component::ActorComponentModelInfo*>(BaseComponent);
			if (ModelInfoComponent)
			{
				if (ModelInfoComponent->mModelProjectName.has_value() && ModelInfoComponent->mFmdbName.has_value() && !ModelInfoComponent->mModelProjectName.value().empty() && !ModelInfoComponent->mFmdbName.value().empty())
				{
					mTexturePatternModelProjectName = ModelInfoComponent->mModelProjectName.value();
					const std::string ModelBfresPath = application::util::FileUtil::GetBfresFilePath(ModelInfoComponent->mModelProjectName.value() + "." + ModelInfoComponent->mFmdbName.value() + ".bfres");
					application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s ResolvingMainBfres=%s", mGyml.c_str(), ModelBfresPath.c_str());
					application::file::game::bfres::BfresFile* File = application::manager::BfresFileMgr::GetBfresFile(ModelBfresPath);
					application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s BfresFile=%p", mGyml.c_str(), static_cast<const void*>(File));
					mBfresRenderer = application::manager::BfresRendererMgr::GetRenderer(File);
					application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s BfresRenderer=%p", mGyml.c_str(), static_cast<const void*>(mBfresRenderer));
					HasResolvedMainModel = mBfresRenderer != nullptr && !mBfresRenderer->mBfresFile->mDefaultModel;
				}

				// Texture pattern animations in ModelInfo are evaluated at a fixed frame.
				// We use that frame as static texture array index override for rendering.
				if (ModelInfoComponent->mModelVariationFmabName.has_value() && ModelInfoComponent->mModelVariationFmabFrame.has_value() && !ModelInfoComponent->mModelVariationFmabName->empty())
				{
					mTexturePatternAnimationName = std::filesystem::path(ModelInfoComponent->mModelVariationFmabName.value()).stem().string();
					mTexturePatternFrame = std::max(0, static_cast<int32_t>(std::lround(ModelInfoComponent->mModelVariationFmabFrame.value())));
					mTexturePatternOverrideKey = mTexturePatternModelProjectName + "|" + mTexturePatternAnimationName + "|" + std::to_string(mTexturePatternFrame);
					application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s ResolvingTexturePattern Project=%s Anim=%s Frame=%d",
						mGyml.c_str(), mTexturePatternModelProjectName.c_str(), mTexturePatternAnimationName.c_str(), mTexturePatternFrame);
					if (std::optional<std::unordered_map<std::string, std::string>> Overrides = ResolveTexturePatternAlbedoOverrides(mTexturePatternModelProjectName, mTexturePatternAnimationName, mTexturePatternFrame); Overrides.has_value())
					{
						mTexturePatternAlbedoOverridesByMaterial = std::move(Overrides.value());
					}
					application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s TexturePatternResolved Overrides=%u",
						mGyml.c_str(), static_cast<uint32_t>(mTexturePatternAlbedoOverridesByMaterial.size()));

				}
			}
		}

		if (mBfresRenderer == nullptr)
		{
			application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s ResolvingFallbackBfresByGymlSubstring", mGyml.c_str());
			application::file::game::bfres::BfresFile* File = &application::manager::BfresFileMgr::gBfresFiles["Default"];
			for (auto& [Key, Val] : application::manager::BfresFileMgr::gBfresFiles)
			{
				if (mGyml.find(Key) != std::string::npos)
				{
					File = &Val;
					break;
				}
			}

			mBfresRenderer = application::manager::BfresRendererMgr::GetRenderer(File);
			application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s FallbackBfresRenderer=%p", mGyml.c_str(), static_cast<const void*>(mBfresRenderer));
		}

		if (mBfresRenderer == nullptr)
		{
			application::util::Logger::Warning("LoadDebug", "GenerateBancEntityInfo Gyml=%s aborted: mBfresRenderer is null", mGyml.c_str());
			return false;
		}

		if (HasResolvedMainModel)
		{
			application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s ResolvingHorn", mGyml.c_str());
			std::optional<std::string> HornModelPath = ResolveHornModelPath(*mActorPack, FindHornTypeToken(*this), mGyml);
			mHornBfresRenderer = ResolveHornRenderer(HornModelPath);
			application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s HornRenderer=%p HornPath=%s",
				mGyml.c_str(), static_cast<const void*>(mHornBfresRenderer), HornModelPath.value_or(std::string("<none>")).c_str());

			if (mHornBfresRenderer != nullptr)
			{
				// Main actor attachment point in world space.
				application::file::game::bfres::BfresFile::Model& MainModel = mBfresRenderer->mBfresFile->Models.GetByIndex(0).mValue;
				application::file::game::bfres::BfresFile::Skeleton::Bone* SelectedBone = nullptr;
				for (auto& [BoneName, BoneNode] : MainModel.ModelSkeleton.Bones.mNodes)
				{
					const std::string LowerName = ToLowerCopy(BoneName);
					if (LowerName == "material_pod")
					{
						SelectedBone = &BoneNode.mValue;
						break;
					}
				}

				if (SelectedBone == nullptr)
				{
					for (auto& [BoneName, BoneNode] : MainModel.ModelSkeleton.Bones.mNodes)
					{
						const std::string LowerName = ToLowerCopy(BoneName);
						if (ContainsInsensitive(LowerName, "material_pod"))
						{
							SelectedBone = &BoneNode.mValue;
							break;
						}
					}
				}

				if (SelectedBone != nullptr)
				{
					mHornAttachmentMatrix = SelectedBone->WorldMatrix;
					mHasHornAttachment = true;
				}

				// Requested behavior: bind horn model origin (0,0,0) directly to Material_Pod.
				// No horn-local bone correction transform is applied.
				mHornModelCorrectionMatrix = glm::mat4(1.0f);

				if (!mHasHornAttachment)
				{
					mHornBfresRenderer = nullptr;
				}
			}

			std::unordered_map<std::string, std::string> SlotValues;
			std::string WeaponBoneName = "Weapon";
			std::string ShieldBoneName = "Tool";
			std::string BowBoneName = "Bow";
			float BowScaleRatio = 1.0f;
			float ShieldScaleRatio = 1.0f;
			float SmallSwordScaleRatio = 1.0f;
			float LargeSwordScaleRatio = 1.0f;
			float SpearScaleRatio = 1.0f;

			const std::array<const std::map<std::string, std::variant<std::string, bool, int32_t, int64_t, uint32_t, uint64_t, float, glm::vec3>>*, 3> SlotSources =
			{
				&mPresence,
				&mExternalParameter,
				&mDynamic
			};
			for (const auto* Source : SlotSources)
			{
				for (const auto& [Key, Value] : *Source)
				{
					if (!std::holds_alternative<std::string>(Value))
					{
						continue;
					}

					const std::string StringValue = std::get<std::string>(Value);
					if (StringValue.empty())
					{
						continue;
					}

					if (Key == "EquipmentUser_Weapon" || Key == "EquipmentUser_Shield" || Key == "EquipmentUser_Bow" ||
						Key == "EquipmentUser_Head" || Key == "EquipmentUser_Upper" || Key == "EquipmentUser_Lower")
					{
						SlotValues[Key] = StringValue;
					}
					else if (Key == "EquipmentUser_Helmet")
					{
						SlotValues["EquipmentUser_Head"] = StringValue;
					}
				}
			}

			if (std::optional<std::string> RootActorParamPath = FindActorRootParamBymlPath(*mActorPack, mGyml); RootActorParamPath.has_value())
			{
				std::vector<application::file::game::byml::BymlFile> ActorParamChain = ResolveBymlInheritanceChain(*mActorPack, RootActorParamPath.value());
				std::optional<std::string> EquipmentUserParamPath = std::nullopt;
				std::optional<std::string> PackMatchedEquipmentUserParamPath = FindEquipmentUserParamPathInPack(*mActorPack, mGyml);
				if (std::optional<std::string> EquipmentUserRef = FindFirstNonEmptyStringInChain(ActorParamChain, "EquipmentUserRef"); EquipmentUserRef.has_value())
				{
					EquipmentUserParamPath = NormalizeWorkGymlPathToBymlPath(EquipmentUserRef.value());
					if (PackMatchedEquipmentUserParamPath.has_value() &&
						(ContainsInsensitive(EquipmentUserParamPath.value(), "enemycommon") || ContainsInsensitive(EquipmentUserParamPath.value(), "common")))
					{
						EquipmentUserParamPath = PackMatchedEquipmentUserParamPath;
					}
				}
				else
				{
					EquipmentUserParamPath = PackMatchedEquipmentUserParamPath;
				}

				if (EquipmentUserParamPath.has_value())
				{
					std::vector<application::file::game::byml::BymlFile> EquipmentUserChain = ResolveBymlInheritanceChain(*mActorPack, EquipmentUserParamPath.value());
					if (!EquipmentUserChain.empty())
					{
						std::optional<std::string> BlackboardTableRef = FindFirstNonEmptyStringInChain(EquipmentUserChain, "BlackboardTableRef");
						if (BlackboardTableRef.has_value())
						{
							std::vector<application::file::game::byml::BymlFile> BlackboardChain =
								ResolveBymlInheritanceChain(*mActorPack, NormalizeWorkGymlPathToBymlPath(BlackboardTableRef.value()));
							std::unordered_map<std::string, std::string> BlackboardSlots = ResolveBlackboardStringInitValues(BlackboardChain);
							for (const auto& [Key, Value] : BlackboardSlots)
							{
								if (!SlotValues.contains(Key))
								{
									SlotValues[Key] = Value;
								}
							}
						}

						WeaponBoneName = FindFirstNonEmptyStringInChain(EquipmentUserChain, "WeaponEquippedBoneName").value_or(WeaponBoneName);
						ShieldBoneName = FindFirstNonEmptyStringInChain(EquipmentUserChain, "ToolEquippedBoneName").value_or(ShieldBoneName);
						BowBoneName = FindFirstNonEmptyStringInChain(EquipmentUserChain, "BowEquippedBoneName").value_or(BowBoneName);
						BowScaleRatio = FindFirstNumericInChain(EquipmentUserChain, "BowScaleRatio").value_or(BowScaleRatio);
						ShieldScaleRatio = FindFirstNumericInChain(EquipmentUserChain, "ShieldScaleRatio").value_or(ShieldScaleRatio);
						SmallSwordScaleRatio = FindFirstNumericInChain(EquipmentUserChain, "SmallSwordScaleRatio").value_or(SmallSwordScaleRatio);
						LargeSwordScaleRatio = FindFirstNumericInChain(EquipmentUserChain, "LargeSwordScaleRatio").value_or(LargeSwordScaleRatio);
						SpearScaleRatio = FindFirstNumericInChain(EquipmentUserChain, "SpearScaleRatio").value_or(SpearScaleRatio);
					}
				}
			}

			if (SlotValues.contains("EquipmentUser_Helmet") && !SlotValues.contains("EquipmentUser_Head"))
			{
				SlotValues["EquipmentUser_Head"] = SlotValues["EquipmentUser_Helmet"];
			}

			struct SlotConfig
			{
				std::string mSlotKey;
				std::string mTargetBoneName;
				std::string mSourceBoneName;
			};
			const std::array<SlotConfig, 6> SlotConfigs =
			{
				SlotConfig{"EquipmentUser_Weapon", WeaponBoneName, "Root"},
				SlotConfig{"EquipmentUser_Shield", ShieldBoneName, "Root"},
				SlotConfig{"EquipmentUser_Bow", BowBoneName, "Root"},
				SlotConfig{"EquipmentUser_Head", "Root", "Root"},
				SlotConfig{"EquipmentUser_Upper", "Root", "Root"},
				SlotConfig{"EquipmentUser_Lower", "Root", "Root"}
			};

			application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Gyml=%s ResolvingEquipment SlotCount=%u",
				mGyml.c_str(), static_cast<uint32_t>(SlotValues.size()));
			application::file::game::bfres::BfresFile::Model& MainModel = mBfresRenderer->mBfresFile->Models.GetByIndex(0).mValue;
			for (const SlotConfig& Slot : SlotConfigs)
			{
				if (!SlotValues.contains(Slot.mSlotKey))
				{
					continue;
				}

				const std::string Gyml = NormalizeActorReferenceToGymlName(SlotValues[Slot.mSlotKey]);
				if (Gyml.empty())
				{
					continue;
				}

				application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo Owner=%s ResolvingEquipmentSlot Key=%s Gyml=%s",
					mGyml.c_str(), Slot.mSlotKey.c_str(), Gyml.c_str());
				application::gl::BfresRenderer* Renderer = ResolveRendererFromActorGyml(Gyml);
				if (Renderer == nullptr || Renderer->mBfresFile->mDefaultModel)
				{
					continue;
				}

				std::string ResolvedTargetBoneName;
				application::file::game::bfres::BfresFile::Skeleton::Bone* TargetBone = FindBoneByName(MainModel, Slot.mTargetBoneName, &ResolvedTargetBoneName);
				if (TargetBone == nullptr)
				{
					continue;
				}

				application::file::game::bfres::BfresFile::Model& EquipmentModel = Renderer->mBfresFile->Models.GetByIndex(0).mValue;
				float ScaleRatio = 1.0f;
				if (Slot.mSlotKey == "EquipmentUser_Bow")
				{
					ScaleRatio = BowScaleRatio;
				}
				else if (Slot.mSlotKey == "EquipmentUser_Shield")
				{
					ScaleRatio = ShieldScaleRatio;
				}
				else if (Slot.mSlotKey == "EquipmentUser_Weapon")
				{
					const std::string LowerGyml = ToLowerCopy(Gyml);
					if (LowerGyml.starts_with("weapon_sword_"))
					{
						ScaleRatio = SmallSwordScaleRatio;
					}
					else if (LowerGyml.starts_with("weapon_lsword_"))
					{
						ScaleRatio = LargeSwordScaleRatio;
					}
					else if (LowerGyml.starts_with("weapon_spear_"))
					{
						ScaleRatio = SpearScaleRatio;
					}
				}

				glm::mat4 ScaleMatrix(1.0f);
				ScaleMatrix[0][0] = ScaleRatio;
				ScaleMatrix[1][1] = ScaleRatio;
				ScaleMatrix[2][2] = ScaleRatio;
				EquipmentAttachment Attachment;
				Attachment.mSlotKey = Slot.mSlotKey;
				Attachment.mActorName = Gyml;
				Attachment.mBfresRenderer = Renderer;
				Attachment.mAttachmentMatrix = TargetBone->WorldMatrix;
				Attachment.mModelCorrectionMatrix = ScaleMatrix * BuildAttachmentCorrectionMatrix(EquipmentModel, Slot.mSourceBoneName);
				Attachment.mEnabled = true;
				mEquipmentAttachments.push_back(std::move(Attachment));
			}
		}

		application::util::Logger::Info("LoadDebug", "GenerateBancEntityInfo end Gyml=%s Equipment=%u Horn=%d",
			mGyml.c_str(), static_cast<uint32_t>(mEquipmentAttachments.size()), mHasHornAttachment ? 1 : 0);
		return true;
	}

	application::file::game::byml::BymlFile::Node BancEntity::ToByml()
	{
		application::file::game::byml::BymlFile::Node Node(application::file::game::byml::BymlFile::Type::Dictionary);

		if (mBakeable) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "Bakeable", mBakeable));
		WriteDynamicTypeParameter(Node, "Dynamic", mDynamic);
		WriteDynamicTypeParameter(Node, "ExternalParameter", mExternalParameter);
		if (mExtraCreateRadius != 0.0f) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Float, "ExtraCreateRadius", mExtraCreateRadius));
		Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Gyaml", mGyml));
		Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Hash", mHash));
		if (mIsForceActive) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "IsForceActive", mIsForceActive));
		if (mIsInWater) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "IsInWater", mIsInWater));
		if (mIsPhysicsStable) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "IsPhysicsStable", mIsPhysicsStable));

		if (!mLinks.empty())
		{
			application::file::game::byml::BymlFile::Node LinksNode(application::file::game::byml::BymlFile::Type::Array, "Links");

			for (Link& Link : mLinks)
			{
				application::file::game::byml::BymlFile::Node LinkNodeRoot(application::file::game::byml::BymlFile::Type::Dictionary);

				LinkNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Dst", Link.mDest));
				if (!Link.mGyml.empty()) LinkNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Gyaml", Link.mGyml));
				LinkNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Name", Link.mName));
				LinkNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Src", Link.mSrc));

				LinksNode.AddChild(LinkNodeRoot);
			}

			Node.AddChild(LinksNode);
		}

		if (mMoveRadius != 0.0f) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Float, "MoveRadius", mMoveRadius));
		if (!mName.empty()) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Name", mName));
		
		//Phive - Begin

		application::file::game::byml::BymlFile::Node PhiveRoot(application::file::game::byml::BymlFile::Type::Dictionary, "Phive");

		if (mPhive.mConstraintLink.has_value())
		{
			Phive::ConstraintLink& ConstraintLink = mPhive.mConstraintLink.value();
			application::file::game::byml::BymlFile::Node ConstraintLinkNode(application::file::game::byml::BymlFile::Type::Dictionary, "ConstraintLink");
			
			ConstraintLinkNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "ID", ConstraintLink.mID));
			
			if (!ConstraintLink.mOwners.empty())
			{
				application::file::game::byml::BymlFile::Node ConstraintLinkOwnersNode(application::file::game::byml::BymlFile::Type::Array, "Owners");
				
				for (Phive::ConstraintLink::Owner& Owner : ConstraintLink.mOwners)
				{
					application::file::game::byml::BymlFile::Node OwnerNode(application::file::game::byml::BymlFile::Type::Dictionary);

					WriteDynamicTypeParameter(OwnerNode, "AliasData", Owner.mAliasData);
					WriteDynamicTypeParameter(OwnerNode, "BreakableData", Owner.mBreakableData);
					WriteDynamicTypeParameter(OwnerNode, "ClusterData", Owner.mClusterData);
					
					if (Owner.mOwnerPose.mRotate != glm::vec3(0.0f, 0.0f, 0.0f) ||
						Owner.mOwnerPose.mTranslate != glm::vec3(0.0f, 0.0f, 0.0f))
					{
						application::file::game::byml::BymlFile::Node OwnerPoseNode(application::file::game::byml::BymlFile::Type::Dictionary, "OwnerPose");

						GenerateBymlVectorNode(OwnerPoseNode, "Rotate", glm::radians(Owner.mOwnerPose.mRotate), glm::vec3(0.0f, 0.0f, 0.0f), true);
						GenerateBymlVectorNode(OwnerPoseNode, "Trans", Owner.mOwnerPose.mTranslate, glm::vec3(0.0f, 0.0f, 0.0f), true);

						OwnerNode.AddChild(OwnerPoseNode);
					}

					WriteDynamicTypeParameter(OwnerNode, "ParamData", Owner.mParamData);

					application::file::game::byml::BymlFile::Node PivotDataNode(application::file::game::byml::BymlFile::Type::Dictionary, "PivotData");
					//Axis
					if (Owner.mPivotData.mAxis.has_value())
					{
						PivotDataNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Int32, "Axis", Owner.mPivotData.mAxis.value()));
					}
					if (Owner.mPivotData.mAxisA.has_value())
					{
						PivotDataNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Int32, "AxisA", Owner.mPivotData.mAxisA.value()));
					}
					if (Owner.mPivotData.mAxisB.has_value())
					{
						PivotDataNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Int32, "AxisB", Owner.mPivotData.mAxisB.value()));
					}
					//Vectors
					if (Owner.mPivotData.mPivot.has_value())
					{
						GenerateBymlVectorNode(PivotDataNode, "Pivot", Owner.mPivotData.mPivot.value(), glm::vec3(0.0f, 0.0f, 0.0f), true);
					}
					if (Owner.mPivotData.mPivotA.has_value())
					{
						GenerateBymlVectorNode(PivotDataNode, "PivotA", Owner.mPivotData.mPivotA.value(), glm::vec3(0.0f, 0.0f, 0.0f), true);
					}
					if (Owner.mPivotData.mPivotB.has_value())
					{
						GenerateBymlVectorNode(PivotDataNode, "PivotB", Owner.mPivotData.mPivotB.value(), glm::vec3(0.0f, 0.0f, 0.0f), true);
					}
					OwnerNode.AddChild(PivotDataNode);

					OwnerNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Refer", Owner.mRefer));

					if (Owner.mReferPose.mRotate != glm::vec3(0.0f, 0.0f, 0.0f) ||
						Owner.mReferPose.mTranslate != glm::vec3(0.0f, 0.0f, 0.0f))
					{
						application::file::game::byml::BymlFile::Node ReferPoseNode(application::file::game::byml::BymlFile::Type::Dictionary, "ReferPose");

						GenerateBymlVectorNode(ReferPoseNode, "Rotate", glm::radians(Owner.mReferPose.mRotate), glm::vec3(0.0f, 0.0f, 0.0f), true);
						GenerateBymlVectorNode(ReferPoseNode, "Trans", Owner.mReferPose.mTranslate, glm::vec3(0.0f, 0.0f, 0.0f), true);

						OwnerNode.AddChild(ReferPoseNode);
					}

					if(!Owner.mType.empty()) OwnerNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Type", Owner.mType));

					WriteDynamicTypeParameter(OwnerNode, "UserData", Owner.mUserData);

					ConstraintLinkOwnersNode.AddChild(OwnerNode);
				}

				ConstraintLinkNode.AddChild(ConstraintLinkOwnersNode);
			}

			if (!ConstraintLink.mRefers.empty())
			{
				application::file::game::byml::BymlFile::Node ConstraintLinkRefersNode(application::file::game::byml::BymlFile::Type::Array, "Refers");

				for (Phive::ConstraintLink::Refer& Refer : ConstraintLink.mRefers)
				{
					application::file::game::byml::BymlFile::Node ReferNode(application::file::game::byml::BymlFile::Type::Dictionary);

					ReferNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Owner", Refer.mOwner));
					ReferNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Type", Refer.mType));

					ConstraintLinkRefersNode.AddChild(ReferNode);
				}
				ConstraintLinkNode.AddChild(ConstraintLinkRefersNode);
			}

			PhiveRoot.AddChild(ConstraintLinkNode);
		}

		WriteDynamicTypeParameter(PhiveRoot, "Placement", mPhive.mPlacement);

		if (!mPhive.mRails.empty())
		{
			application::file::game::byml::BymlFile::Node Rails(application::file::game::byml::BymlFile::Type::Array, "Rails");

			for (Phive::Rail& Rail : mPhive.mRails)
			{
				application::file::game::byml::BymlFile::Node RailNode(application::file::game::byml::BymlFile::Type::Dictionary);

				RailNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "IsClosed", Rail.mIsClosed));

				if (!Rail.mNodes.empty())
				{
					application::file::game::byml::BymlFile::Node Nodes(application::file::game::byml::BymlFile::Type::Array, "Nodes");

					for (Phive::Rail::Node& RailNode : Rail.mNodes)
					{
						application::file::game::byml::BymlFile::Node RailDict(application::file::game::byml::BymlFile::Type::Dictionary);
						GenerateBymlVectorNode(RailDict, RailNode.mKey, RailNode.mValue, glm::vec3(0.0f, 0.0f, 0.0f), true);
						Nodes.AddChild(RailDict);
					}

					RailNode.AddChild(Nodes);
				}

				if (!Rail.mType.empty())
				{
					RailNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Type", Rail.mType));
				}

				Rails.AddChild(RailNode);
			}

			PhiveRoot.AddChild(Rails);
		}

		if (mPhive.mRopeHeadLink.has_value())
		{
			Phive::RopeLink& RopeHeadLink = mPhive.mRopeHeadLink.value();

			application::file::game::byml::BymlFile::Node RopeHeadLinkNode(application::file::game::byml::BymlFile::Type::Dictionary, "RopeHeadLink");

			RopeHeadLinkNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "ID", RopeHeadLink.mID));

			if (!RopeHeadLink.mOwners.empty())
			{
				application::file::game::byml::BymlFile::Node Owners(application::file::game::byml::BymlFile::Type::Array, "Owners");

				for (uint64_t Owner : RopeHeadLink.mOwners)
				{
					application::file::game::byml::BymlFile::Node OwnerNode(application::file::game::byml::BymlFile::Type::Dictionary);

					OwnerNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Refer", Owner));

					Owners.AddChild(OwnerNode);
				}
				RopeHeadLinkNode.AddChild(Owners);
			}

			if (!RopeHeadLink.mRefers.empty())
			{
				application::file::game::byml::BymlFile::Node Refers(application::file::game::byml::BymlFile::Type::Array, "Refers");

				for (uint64_t Refer : RopeHeadLink.mRefers)
				{
					application::file::game::byml::BymlFile::Node ReferNode(application::file::game::byml::BymlFile::Type::Dictionary);

					ReferNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Owner", Refer));

					Refers.AddChild(ReferNode);
				}
				RopeHeadLinkNode.AddChild(Refers);
			}

			PhiveRoot.AddChild(RopeHeadLinkNode);
		}

		if (mPhive.mRopeTailLink.has_value())
		{
			Phive::RopeLink& RopeHeadLink = mPhive.mRopeTailLink.value();

			application::file::game::byml::BymlFile::Node RopeHeadLinkNode(application::file::game::byml::BymlFile::Type::Dictionary, "RopeTailLink");

			RopeHeadLinkNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "ID", RopeHeadLink.mID));

			if (!RopeHeadLink.mOwners.empty())
			{
				application::file::game::byml::BymlFile::Node Owners(application::file::game::byml::BymlFile::Type::Array, "Owners");

				for (uint64_t Owner : RopeHeadLink.mOwners)
				{
					application::file::game::byml::BymlFile::Node OwnerNode(application::file::game::byml::BymlFile::Type::Dictionary);

					OwnerNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Refer", Owner));

					Owners.AddChild(OwnerNode);
				}
				RopeHeadLinkNode.AddChild(Owners);
			}

			if (!RopeHeadLink.mRefers.empty())
			{
				application::file::game::byml::BymlFile::Node Refers(application::file::game::byml::BymlFile::Type::Array, "Refers");

				for (uint64_t Refer : RopeHeadLink.mRefers)
				{
					application::file::game::byml::BymlFile::Node ReferNode(application::file::game::byml::BymlFile::Type::Dictionary);

					ReferNode.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Owner", Refer));

					Refers.AddChild(ReferNode);
				}
				RopeHeadLinkNode.AddChild(Refers);
			}

			PhiveRoot.AddChild(RopeHeadLinkNode);
		}

		if (!PhiveRoot.GetChildren().empty())
		{
			Node.AddChild(PhiveRoot);
		}
		//Phive - End
		
		WriteDynamicTypeParameter(Node, "Presence", mPresence);
		
		if (!mRails.empty())
		{
			application::file::game::byml::BymlFile::Node RailsNode(application::file::game::byml::BymlFile::Type::Array, "Rails");

			for (Rail& Rail : mRails)
			{
				application::file::game::byml::BymlFile::Node RailNodeRoot(application::file::game::byml::BymlFile::Type::Dictionary);

				RailNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt64, "Dst", Rail.mDest));
				if (!Rail.mGyml.empty()) RailNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Gyaml", Rail.mGyml));
				RailNodeRoot.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::StringIndex, "Name", Rail.mName));

				RailsNode.AddChild(RailNodeRoot);
			}

			Node.AddChild(RailsNode);
		}

		GenerateBymlVectorNode(Node, "Rotate", mRotateUserEdited ? mRotate : mRotateSerializedRadiansSnap, glm::vec3(0.0f, 0.0f, 0.0f), false);
		Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::UInt32, "SRTHash", mSRTHash));
		GenerateBymlVectorNode(Node, "Scale", mScale, glm::vec3(1.0f, 1.0f, 1.0f), false);
		GenerateBymlVectorNode(Node, "Translate", mTranslate, glm::vec3(0.0f, 0.0f, 0.0f), false);

		if(mTurnActorNearEnemy) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Bool, "TurnActorNearEnemy", mTurnActorNearEnemy));
		if(mVersion != -1) Node.AddChild(GenerateBymlNode(application::file::game::byml::BymlFile::Type::Int32, "Version", mVersion));

		return Node;
	}
}