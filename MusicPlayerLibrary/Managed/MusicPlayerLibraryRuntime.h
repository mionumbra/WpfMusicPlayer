// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
	// CLR-facing process runtime. It installs CLR-dependent services first and
	// then delegates all native work to NativeLibraryRuntime.
	public ref class MusicPlayerLibraryRuntime abstract sealed
	{
	public:
		static void Initialize(System::Object^ logger);
		static void Shutdown();
		static bool IsInitialized();
	};
}
