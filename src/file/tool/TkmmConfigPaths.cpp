#include "TkmmConfigPaths.h"

#include <util/FileUtil.h>
#include <util/Logger.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>


namespace application::file::tool
{
	namespace
	{
		bool IsLikelyTotkRomFsRoot(const std::string& AbsoluteRootUtf8)
		{
			return application::util::FileUtil::FileExists(
				AbsoluteRootUtf8 + "/Pack/Bootup.Nin_NX_NVN.pack.zs");
		}

		std::string WeaklyNormalizePathUtf8(std::filesystem::path P)
		{
			std::error_code Ec;
			P = std::filesystem::weakly_canonical(P, Ec);
			if (!Ec)
				return P.string();
			return P.lexically_normal().string();
		}

#ifdef _WIN32
		std::string GetTkConfigPathUtf8()
		{
			const char* Env = std::getenv("LOCALAPPDATA");
			if (!Env || !*Env)
				return {};

			return (std::filesystem::path(Env) / "tkmm2" / "TkConfig.json").string();
		}
#else
		std::string GetTkConfigPathUtf8()
		{
			const char* Home = std::getenv("HOME");
			if (!Home || !*Home)
				return {};

#if defined(__APPLE__)
			return (std::filesystem::path(Home)
				/ "Library/Application Support/tkmm2/TkConfig.json")
				.string();
#else
			return (std::filesystem::path(Home) / ".local/share/tkmm2/TkConfig.json").string();
#endif
		}
#endif

		std::optional<std::string> TryGuessRomFsFromDumpFolder(const std::string& FolderUtf8Trimmed)
		{
			if (FolderUtf8Trimmed.empty())
				return std::nullopt;

			const std::filesystem::path Folder(FolderUtf8Trimmed);

			std::vector<std::filesystem::path> Candidates = { Folder };

			const std::filesystem::path MaybeRomFs = Folder / "romfs";
			if (MaybeRomFs != Folder)
				Candidates.push_back(MaybeRomFs);

			const std::filesystem::path NestedRomFs =
				Folder / "0100f2c0115b6800/romfs";
			Candidates.push_back(NestedRomFs);

			for (const auto& Cand : Candidates)
			{
				const std::string NormalizedRoot = WeaklyNormalizePathUtf8(Cand);
				if (IsLikelyTotkRomFsRoot(NormalizedRoot))
					return NormalizedRoot;
			}

			return std::nullopt;
		}
	} // namespace

	void TkmmConfigPaths::TryPrefillRomFsFromTkmm()
	{
		if (!application::util::FileUtil::gRomFSPath.empty())
			return;

		const std::string ConfigPath = GetTkConfigPathUtf8();
		if (ConfigPath.empty())
			return;

		if (!application::util::FileUtil::FileExists(ConfigPath))
			return;

		std::ifstream Input(ConfigPath, std::ios::binary);
		if (!Input)
			return;

		nlohmann::json Json;
		try
		{
			Input >> Json;
		}
		catch (const std::exception& Ex)
		{
			application::util::Logger::Warning(
				"TkmmConfig",
				"Failed to parse TkConfig.json: %s",
				Ex.what());
			return;
		}

		if (!Json.contains("GameDumpFolderPaths") || !Json["GameDumpFolderPaths"].is_array())
			return;

		for (const auto& Entry : Json["GameDumpFolderPaths"])
		{
			if (!Entry.is_string())
				continue;

			std::string Raw = Entry.get<std::string>();
			const auto NotSpaceFront = [](unsigned char C) {
				return C != ' ' && C != '\t' && C != '\r';
			};
			while (!Raw.empty() && !NotSpaceFront(static_cast<unsigned char>(Raw.front())))
				Raw.erase(Raw.begin());
			while (!Raw.empty() && !NotSpaceFront(static_cast<unsigned char>(Raw.back())))
				Raw.pop_back();

			auto RomFsGuess = TryGuessRomFsFromDumpFolder(Raw);
			if (!RomFsGuess.has_value())
				continue;

			application::util::FileUtil::gRomFSPath = *RomFsGuess;
			application::util::FileUtil::ValidatePaths();
			if (application::util::FileUtil::gPathsValid)
			{
				application::util::Logger::Info(
					"TkmmConfig",
					"Prefilled RomFS from TkMM GameDumpFolderPaths: %s",
					application::util::FileUtil::gRomFSPath.c_str());
				return;
			}

			application::util::FileUtil::gRomFSPath.clear();
		}
	}
}
