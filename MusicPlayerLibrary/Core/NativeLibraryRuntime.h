// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
	// 草泥马的ncnn非得让我写个入口来管理原生库，否则在CLR销毁的时候直接崩给你看
	class NativeLibraryRuntime final
	{
	public:
		static void Initialize();
		static void Shutdown() noexcept;
		[[nodiscard]] static bool IsInitialized() noexcept;

		NativeLibraryRuntime() = delete;
	};
}
