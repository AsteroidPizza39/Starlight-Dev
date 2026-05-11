#pragma once

namespace application::file::tool
{
	namespace TkmmConfigPaths
	{
		// When RomFS path is empty, reads %LOCALAPPDATA%/tkmm2/TkConfig.json (Windows),
		// parses GameDumpFolderPaths, uses the first folder that validates as TotK RomFS root.
		void TryPrefillRomFsFromTkmm();
	}
}
