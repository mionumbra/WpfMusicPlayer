// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Managed/MusicPlayerManaged.h"

namespace
{
	bool HasAudioFormatInfo(
		const MusicPlayerLibrary::AudioFormatInfo& format) noexcept
	{
		return format.channel_type_id !=
				static_cast<int>(MusicPlayerLibrary::AudioChannelMode::Unknown) ||
			format.sample_rate > 0 ||
			format.bit_depth !=
				static_cast<int>(MusicPlayerLibrary::AudioBitDepth::Unknown);
	}

	System::InvalidOperationException^ ToManagedException(const std::exception& exception)
	{
		const std::string message = exception.what();
		return gcnew System::InvalidOperationException(
			gcnew System::String(message.c_str(), 0, static_cast<int>(message.size()),
				System::Text::Encoding::UTF8));
	}
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioChannelType(
	const int channel_type_id)
{
	switch (static_cast<AudioChannelMode>(channel_type_id))
	{
	case AudioChannelMode::System: return "System";
	case AudioChannelMode::Mono: return "Mono";
	case AudioChannelMode::Stereo: return "Stereo";
	case AudioChannelMode::Surround51: return "Surround 5.1";
	case AudioChannelMode::Surround71: return "Surround 7.1";
	case AudioChannelMode::Unknown:
	default:
		return "Unknown channel layout";
	}
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioSampleRate(
	const int sample_rate)
{
	if (sample_rate <= 0)
		return "Unknown sample rate";
	if (sample_rate < 1'000)
	{
		return System::String::Format(
			System::Globalization::CultureInfo::InvariantCulture,
			"{0} Hz", sample_rate);
	}
	return System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0:0.###} kHz", sample_rate / 1'000.0);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioBitDepth(
	const int bit_depth)
{
	if (bit_depth == static_cast<int>(AudioBitDepth::System))
		return "System";
	if (bit_depth < 0)
		return "Unknown bit depth";
	return System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0}-bit", bit_depth);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioFormat(
	const int channel_type_id,
	const int sample_rate,
	const int bit_depth,
	const int bit_rate)
{
	return 
		System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0} / {1} / {2}, {3:f2} kbps",
		FormatAudioSampleRate(sample_rate),
		FormatAudioBitDepth(bit_depth),
		FormatAudioChannelType(channel_type_id),
		bit_rate / 1000.0);
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::FormatAudioFormat(int channel_type_id, int sample_rate,
	int bit_depth)
{
	return 
	System::String::Format(
		System::Globalization::CultureInfo::InvariantCulture,
		"{0} / {1} / {2}",
		FormatAudioSampleRate(sample_rate),
		FormatAudioBitDepth(bit_depth),
		FormatAudioChannelType(channel_type_id));
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged()
{
	event_bridge = new MusicPlayerEventBridge(this);
	native_handle = new MusicPlayer(event_bridge);
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged(int sample_rate)
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		AudioOutputFormat requested{};
		requested.requested_sample_rate = sample_rate;
		native_handle = new MusicPlayer(event_bridge, requested);
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

MusicPlayerLibrary::MusicPlayerManaged::MusicPlayerManaged(
	int sample_rate,
	int channel_mode,
	int bit_depth)
{
	try
	{
		event_bridge = new MusicPlayerEventBridge(this);
		AudioOutputFormat requested{};
		requested.requested_sample_rate = sample_rate;
		requested.requested_channel_mode = static_cast<AudioChannelMode>(channel_mode);
		requested.requested_bit_depth = static_cast<AudioBitDepth>(bit_depth);
		native_handle = new MusicPlayer(event_bridge, requested);
	}
	catch (const std::exception& exception)
	{
		delete event_bridge;
		event_bridge = nullptr;
		native_handle = nullptr;
		throw ToManagedException(exception);
	}
}

void MusicPlayerLibrary::MusicPlayerManaged::check_if_null()
{
	if (!native_handle)
		throw gcnew System::InvalidOperationException("MusicPlayerNative initialization failed!");
}

void MusicPlayerLibrary::MusicPlayerManaged::ProcessEvent(int event_type, Object^ payload)
{
	if (!native_handle)
		return; // 析构后或尚未初始化，安静忽略

	ProcessEventState^ state = gcnew ProcessEventState();
	state->EventType = event_type;
	state->Payload = payload;
	System::Threading::ThreadPool::QueueUserWorkItem(
		gcnew System::Threading::WaitCallback(this, &MusicPlayerManaged::ProcessEventCore), state);
}

void MusicPlayerLibrary::MusicPlayerManaged::ProcessEventCore(Object^ stateObj)
{	
	if (!native_handle || !stateObj)
		return; // native 已被销毁，跳过
	ProcessEventState^ state = safe_cast<ProcessEventState^>(stateObj);
	Object^ encoded_payload = state->Payload;

	switch (static_cast<PlayerMessageType>(state->EventType)) {
	case PlayerMessageType::FileInitialized:
		if (OnPlayerFileInit)
			OnPlayerFileInit();
		break;
	case PlayerMessageType::AlbumArtInitialized:
		if (OnPlayerAlbumArtInit)
			OnPlayerAlbumArtInit(safe_cast<array<System::Byte>^>(encoded_payload));
		break;
	case PlayerMessageType::NcmAlbumArtDownloadRequired:
		if (OnPlayerNcmRequireAlbumArtDownload)
			OnPlayerNcmRequireAlbumArtDownload(safe_cast<System::String^>(encoded_payload));
		break;
	case PlayerMessageType::Started:
		if (OnPlayerStart)
			OnPlayerStart();
		break;
	case PlayerMessageType::Paused:
		if (OnPlayerPause)
			OnPlayerPause();
		break;
	case PlayerMessageType::Stopped:
		if (OnPlayerStop)
			OnPlayerStop();
		break;
	case PlayerMessageType::Destroyed:
		if (OnPlayerDestroy)
			OnPlayerDestroy();
		break;
	case PlayerMessageType::Error:
		if (OnPlayerError)
			OnPlayerError(encoded_payload ? safe_cast<System::Exception^>(encoded_payload) : nullptr);
		break;
	case PlayerMessageType::TimeChanged:
		if (OnPlayerTimeChange) {
			double time = encoded_payload ? safe_cast<double>(state->Payload) : 0.0;
			OnPlayerTimeChange(time);
		}
		break;
	default:
		break;
	}
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsInitialized()
{
	if (!is_native_valid()) return false;
	return native_handle->IsInitialized();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsPlaying()
{
	if (!is_native_valid()) return false;
	return native_handle->IsPlaying();
}

static bool IsValidPath(const std::wstring& path)
{
	if (path.empty())
		return false;

	static constexpr std::wstring_view invalidChars = L"<>\"|?*";
	for (wchar_t invalidChar : invalidChars)
	{
		if (path.find(invalidChar) != std::wstring::npos)
		{
			NATIVE_TRACE("err: invalid character found, char: %c\n", static_cast<char>(invalidChar));
			return false;
		}
	}
	return MusicPlayerLibrary::GetDefaultFileSystem().FileExists(path);
}

void MusicPlayerLibrary::MusicPlayerManaged::OpenFile(const System::String^ fileName)
{
	OpenFile(fileName, false);
}

void MusicPlayerLibrary::MusicPlayerManaged::OpenFile(const System::String^ fileName, bool skipAlbumArtLoading)
{
	check_if_null();
	pin_ptr<const wchar_t> wch = PtrToStringChars(fileName);
	std::wstring nativeFileName(wch);
	if (!IsValidPath(nativeFileName)) {
		throw gcnew System::ArgumentException("file does not exist!");
	}
	auto& default_fs = GetDefaultFileSystem();
	std::wstring extension = default_fs.GetFileExtension(nativeFileName);
	if (!extension.empty() && extension[0] == L'.')
		extension.erase(0, 1);
	try
	{
		native_handle->OpenFile(nativeFileName, extension, skipAlbumArtLoading);
	}
	catch (const std::exception& exception)
	{
		throw ToManagedException(exception);
	}
}

double MusicPlayerLibrary::MusicPlayerManaged::GetMusicTimeLength()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetMusicTimeLength();
}

double MusicPlayerLibrary::MusicPlayerManaged::GetCurrentMusicPosition()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetCurrentMusicPosition();
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetSongTitle()
{
	if (!is_native_valid()) return nullptr;
	std::wstring title = native_handle->GetSongTitle();
	// TODO: 在此处插入 return 语句
	if (title.empty()) return nullptr;
	return gcnew System::String(title.c_str());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetSongArtist()
{
	if (!is_native_valid()) return nullptr;
	std::wstring artist = native_handle->GetSongArtist();
	// TODO: 在此处插入 return 语句
	if (artist.empty()) return nullptr;
	return gcnew System::String(artist.c_str());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetAudioSourceFormat()
{
	if (!is_native_valid()) return nullptr;
	const AudioFormatInfo format = native_handle->GetAudioSourceFormatInfo();
	if (!HasAudioFormatInfo(format)) return nullptr;
	return FormatAudioFormat(
		format.channel_type_id, format.sample_rate, format.bit_depth, native_handle->GetAverageAudioBitrate());
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetDeviceOutputFormat()
{
	if (!is_native_valid()) return nullptr;
	const AudioFormatInfo format = native_handle->GetDeviceOutputFormatInfo();
	return FormatAudioFormat(
		format.channel_type_id, format.sample_rate, format.bit_depth);
}

double MusicPlayerLibrary::MusicPlayerManaged::GetAudioSourceBitrate()
{
	if (!is_native_valid()) return 0.0;
	return native_handle->GetAudioSourceBitrate();
}

int MusicPlayerLibrary::MusicPlayerManaged::GetAverageAudioBitrate()
{
	if (!is_native_valid()) return 0;
	return native_handle->GetAverageAudioBitrate();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsLoselessAudio()
{
	if (!is_native_valid()) return false;
	return native_handle->IsLoselessAudio();
}

bool MusicPlayerLibrary::MusicPlayerManaged::IsHiResAudio()
{
	if (!is_native_valid()) return false;
	return native_handle->IsHiResAudio();
}

void MusicPlayerLibrary::MusicPlayerManaged::Start()
{
	check_if_null();
	native_handle->Start();
}

void MusicPlayerLibrary::MusicPlayerManaged::Pause()
{
	check_if_null();
	native_handle->Pause();
}

void MusicPlayerLibrary::MusicPlayerManaged::Stop()
{
	check_if_null();
	native_handle->Stop();
}

void MusicPlayerLibrary::MusicPlayerManaged::SetMasterVolume(float volume)
{
	check_if_null();
	native_handle->SetMasterVolume(volume);
}

void MusicPlayerLibrary::MusicPlayerManaged::SeekToPosition(double time, bool need_stop)
{
	check_if_null();
	native_handle->SeekToPosition(time, need_stop);
}

int MusicPlayerLibrary::MusicPlayerManaged::GetNBlockAlign()
{
	if (!is_native_valid()) return -1;
	return native_handle->GetNBlockAlign();
}

System::String^ MusicPlayerLibrary::MusicPlayerManaged::GetID3Lyric()
{
	if (!is_native_valid()) return nullptr;
	std::wstring lyric = native_handle->GetID3Lyric();
	// TODO: 在此处插入 return 语句
	return gcnew System::String(lyric.c_str());
}

int MusicPlayerLibrary::MusicPlayerManaged::GetEqualizerBand(int index)
{
	if (!is_native_valid()) return 0;
	return native_handle->GetEqualizerBand(index);
}

void MusicPlayerLibrary::MusicPlayerManaged::SetEqualizerBand(int index, int value)
{
	check_if_null();
	native_handle->SetEqualizerBand(index, value);
}

MusicPlayerLibrary::MusicPlayerManaged::~MusicPlayerManaged()
{
	release_native_resources();
	OnPlayerFileInit = nullptr;
	OnPlayerAlbumArtInit = nullptr;
	OnPlayerStart = nullptr;
	OnPlayerPause = nullptr;
	OnPlayerStop = nullptr;
	OnPlayerTimeChange = nullptr;
	OnPlayerDestroy = nullptr;
	OnPlayerError = nullptr;
	OnPlayerNcmRequireAlbumArtDownload = nullptr;
	System::GC::SuppressFinalize(this);
}

void MusicPlayerLibrary::MusicPlayerManaged::!MusicPlayerManaged()
{
	release_native_resources();
	OnPlayerFileInit = nullptr;
	OnPlayerAlbumArtInit = nullptr;
	OnPlayerStart = nullptr;
	OnPlayerPause = nullptr;
	OnPlayerStop = nullptr;
	OnPlayerTimeChange = nullptr;
	OnPlayerDestroy = nullptr;
	OnPlayerError = nullptr;
	OnPlayerNcmRequireAlbumArtDownload = nullptr;
}

void MusicPlayerLibrary::MusicPlayerManaged::release_native_resources()
{
	MusicPlayer* handle = native_handle;
	native_handle = nullptr;
	delete handle;
	delete event_bridge;
	event_bridge = nullptr;
}

int MusicPlayerLibrary::MusicPlayerManaged::CopyAudioFFTData(array<float>^ destination)
{
	if (!is_native_valid())
		return 0;
	if (!native_handle->fft_executer)
		return 0;
	if (destination == nullptr || destination->Length <= 0)
		return 0;

	pin_ptr<float> destination_ptr = &destination[0];
	return native_handle->fft_executer->CopyAudioFFTData(destination_ptr, destination->Length);
}
