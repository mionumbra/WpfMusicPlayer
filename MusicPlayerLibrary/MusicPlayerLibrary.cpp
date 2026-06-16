#include "pch.h"

#include "MusicPlayerLibrary.h"
#include <atlbase.h>
#include <limits>
#include <msclr/marshal_cppstd.h>
#include <numeric>

using namespace System::Runtime::InteropServices;

static float GetSystemDpiScale()
{
	HDC hdc = ::GetDC(nullptr);
	int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
	::ReleaseDC(nullptr, hdc);
	return static_cast<float>(dpiX) / 96.0f;
}

namespace
{
	enum class ImageResizeMode
	{
		Stretch,
		CenterCrop
	};

	HBITMAP DecodeImageBufferToHBitmap(BYTE* image_data, ULONGLONG image_size, int scale_size, ImageResizeMode resize_mode)
	{
		if (image_data == nullptr || image_size == 0 || scale_size <= 0)
			return nullptr;
		if (image_size > (std::numeric_limits<DWORD>::max)())
		{
			ATLTRACE("err: image buffer is too large for WIC stream\n");
			return nullptr;
		}

		CComPtr<IWICImagingFactory> imaging_factory;
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&imaging_factory));
		if (FAILED(hr) || !imaging_factory)
		{
			ATLTRACE("err: create WIC imaging factory failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		CComPtr<IWICStream> iwic_stream;
		hr = imaging_factory->CreateStream(&iwic_stream);
		if (FAILED(hr) || !iwic_stream)
		{
			ATLTRACE("err: create WIC stream failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		hr = iwic_stream->InitializeFromMemory(image_data, static_cast<DWORD>(image_size));
		if (FAILED(hr))
		{
			ATLTRACE("err: initialize WIC stream from memory failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		CComPtr<IWICBitmapDecoder> bitmap_decoder;
		hr = imaging_factory->CreateDecoderFromStream(iwic_stream, nullptr,
			WICDecodeMetadataCacheOnLoad, &bitmap_decoder);
		if (FAILED(hr) || !bitmap_decoder)
		{
			ATLTRACE("err: create decoder from stream failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		CComPtr<IWICBitmapFrameDecode> source;
		hr = bitmap_decoder->GetFrame(0, &source);
		if (FAILED(hr) || !source)
		{
			ATLTRACE("err: get WIC bitmap frame failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		CComPtr<IWICFormatConverter> iwic_format_converter;
		hr = imaging_factory->CreateFormatConverter(&iwic_format_converter);
		if (FAILED(hr) || !iwic_format_converter)
		{
			ATLTRACE("err: create WIC format converter failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		hr = iwic_format_converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
			WICBitmapDitherTypeNone, nullptr, 0.f,
			WICBitmapPaletteTypeCustom);
		if (FAILED(hr))
		{
			ATLTRACE("err: initialize WIC format converter failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		UINT width = 0;
		UINT height = 0;
		hr = source->GetSize(&width, &height);
		if (FAILED(hr) || width == 0 || height == 0)
		{
			ATLTRACE("err: get WIC bitmap size failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		CComPtr<IWICBitmapClipper> clipper;
		CComPtr<IWICBitmapScaler> scaler;
		hr = imaging_factory->CreateBitmapScaler(&scaler);
		if (FAILED(hr) || !scaler)
		{
			ATLTRACE("err: create WIC bitmap scaler failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		if (resize_mode == ImageResizeMode::CenterCrop)
		{
			const double scale_x = static_cast<double>(scale_size) / width;
			const double scale_y = static_cast<double>(scale_size) / height;
			const double scale = scale_x > scale_y ? scale_x : scale_y;

			const UINT crop_width = static_cast<UINT>(scale_size / scale);
			const UINT crop_height = static_cast<UINT>(scale_size / scale);
			const UINT crop_x = (width - crop_width) / 2;
			const UINT crop_y = (height - crop_height) / 2;

			ATLTRACE("info: center crop - src(%u %u), crop(%u %u) at (%u %u)\n",
				width, height, crop_width, crop_height, crop_x, crop_y);

			hr = imaging_factory->CreateBitmapClipper(&clipper);
			if (FAILED(hr) || !clipper)
			{
				ATLTRACE("err: create WIC bitmap clipper failed, hr=0x%08lx\n", hr);
				return nullptr;
			}

			const WICRect crop_rect = { static_cast<INT>(crop_x), static_cast<INT>(crop_y),
								  static_cast<INT>(crop_width), static_cast<INT>(crop_height) };
			hr = clipper->Initialize(iwic_format_converter, &crop_rect);
			if (FAILED(hr))
			{
				ATLTRACE("err: initialize WIC bitmap clipper failed, hr=0x%08lx\n", hr);
				return nullptr;
			}

			hr = scaler->Initialize(clipper, scale_size, scale_size, WICBitmapInterpolationModeFant);
		}
		else
		{
			hr = scaler->Initialize(iwic_format_converter, scale_size, scale_size, WICBitmapInterpolationModeFant);
		}
		if (FAILED(hr))
		{
			ATLTRACE("err: initialize WIC bitmap scaler failed, hr=0x%08lx\n", hr);
			return nullptr;
		}

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = scale_size;
		bmi.bmiHeader.biHeight = -scale_size; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		const UINT stride = scale_size * 4;
		const UINT buffer_size = stride * scale_size;
		BYTE* image_bits = nullptr;
		HDC hdc_screen = GetDC(nullptr);
		HBITMAP bitmap = CreateDIBSection(hdc_screen, &bmi, DIB_RGB_COLORS,
			reinterpret_cast<void**>(&image_bits), nullptr, 0);
		ReleaseDC(nullptr, hdc_screen);
		if (!bitmap || image_bits == nullptr)
		{
			ATLTRACE("err: create album art DIB section failed\n");
			return nullptr;
		}

		hr = scaler->CopyPixels(nullptr, stride, buffer_size, image_bits);
		if (FAILED(hr))
		{
			ATLTRACE("err: copy WIC pixels failed, hr=0x%08lx\n", hr);
			DeleteObject(bitmap);
			return nullptr;
		}

		return bitmap;
	}
}

int MusicPlayerLibrary::MusicPlayerNative::read_func(uint8_t* buf, int buf_size) {
	// ATLTRACE("info: read buf_size=%d, rest=%lld\n", buf_size, file_stream->GetLength() - file_stream->GetPosition());
	// reset file_stream_end
	file_stream_end = false;
	int gcount = static_cast<int>(file_stream->Read(buf, buf_size));
	if (gcount == 0) {
		file_stream_end = true;
		return -1;
	}
	return gcount;
}

int MusicPlayerLibrary::MusicPlayerNative::read_func_wrapper(void* opaque, uint8_t* buf, int buf_size)
{
	auto callObject = reinterpret_cast<MusicPlayerNative*>(opaque);
	return callObject->read_func(buf, buf_size);
}

int64_t MusicPlayerLibrary::MusicPlayerNative::seek_func(int64_t offset, int whence)
{
	FileSeekOrigin origin;
	switch (whence) {
	case AVSEEK_SIZE: return static_cast<int64_t>(file_stream->GetLength());
	case SEEK_SET: origin = FileSeekOrigin::Begin; break;
	case SEEK_CUR: origin = FileSeekOrigin::Current; break;
	case SEEK_END: origin = FileSeekOrigin::End; break;
	default: return -1; // unsupported
	}
	ULONGLONG pos = file_stream->Seek(offset, origin);
	return static_cast<int64_t>(pos);
}

int64_t MusicPlayerLibrary::MusicPlayerNative::seek_func_wrapper(void* opaque, int64_t offset, int whence)
{
	auto callObject = reinterpret_cast<MusicPlayerLibrary::MusicPlayerNative*>(opaque);
	return callObject->seek_func(offset, whence);
}

inline int MusicPlayerLibrary::MusicPlayerNative::load_audio_context(const CString& audio_filename, const CString& file_extension_in)
{
	// 打开文件流
	// std::ios::sync_with_stdio(false);
	const std::wstring audio_file_path(audio_filename.GetString());
	file_stream = GetDefaultFileSystem().OpenReadFile(audio_file_path, true, false);
	bool is_ncm = false;
	file_extension = file_extension_in;
	if (!file_stream)
	{
		ATLTRACE("err: file not exists!\n");
		return -1;
	}
	char magic[10];
	if (const int ret = file_stream->Read(magic, 8); ret != 8)
	{
		ATLTRACE("err: failed to read magic bytes\n");
		file_stream.reset();
		return -1;
	}
	magic[9] = '\0';
	ATLTRACE("info: magic bytes: %s\n", magic);
	if (CStringA(magic) == "CTENFDAM")
	{
		ATLTRACE("info: found ncm header\n");
		is_ncm = true;
	}
	file_stream->SeekToBegin();

	if (file_extension_in.CompareNoCase(_T("ncm")) == 0 || is_ncm)
	{
		try
		{
			std::vector<uint8_t> file_data;
			DWORD file_size = 0;
			file_stream->SeekToBegin();
			file_size = static_cast<DWORD>(file_stream->GetLength());
			file_data.resize(file_size);
			file_stream->Read(file_data.data(), static_cast<UINT>(file_stream->GetLength()));
			file_stream->SeekToBegin();
			NcmDecryptor decryptor(file_data, audio_filename);
			auto decryptor_result = decryptor.Decrypt();
			file_stream->Close();
			file_stream.reset();
			auto mem_file = GetDefaultFileSystem().CreateMemoryFile();
			if (!mem_file)
				return -1;
			mem_file->Write(decryptor_result.audioData.data(), static_cast<UINT>(decryptor_result.audioData.size()));
			mem_file->SeekToBegin();
			file_stream = std::move(mem_file);
			download_ncm_album_art_async(decryptor_result.pictureUrl, static_cast<int>(500.f * GetSystemDpiScale()));
		}
		catch (std::exception& e)
		{
			ATLTRACE("err: decrypt ncm failed: %s\n", e.what());
			ATLTRACE("err: this can be caused by ncm algorithm update, or ncm file corrupt\n");
			ATLTRACE("err: please try to report ncm file to issues\n");
			file_stream.reset();
			return -1;
		}
		// create a new memory buffer managed by file stream
	}
	return load_audio_context_from_file_stream();
}

int MusicPlayerLibrary::MusicPlayerNative::load_audio_context_from_file_stream()
{
	if (!file_stream)
		return -1;

	// 重置文件流指针，防止读取后未复位
	file_stream->SeekToBegin();
	char* buf = DBG_NEW char[1024];
	memset(buf, 0, sizeof(char) * 1024);

	// 取得文件大小
	format_context = avformat_alloc_context();
	size_t file_len = static_cast<int64_t>(file_stream->GetLength());
	ATLTRACE("info: file loaded, size = %zu\n", file_len);

	constexpr size_t avio_buf_size = 8192;


	buffer = reinterpret_cast<unsigned char*>(av_malloc(avio_buf_size));
	avio_context =
		avio_alloc_context(buffer, avio_buf_size, 0,
			this,
			&read_func_wrapper,
			nullptr,
			&seek_func_wrapper);

	format_context->pb = avio_context;

	// 打开音频文件
	int res = avformat_open_input(&format_context,
		nullptr, // dummy parameter, read from memory stream
		nullptr, // let ffmpeg auto detect format
		nullptr  // no parateter specified
	);
	if (res < 0) {
		av_strerror(res, buf, 1024);
		ATLTRACE("err: avformat_open_input failed: %s\n", buf);
		return -1;
	}
	if (!format_context)
	{
		av_strerror(res, buf, 1024);
		ATLTRACE("err: avformat_open_input failed, reason = %s(%d)\n", buf, res);
		release_audio_context();
		delete[] buf;
		return -1;
	}

	res = avformat_find_stream_info(format_context, nullptr);
	if (res == AVERROR_EOF)
	{
		ATLTRACE("err: no stream found in file\n");
		release_audio_context();
		delete[] buf;
		return -1;
	}
	ATLTRACE("info: stream count %d\n", format_context->nb_streams);
	audio_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_AUDIO, -1, -1, const_cast<const AVCodec**>(&codec), 0);
	if (audio_stream_index < 0) {
		ATLTRACE("err: no audio stream found\n");
		release_audio_context();
		delete[] buf;
		return -1;
	}

	AVStream* current_stream = format_context->streams[audio_stream_index];
	codec = const_cast<AVCodec*>(avcodec_find_decoder(current_stream->codecpar->codec_id));
	if (!codec)
	{
		ATLTRACE("warn: no valid decoder found, stream id = %d!\n", audio_stream_index);
		release_audio_context();
		delete[] buf;
		return -1;
	}

	ATLTRACE("info: open stream id %d, format=%d, sample_rate=%d, channels=%d, channel_layout=%d\n",
		audio_stream_index,
		current_stream->codecpar->format,
		current_stream->codecpar->sample_rate,
		current_stream->codecpar->ch_layout.nb_channels,
		current_stream->codecpar->ch_layout.order);

	int image_stream_id = -1;

	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		if (AVStream* stream = format_context->streams[i]; stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
			ATLTRACE("info: open stream id %d read attaching pic\n", i);
			image_stream_id = static_cast<int>(i);
			break;
		}
	}

	if (image_stream_id != -1) {
		album_art = decode_id3_album_art(image_stream_id, static_cast<int>(500.0f * GetSystemDpiScale()));
	}

	if (this->file_extension != _T("ncm"))
	{
		managed_music_player->ProcessEvent(WM_PLAYER_ALBUM_ART_INIT, reinterpret_cast<WPARAM>(album_art), 0);
		album_art = nullptr; // ownership transferred to async event handler
	}
	read_metadata();

	// 从0ms开始读取
	avformat_seek_file(format_context, -1, INT64_MIN, 0, INT64_MAX, 0);
	// codec is not null
	// 建立解码器上下文
	codec_context = avcodec_alloc_context3(codec);
	if (codec_context == nullptr)
	{
		ATLTRACE("err: avcodec_alloc_context3 failed\n");
		release_audio_context();
		delete[] buf;
		return -1;
	}
	avcodec_parameters_to_context(codec_context, format_context->streams[audio_stream_index]->codecpar);

	// 降低错误容忍度
	codec_context->err_recognition = AV_EF_IGNORE_ERR | AV_EF_COMPLIANT;
	// 错误隐藏
	codec_context->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
	// 跳过坏帧
	codec_context->skip_frame = AVDISCARD_NONREF;

	// 解码文件
	codec_context->request_sample_fmt = AV_SAMPLE_FMT_S32P;
	res = avcodec_open2(codec_context, codec, nullptr);
	if (res)
	{
		av_strerror(res, buf, 1024);
		ATLTRACE("err: avcodec_open2 failed, reason = %s\n", buf);
		release_audio_context();
		delete[] buf;
		return -1;
	}

	// avoid ffmpeg warning
	codec_context->pkt_timebase = format_context->streams[audio_stream_index]->time_base;
	// set parallel decode (flac, wav..
	av_opt_set_int(codec_context, "threads", 0, 0);

	// init avaudiofifo with the same format the filter graph feeds to XAudio2.
	AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
	fifo_audio_channels = stereo_layout.nb_channels;
	fifo_audio_sample_fmt = AV_SAMPLE_FMT_S16;
	fifo_sample_rate = sample_rate;
	if (!audio_fifo) {
		res = initialize_audio_fifo(fifo_audio_sample_fmt,
			fifo_audio_channels,
			1024); // initial size
		if (res < 0) {
			ATLTRACE("err: initialize_audio_fifo failed\n");
			release_audio_context();
			delete[] buf;
			return -1;
		}
	}
	delete[] buf;

	// init decoder
	frame = av_frame_alloc();
	filt_frame = av_frame_alloc();
	packet = av_packet_alloc();

	reset_av_filter_equalizer();
	if (init_av_filter_equalizer() < 0)
	{
		uninitialize_audio_engine();
		release_audio_context();
		return -1;
	}

	init_decoder_thread();
	return 0;
}

void MusicPlayerLibrary::MusicPlayerNative::release_audio_context()
{
	if (album_art)
	{
		DeleteObject(album_art);
		album_art = nullptr;
	}
	if (avio_context)
	{
		// 释放缓冲区上下文
		avio_context_free(&avio_context);
		avio_context = nullptr;
	}
	if (format_context)
	{
		// 释放文件解析上下文
		avformat_close_input(&format_context);
		format_context = nullptr;
	}

	if (codec_context)
	{
		// 释放解码器上下文
		avcodec_free_context(&codec_context);
		codec_context = nullptr;
	}
	uninitialize_audio_fifo();
	if (file_stream)
	{
		file_stream.reset();
	}
}

void MusicPlayerLibrary::MusicPlayerNative::reset_audio_context()
{
	// release_audio_context();
	file_stream_end = false;
	if (is_audio_context_initialized()) {
		stop_audio_decode();
		av_seek_frame(format_context, static_cast<int>(audio_stream_index), 0, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(codec_context);
		// 重置滤镜图
		ATLTRACE("info: audio context reset, rebuilding filter graph\n");
		reset_av_filter_equalizer();
		if (init_av_filter_equalizer() < 0)
		{
			playback_state.store(audio_playback_state_stopped);
			return;
		}
	}
	playback_state.store(audio_playback_state_init);
	reset_audio_fifo();
	init_decoder_thread();
	// load_audio_context_from_file_stream();
}

bool MusicPlayerLibrary::MusicPlayerNative::is_audio_context_initialized()
{
	return avio_context
		&& format_context
		&& codec_context
		&& file_stream;
}

HBITMAP MusicPlayerLibrary::MusicPlayerNative::download_ncm_album_art(const CString& url, int scale_size)
{
	if (url.IsEmpty()) return nullptr;

	auto file = GetDefaultFileSystem().CreateMemoryFile();
	if (!file)
		return nullptr;
	ULONGLONG totalBytesRead = 0;
	HINTERNET hSession = nullptr;
	HINTERNET hConnect = nullptr;
	HINTERNET hRequest = nullptr;
	auto close_winhttp_handles = [&]()
	{
		if (hRequest)
		{
			WinHttpCloseHandle(hRequest);
			hRequest = nullptr;
		}
		if (hConnect)
		{
			WinHttpCloseHandle(hConnect);
			hConnect = nullptr;
		}
		if (hSession)
		{
			WinHttpCloseHandle(hSession);
			hSession = nullptr;
		}
	};

	bool downloadSucceeded = false;
	do
	{
		URL_COMPONENTS urlComponents = {};
		urlComponents.dwStructSize = sizeof(urlComponents);
		urlComponents.dwSchemeLength = static_cast<DWORD>(-1);
		urlComponents.dwHostNameLength = static_cast<DWORD>(-1);
		urlComponents.dwUrlPathLength = static_cast<DWORD>(-1);
		urlComponents.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(url.GetString(), static_cast<DWORD>(url.GetLength()), 0, &urlComponents))
		{
			ATLTRACE("err: crack album art url failed, gle=%lu\n", GetLastError());
			break;
		}

		if (urlComponents.nScheme != INTERNET_SCHEME_HTTP && urlComponents.nScheme != INTERNET_SCHEME_HTTPS)
		{
			ATLTRACE("err: unsupported album art url scheme=%d\n", urlComponents.nScheme);
			break;
		}

		CString host(urlComponents.lpszHostName, static_cast<int>(urlComponents.dwHostNameLength));
		CString requestPath(urlComponents.lpszUrlPath, static_cast<int>(urlComponents.dwUrlPathLength));
		if (requestPath.IsEmpty())
			requestPath = _T("/");
		if (urlComponents.dwExtraInfoLength > 0)
			requestPath += CString(urlComponents.lpszExtraInfo, static_cast<int>(urlComponents.dwExtraInfoLength));

		ATLTRACE("info: establishing connection with ncm server\n");
		hSession = WinHttpOpen(
			_T("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36"),
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0);
		if (!hSession)
		{
			ATLTRACE("err: open winhttp session failed, gle=%lu\n", GetLastError());
			break;
		}

		hConnect = WinHttpConnect(hSession, host.GetString(), urlComponents.nPort, 0);
		if (!hConnect)
		{
			ATLTRACE("err: connect album art host failed, gle=%lu\n", GetLastError());
			break;
		}

		DWORD requestFlags = urlComponents.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
		hRequest = WinHttpOpenRequest(
			hConnect,
			_T("GET"),
			requestPath.GetString(),
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			requestFlags);
		if (!hRequest)
		{
			ATLTRACE("err: open album art request failed, gle=%lu\n", GetLastError());
			break;
		}

		static constexpr TCHAR cacheHeaders[] = _T("Cache-Control: no-cache\r\nPragma: no-cache\r\n");
		if (!WinHttpSendRequest(
			hRequest,
			cacheHeaders,
			static_cast<DWORD>(_tcslen(cacheHeaders)),
			WINHTTP_NO_REQUEST_DATA,
			0,
			0,
			0))
		{
			ATLTRACE("err: send album art request failed, gle=%lu\n", GetLastError());
			break;
		}

		if (!WinHttpReceiveResponse(hRequest, nullptr))
		{
			ATLTRACE("err: receive album art response failed, gle=%lu\n", GetLastError());
			break;
		}

		DWORD statusCode = 0;
		DWORD statusCodeSize = sizeof(statusCode);
		if (WinHttpQueryHeaders(
			hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&statusCode,
			&statusCodeSize,
			WINHTTP_NO_HEADER_INDEX)
			&& (statusCode < 200 || statusCode >= 300))
		{
			ATLTRACE("err: album art response status=%lu\n", statusCode);
			break;
		}

		BYTE buf[4096];
		DWORD nRead = 0;
		bool readSucceeded = true;
		do
		{
			if (!WinHttpReadData(hRequest, buf, sizeof(buf), &nRead))
			{
				ATLTRACE("err: read album art response failed, gle=%lu\n", GetLastError());
				readSucceeded = false;
				nRead = 0;
				break;
			}

			if (nRead == 0)
				break;

			totalBytesRead += nRead;
			file->Write(buf, nRead);
		} while (nRead > 0);

		downloadSucceeded = readSucceeded && totalBytesRead > 0;
	} while (false);

	close_winhttp_handles();
	if (!downloadSucceeded)
	{
		return nullptr;
	}

	ATLTRACE("info: downloaded %llu bytes\n", totalBytesRead);

	file->SeekToBegin();
	void* pBufStart = nullptr;
	void* pBufMax = nullptr;
	if (!file->GetReadBuffer(&pBufStart, &pBufMax))
	{
		ATLTRACE("err: get album art memory buffer failed\n");
		return nullptr;
	}
	HBITMAP bmp = DecodeImageBufferToHBitmap(
		static_cast<BYTE*>(pBufStart),
		file->GetLength(),
		scale_size,
		ImageResizeMode::Stretch);
	return bmp;
}

HBITMAP MusicPlayerLibrary::MusicPlayerNative::decode_id3_album_art(const int stream_index, int scale_size)
{
	if (!format_context) return nullptr;

	// stream_index = attached pic
	AVPacket pkt = format_context->streams[stream_index]->attached_pic;
	return DecodeImageBufferToHBitmap(
		pkt.data,
		static_cast<ULONGLONG>(pkt.size),
		scale_size,
		ImageResizeMode::CenterCrop);
}

void MusicPlayerLibrary::MusicPlayerNative::download_ncm_album_art_async(const CString& url, int scale_size)
{
	if (album_art_worker_thread.joinable())
	{
		album_art_worker_thread.request_stop();
		album_art_worker_thread.join();
	}

	album_art_worker_thread = std::jthread([this, url, scale_size](std::stop_token stop_token)
		{
			if (stop_token.stop_requested())
				return;

			HBITMAP bitmap = download_ncm_album_art(url, scale_size);
			if (stop_token.stop_requested())
			{
				if (bitmap)
					DeleteObject(bitmap);
				return;
			}

			managed_music_player->ProcessEvent(WM_PLAYER_ALBUM_ART_INIT, reinterpret_cast<WPARAM>(bitmap), 0);
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_ALBUM_ART_INIT, reinterpret_cast<WPARAM>(bitmap));
		});
}

void MusicPlayerLibrary::MusicPlayerNative::read_metadata()
{
	auto convert_utf8 = [](const char* utf_8_str) {
		int len = MultiByteToWideChar(CP_UTF8, 0, utf_8_str, -1, nullptr, 0);
		CStringW wtitle;
		wchar_t* wtitle_raw_buffer = wtitle.GetBufferSetLength(len);
		MultiByteToWideChar(CP_UTF8, 0, utf_8_str, -1, wtitle_raw_buffer, len);
		wtitle.ReleaseBuffer();
		return wtitle;
		};
	auto read_metadata_iter = [&](AVDictionaryEntry* tag) {
		CString key = convert_utf8(tag->key);
		CString value = convert_utf8(tag->value);
		ATLTRACE(_T("info: key %s = %s\n"), key.GetString(), value.GetString());
		if (!key.CompareNoCase(_T("title")) && song_title.IsEmpty()) {
			song_title = value;
			ATLTRACE(_T("info: song title: %s\n"), song_title.GetString());
		}
		else if (!key.CompareNoCase(_T("artist")) && song_artist.IsEmpty()) {
			song_artist = value;
			ATLTRACE(_T("info: song artist: %s\n"), song_artist.GetString());
		}
		else
		{
			key.MakeLower();
			if (key.Find(_T("lyric")) != -1)
			{
				this->id3_string_lyric = value;
				ATLTRACE("info: song contains lyric in metadata\n");
			}
		}
	};

	AVDictionaryEntry* tag = nullptr;
	while ((tag = av_dict_get(format_context->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
		read_metadata_iter(tag);
	}

	// decode album title & artist
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream* stream = format_context->streams[i];
		tag = nullptr;
		while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
			read_metadata_iter(tag);
		}
	}
}

// playback area
bool MusicPlayerLibrary::MusicPlayerNative::wait_frame_ready(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_ready_cv.wait_for(lock, timeout, [this]
		{
			return frame_ready_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_ready_requested = false;
	return true;
}

bool MusicPlayerLibrary::MusicPlayerNative::wait_frame_underrun(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(frame_event_mutex);
	if (!frame_underrun_cv.wait_for(lock, timeout, [this]
		{
			return frame_underrun_requested || playback_state.load() == audio_playback_state_stopped;
		}))
	{
		return false;
	}
	frame_underrun_requested = false;
	return true;
}

void MusicPlayerLibrary::MusicPlayerNative::notify_frame_ready()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
	}
	frame_ready_cv.notify_one();
}

void MusicPlayerLibrary::MusicPlayerNative::notify_frame_underrun()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_underrun_requested = true;
	}
	frame_underrun_cv.notify_one();
}

void MusicPlayerLibrary::MusicPlayerNative::reset_frame_notifications()
{
	std::lock_guard lock(frame_event_mutex);
	frame_ready_requested = false;
	frame_underrun_requested = false;
}

void MusicPlayerLibrary::MusicPlayerNative::notify_all_frame_notifications()
{
	{
		std::lock_guard lock(frame_event_mutex);
		frame_ready_requested = true;
		frame_underrun_requested = true;
	}
	frame_ready_cv.notify_all();
	frame_underrun_cv.notify_all();
}

inline int MusicPlayerLibrary::MusicPlayerNative::initialize_audio_engine()
{
	if (!codec_context)
		return -1;

	// COM init in CMFCMusicPlayerLibrary::MusicPlayerNative.cpp

	// create com obj
	if (FAILED(XAudio2Create(&xaudio2)))
	{
		ATLTRACE("err: create xaudio2 com object failed\n");
		uninitialize_audio_engine();
		return -1;
	}

	// create mastering voice
	if (FAILED(xaudio2->CreateMasteringVoice(&mastering_voice,
		XAUDIO2_DEFAULT_CHANNELS,
		XAUDIO2_DEFAULT_SAMPLERATE,
		0, nullptr, nullptr,
		AudioCategory_GameMedia))) {
		ATLTRACE("err: creating mastering voice failed\n");
		uninitialize_audio_engine();
		return -1;
	}


	// 创建source voice
	// TODO: customizable output rate
	wfx.wFormatTag = WAVE_FORMAT_PCM;                     // pcm格式
	wfx.nChannels = 2;                                    // 音频通道数
	wfx.nSamplesPerSec = sample_rate;                           // 采样率
	wfx.wBitsPerSample = 16;  // xaudio2支持16-bit pcm，如果不符合格式的音频，使用swscale进行转码
	wfx.nBlockAlign = (wfx.wBitsPerSample / 8) * wfx.nChannels; // 样本大小：样本大小(16-bit)*通道数
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign; // 每秒钟解码多少字节，样本大小*采样率
	wfx.cbSize = sizeof(wfx);
	if (FAILED(xaudio2->CreateSourceVoice(&source_voice, &wfx, XAUDIO2_VOICE_NOPITCH)))
	{
		ATLTRACE("err: create source voice failed\n");
		uninitialize_audio_engine();
		return -1;
	}

	last_frametime = 0.0;
	standard_frametime = xaudio2_play_frame_size * 1.0 / wfx.nSamplesPerSec * 1000; // in ms
	playback_state.store(audio_playback_state_init);
	// init FFTExecuter
	try
	{
		fft_executer = new FFTExecuter(wfx.nSamplesPerSec);
	}
	catch (const std::exception& e)
	{
		ATLTRACE("err: create fft executer failed, reason=%s\n", e.what());
		uninitialize_audio_engine();
		return -1;
	}
	
	return 0;
}

inline void MusicPlayerLibrary::MusicPlayerNative::uninitialize_audio_engine()
{
	// 等待xaudio线程执行完成
	if (audio_player_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		audio_player_worker_thread.request_stop();
		notify_all_frame_notifications();
		audio_player_worker_thread.join();
	}
	if (source_voice) {
		UNREFERENCED_PARAMETER(source_voice->Stop(0));
		UNREFERENCED_PARAMETER(source_voice->FlushSourceBuffers());
		source_voice->DestroyVoice();
		source_voice = nullptr;
	}
	if (mastering_voice) {
		mastering_voice->DestroyVoice();
		mastering_voice = nullptr;
	}
	if (xaudio2) {
		xaudio2->Release();
		xaudio2 = nullptr;
	}
	if (frame) {
		av_frame_free(&frame);
		frame = nullptr;
	}
	if (filt_frame)
	{
		av_frame_free(&filt_frame);
		filt_frame = nullptr;
	}
	if (packet) {
		av_packet_free(&packet);
		packet = nullptr;
	}
	if (fft_executer)
	{
		delete fft_executer;
		fft_executer = nullptr;
	}
	// release xaudio2 buffer
	xaudio2_free_buffer();
	xaudio2_destroy_buffer();
}

void MusicPlayerLibrary::MusicPlayerNative::audio_playback_worker_thread()
{
	HRESULT hr;
	XAUDIO2_VOICE_STATE state;
	double decode_time_ms = 0.0;

	while (true) {
		decode_time_ms = 0.0;
		if (!wait_frame_ready(std::chrono::milliseconds(1))) {
			// check flag
			int cached_sample_size = get_audio_fifo_cached_samples_size();
			if (playback_state.load() == audio_playback_state_stopped) {
				break;
			}
			if (playback_state.load() == audio_playback_state_init ||
				playback_state.load() == audio_playback_state_decoder_exit_pre_stop ||
				cached_sample_size > xaudio2_play_frame_size * 32) {
				// pass
				if (cached_sample_size < xaudio2_play_frame_size * 256) {
					notify_frame_underrun();
				}
			}
			else if (file_stream_end) {
				ATLTRACE("info: decode stopped, fetch from fifo\n");
				notify_frame_ready(); // avoid deadlock
			}
			else {
				notify_frame_underrun();
				continue;
			}
		}
		// clock_t decode_begin_time = clock();

		std::lock_guard playback_lock(audio_playback_mutex);

		int fifo_size = get_audio_fifo_cached_samples_size();
		if (fifo_size < 0 && decoder_is_running) {
			// LeaveCriticalSection(audio_playback_section);
			Sleep(1);
			continue;
		}
		if (playback_state.load() == audio_playback_state_decoder_exit_pre_stop) {
			// bypass
		}
		else if (!decoder_is_running && fifo_size == 0) {
			ATLTRACE("info: decoder stopped and fifo empty, ending playback thread\n");
			playback_state.store(audio_playback_state_decoder_exit_pre_stop);
			continue;
		}
		// if (fifo_size < xaudio2_play_frame_size) {
		// 	SetEvent(frame_underrun_event);
		// 	LeaveCriticalSection(audio_playback_section);
		//	Sleep(1);
		//	continue;
		// }
//			playback_state->store(audio_playback_state_stopped);


		source_voice->GetState(&state);
		if (user_request_stop.load()) {
			// immediate return
			ATLTRACE("info: user request stop, do cleaning\n");

			base_offset = state.SamplesPlayed;
			break;
		}
		if (playback_state.load() ==
			audio_playback_state_decoder_exit_pre_stop)
		{

			if (fifo_size == 0 && state.BuffersQueued > 0)
			{
				ATLTRACE("info: file stream ended, waiting for xaudio2 flush buffer\n");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				source_voice->GetState(&state);
				elapsed_time = static_cast<float>(state.SamplesPlayed - base_offset) * 1.0f / static_cast<float>(wfx.nSamplesPerSec) + static_cast<float>(pts_seconds);
				ATLTRACE("info: samples played=%lld, elapsed time=%lf\n",
					state.SamplesPlayed, elapsed_time);

				UINT32 raw = *reinterpret_cast<UINT32*>(&elapsed_time);
				managed_music_player->ProcessEvent(WM_PLAYER_TIME_CHANGE, raw, 0);
				// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
				continue;
			}
			else
			{
				ATLTRACE("info: playback finished, destroying thread\n");
				managed_music_player->ProcessEvent(WM_PLAYER_STOP, 0, 0);
				// AfxGetMainWnd()->PostMessage(WM_PLAYER_STOP);
				base_offset = state.SamplesPlayed;
				xaudio2_played_samples = 0;
				xaudio2_played_buffers = 0;
				// fix pts_seconds not clear up -> ui thread time error & resume failed
				pts_seconds = 0.0;
				playback_state.store(audio_playback_state_stopped);
				// elapsed_time = 0.0;
				// UINT32 raw = *reinterpret_cast<UINT32*>(&elapsed_time);
				// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
				// EnterCriticalSection(audio_playback_section);
				// bool need_clean = !user_request_stop;
				// LeaveCriticalSection(audio_playback_section);
				// if (need_clean)
				// 	reset_audio_context();
				break; // 读取结束
			}
		}

		// Read XAudio2-ready PCM directly from the FIFO. The filter graph already
		// converts to stereo/s16/sample_rate, so sample counts are in output frames.
		uint8_t** fifo_buf = nullptr;
		int read_samples = 0;
		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			fifo_size = get_audio_fifo_cached_samples_size();
			if (fifo_size <= 0)
			{
				notify_frame_underrun();
				continue;
			}
			if (decoder_is_running && !file_stream_end && fifo_size < xaudio2_play_frame_size)
			{
				notify_frame_underrun();
				continue;
			}
			const int fifo_read_size = (std::min)(xaudio2_play_frame_size, fifo_size);
			fifo_buf = reinterpret_cast<uint8_t**>(av_calloc(fifo_audio_channels, sizeof(uint8_t*)));
			if (!fifo_buf)
			{
				playback_state.store(audio_playback_state_stopped);
				break;
			}
			if (int alloc_ret = av_samples_alloc(fifo_buf, nullptr, fifo_audio_channels, fifo_read_size, fifo_audio_sample_fmt, 0);
				alloc_ret < 0) {
				FFMPEG_CRITICAL_ERROR(alloc_ret);
				av_free(fifo_buf);
				playback_state.store(audio_playback_state_stopped);
				break;
			}
			read_samples = read_samples_from_fifo(fifo_buf, fifo_read_size);
			if (read_samples < 0) {
				ATLTRACE("err: read samples from fifo failed, code=%d\n", read_samples);
				ATLTRACE("err: fifo size=%d", get_audio_fifo_cached_samples_size());
				if (user_request_stop.load())
				{
					ATLTRACE("info: user request stop and fifo cleared up, exiting\n");
					av_freep(&fifo_buf[0]);
					av_free(fifo_buf);
					break;
				}
				FFMPEG_CRITICAL_ERROR(read_samples);
				av_freep(&fifo_buf[0]);
				av_free(fifo_buf);
				// LeaveCriticalSection(audio_fifo_section);
				// LeaveCriticalSection(audio_playback_section);
				playback_state.store(audio_playback_state_stopped);
				break;
			}
		}

		if (read_samples == 0)
		{
			ATLTRACE("info: no samples read, spin wait instead\n");
			av_freep(&fifo_buf[0]);
			av_free(fifo_buf);
			Sleep(5); // wait for producing buffer
			continue;
		}
		const UINT32 audio_bytes = read_samples * wfx.nBlockAlign;
		// samples read
		// remove callback func because 100% causes audio lag
		// submit to FFTExecuter directly
		if (fft_executer)
		{
			fft_executer->AddSamplesToRingBuffer(
				fifo_buf[0],
				static_cast<int>(audio_bytes));
		}


		while (state.BuffersQueued >= 64)
		{
			if (user_request_stop.load())
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			source_voice->GetState(&state);
		}
		if (user_request_stop.load())
		{
			av_freep(&fifo_buf[0]);
			av_free(fifo_buf);
			break;
		}

		// 将滤镜图输出的PCM数据提交到XAudio2
		XAUDIO2_BUFFER* buffer_pcm = xaudio2_get_available_buffer(audio_bytes);
		buffer_pcm->AudioBytes = audio_bytes; // 16-bit stereo, bytes per sample-frame = wfx.nBlockAlign
		memcpy(const_cast<BYTE*>(buffer_pcm->pAudioData), fifo_buf[0], buffer_pcm->AudioBytes);
		av_freep(&fifo_buf[0]);
		av_free(fifo_buf);

		hr = source_voice->SubmitSourceBuffer(buffer_pcm);
		if (FAILED(hr)) {
			ATLTRACE("err: submit source buffer failed, reason=0x%x\n", hr);
			playback_state.store(audio_playback_state_stopped);
			break;
		}

		if (playback_state.load() == audio_playback_state_init)
		{
			// if (state.BuffersQueued == 32)
			// {
			playback_state.store(audio_playback_state_playing);
			UNREFERENCED_PARAMETER(source_voice->Start());
			managed_music_player->ProcessEvent(WM_PLAYER_START, 0, 0);
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_START);
			Sleep(5); // wait for consuming buffer
			// }
		}

		source_voice->GetState(&state);
		// std::printf("info: submitted source buffer, buffers queued=%d\n", state.BuffersQueued);

		// 播放音频
		// source_voice->GetState(&state);
		// if (*playback_state == audio_playback_state_init)
		// {
			// if (state.BuffersQueued == 32)
			// {
			//	InterlockedExchange(playback_state, audio_playback_state_playing);
			//	source_voice->Start();
			// 	AfxGetMainWnd()->PostMessage(WM_PLAYER_START);
			// }
		// }
		// else
		// {
			// fix: avoid crash
		const size_t queued_buffers = state.BuffersQueued;
		if (xaudio2_playing_buffers.size() > queued_buffers)
		{
			const size_t played_count = xaudio2_playing_buffers.size() - queued_buffers;
			auto played_end = xaudio2_playing_buffers.begin();
			for (size_t i = 0; i < played_count; ++i)
			{
				xaudio2_played_samples += (*played_end)->AudioBytes / wfx.nBlockAlign;
				xaudio2_played_buffers++;
				++played_end;
			}
			xaudio2_free_buffers.insert(xaudio2_free_buffers.end(),
				xaudio2_playing_buffers.begin(), played_end);
			xaudio2_playing_buffers.erase(xaudio2_playing_buffers.begin(), played_end);
		}

		if (fft_executer)
		{
			int latency_bytes = std::transform_reduce(xaudio2_playing_buffers.begin(), xaudio2_playing_buffers.end(), 
				0, 
				std::plus{},
				[](auto buf) {
					return buf->AudioBytes;
				}
			);
			// Samples = Bytes / nBlockAlign
			int latency_samples = latency_bytes / wfx.nBlockAlign;
			// Time = Samples / SamplesPerSec
			int latency_ms = static_cast<int>(
				static_cast<double>(latency_samples) * 1000.0 / wfx.nSamplesPerSec
			);
			fft_executer->SetDelayFrames(latency_ms / 16);
		}
		decode_time_ms = static_cast<double>(xaudio2_played_samples - prev_decode_cycle_xaudio2_played_samples) * 1000.0 / wfx.nSamplesPerSec;
		prev_decode_cycle_xaudio2_played_samples = xaudio2_played_samples;
		elapsed_time = static_cast<float>(static_cast<double>(xaudio2_played_samples) * 1.0 / wfx.nSamplesPerSec + this->pts_seconds);

		// clock_t decode_end_time = clock();
		// double decode_time_ms = (decode_end_time - decode_begin_time) * 1000.0 / CLOCKS_PER_SEC;
		// remove duplicate log
		// ATLTRACE("info: xaudio2 cpu time %lf ms , frame time %lf ms!\n",
		//	 decode_time_ms, standard_frametime);
		// limit msg freq to 60mps, avoid ui stuck
		if (message_interval_timer > message_interval
			|| message_interval_timer < 0.0f)
		{
			message_interval_timer = 0.0f;
			managed_music_player->ProcessEvent(WM_PLAYER_TIME_CHANGE, *reinterpret_cast<UINT32*>(&elapsed_time), 0);
			// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, *reinterpret_cast<UINT32*>(&elapsed_time));
		}
		else { message_interval_timer += static_cast<float>(decode_time_ms); }
		// else
		// {
			// std::printf("info: buffer played=%zd\n", xaudio2_played_buffers);
		// }
		//  (wfx.wBitsPerSample / 8) * wfx.nChannels
	// }

	// LeaveCriticalSection(audio_playback_section);
	// EnterCriticalSection(audio_fifo_section);
		std::lock_guard fifo_event_lock(audio_fifo_mutex);
		if (get_audio_fifo_cached_samples_size() < xaudio2_play_frame_size * 32) {
			// need more data
			ATLTRACE("info: audio fifo cached samples size=%d, frame underrun!\n", get_audio_fifo_cached_samples_size());
			notify_frame_underrun();
		}
		else if (state.BuffersQueued < 32) {
			// enough data buffered
			notify_frame_ready();
		}
		// LeaveCriticalSection(audio_fifo_section);
	}
}

void MusicPlayerLibrary::MusicPlayerNative::audio_decode_worker_thread()
{
	bool is_eof = false;
	bool decoder_flushed = false;
	bool filter_flushed = false;
	while (true) {
		// frame underrun, notify decoder to decode more frames
		if (bool underrun_notified = wait_frame_underrun(std::chrono::milliseconds(1));
			!underrun_notified && get_audio_fifo_cached_samples_size() < xaudio2_play_frame_size * 256) {
			notify_frame_underrun();
		}
		else if (!underrun_notified && file_stream_end) {
			// pass
			notify_frame_ready();
		}
		else if (!underrun_notified) {
			continue;
		}
		clock_t decode_begin = clock();
		if (playback_state.load() == audio_playback_state_stopped) {
			ATLTRACE("info: playback stopped, decoder thread exiting\n");
			break;
		}

		// 文件流终止时，还有样本留在滤镜中
		// 删除file_stream_ended 改为判断滤镜是否完全排空
		if (filter_flushed) {
			ATLTRACE("info: decoder and filters completely flushed, decoder thread exiting\n");
			file_stream_end = true;
			break;
		}

		if (playback_state.load() == audio_playback_state_init
			&& is_pause) {
			ATLTRACE("info: resume from pause, pts_seconds=%lf\n", pts_seconds);
			if (av_seek_frame(format_context, -1, static_cast<int64_t>(pts_seconds * AV_TIME_BASE), AVSEEK_FLAG_ANY) < 0) {
				ATLTRACE("err: resume failed\n");
				playback_state.store(audio_playback_state_stopped);
			}
			avcodec_flush_buffers(codec_context);
			is_pause = false;
			is_eof = false;
			decoder_flushed = false;
			filter_flushed = false;
		}

		// 从输入文件中读取数据并解码
		if (!is_eof) {
			if (av_read_frame(format_context, packet) < 0) {
				ATLTRACE("info: av_read_frame reached eof, entering flush mode\n");
				// 文件流结束，进入flush模式
				is_eof = true;
			}
			else if (packet->stream_index != audio_stream_index) {
				notify_frame_underrun();
				av_packet_unref(packet);
				continue; // 跳过非音频流包
			}
		}

		if (packet->stream_index != audio_stream_index) {
			notify_frame_underrun();
			av_packet_unref(packet);
			continue; // skip non-audio packet
		}
		
		if (is_eof && !decoder_flushed) {
			// 发送空包以排空解码器缓存
			if (int ret = avcodec_send_packet(codec_context, nullptr); ret < 0 && ret != AVERROR_EOF) {
				ATLTRACE("warn: flush decoder failed, code=%d\n", ret);
			}
			decoder_flushed = true;
		}
		else if (!is_eof) {
			// 正常送入数据包
			if (int ret = avcodec_send_packet(codec_context, packet); ret < 0) {
				if (ret == AVERROR_INVALIDDATA) {
					// 忽略坏块
					av_packet_unref(packet);
					continue;
				}
				FFMPEG_CRITICAL_ERROR(ret);
				playback_state.store(audio_playback_state_stopped);
				av_packet_unref(packet);
				break;
			}
		}
		while (true)
		{
			if (int receive_res = avcodec_receive_frame(codec_context, frame); receive_res == AVERROR(EAGAIN)) {
				break; // 没有更多帧
			}
			else
			{
				if (receive_res == AVERROR_EOF) {
					// 解码器彻底排空，向滤镜发送空帧触发滤镜排空
					ATLTRACE("info: decoder flushed, sending empty frame to filter\n");
					if (is_eof && !filter_flushed) {
						av_buffersrc_add_frame(filter_context_src, nullptr);
					}
					break;
				}
				if (receive_res < 0) {
					FFMPEG_CRITICAL_ERROR(receive_res);
					playback_state.store(audio_playback_state_stopped);
					break;
				}
			}
			if (int add_frame_ret = av_buffersrc_add_frame(filter_context_src, frame); add_frame_ret < 0)
			{
				if (add_frame_ret != AVERROR_EOF) { // 滤镜图已被永久关闭
					FFMPEG_CRITICAL_ERROR(add_frame_ret);
				}
				else {
					ATLTRACE("info: filter shutdown, exiting\n");
				}
				// LeaveCriticalSection(audio_fifo_section);
				playback_state.store(audio_playback_state_stopped);
				break;
			}
			{
				std::lock_guard fifo_lock(audio_fifo_mutex);
				while (av_buffersink_get_frame(filter_context_sink, filt_frame) >= 0)
				{
					if (int ret_code = add_samples_to_fifo(filt_frame->extended_data, filt_frame->nb_samples); ret_code < 0) {
						FFMPEG_CRITICAL_ERROR(ret_code);
						playback_state.store(audio_playback_state_stopped);
						// LeaveCriticalSection(audio_fifo_section);
						break;
					}
					av_frame_unref(filt_frame);
				}
			}
			// ATLTRACE("info: decoded frame nb_samples=%d, pts=%lld\n", frame->nb_samples, frame->pts);
			// LeaveCriticalSection(audio_fifo_section);
			av_frame_unref(frame);
		}

		// 进入EOF模式后，排空滤镜中的所有样本
		if (is_eof && decoder_flushed && !filter_flushed) {
			std::lock_guard fifo_lock(audio_fifo_mutex);
			int flush_attempt = 0;
			while (true) {
				flush_attempt++;
				ATLTRACE("info: %d attempt of flushing filter\n", flush_attempt);
				if (int res = av_buffersink_get_frame(filter_context_sink, filt_frame); 
					res == AVERROR_EOF) {
					// 滤镜彻底排空
					ATLTRACE("info: filter flushed, all samples processed\n");
					filter_flushed = true;
					file_stream_end = true; // 触发播放线程去读完最后的 FIFO 数据
					break;
				}
				else if (res == AVERROR(EAGAIN) || res < 0) {
					break;
				}

				if (int ret_code = add_samples_to_fifo(filt_frame->extended_data, filt_frame->nb_samples); 
					ret_code < 0) {
					FFMPEG_CRITICAL_ERROR(ret_code);
					playback_state.store(audio_playback_state_stopped);
					break;
				}
				av_frame_unref(filt_frame);
			}
		}

		{
			std::lock_guard fifo_lock(audio_fifo_mutex);
			if (get_audio_fifo_cached_samples_size() > 0) {
				// enough data buffered
				notify_frame_ready();
			}
			if (get_audio_fifo_cached_samples_size() < xaudio2_play_frame_size * 256) {
				notify_frame_underrun();
			}
		}
		// LeaveCriticalSection(audio_fifo_section);

		int player_bufferes_queued = (
			is_xaudio2_initialized()
			? decoder_query_xaudio2_buffer_size()
			: 0
			);
		if (player_bufferes_queued < 4 && playback_state.load() == audio_playback_state_playing) {
			// buffer underrun, resume player thread to submit data immediately
			ATLTRACE("info: xaudio2 buffers queued=%d, notify player thread to submit data\n", player_bufferes_queued);
			notify_frame_ready();
		}
		av_frame_unref(frame); // eof, err process -> proper unref
		if (!is_eof) {
			// EOF模式时，发送的包是空包
			av_packet_unref(packet);
		}
		clock_t decode_end = clock();
		double decode_time_ms = (decode_end - decode_begin) * 1000.0 / CLOCKS_PER_SEC;
		// this seems to be disrupting.
//		if (decode_time_ms > 10)
//			ATLTRACE("warn: decode cycle time=%lf ms > 10, may cause frame underrun!\n", decode_time_ms);
	}
}



void MusicPlayerLibrary::MusicPlayerNative::init_decoder_thread() {
	if (audio_decoder_worker_thread.joinable())
	{
		stop_audio_decode();
	}
	file_stream_end = false;
	audio_decoder_worker_thread = std::jthread(
		[this] {
			SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
			AvSetMmThreadCharacteristics(_T("Pro Audio"), xaudio2_thread_task_index);
			decoder_is_running = true;
			audio_decode_worker_thread();
			decoder_is_running = false;
		});
	ATLTRACE("info: decoder thread created\n");
	notify_frame_underrun();
}

inline void MusicPlayerLibrary::MusicPlayerNative::start_audio_playback()
{
	if (playback_state.load() == audio_playback_state_stopped) {
		reset_audio_context();
	}
	if (source_voice) {
		XAUDIO2_VOICE_STATE state;
		source_voice->GetState(&state);
		base_offset = state.SamplesPlayed;
	}
	playback_state.store(audio_playback_state_init);
	message_interval_timer = -1.0f;
	if (audio_player_worker_thread.joinable())
	{
		stop_audio_playback(0);
	}
	user_request_stop.store(false);
	audio_player_worker_thread = std::jthread(
		[this] {
			SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
			AvSetMmThreadCharacteristics(_T("Pro Audio"), xaudio2_thread_task_index);
			audio_playback_worker_thread();
		});
	ATLTRACE("info: player thread created\n");
	// notify decoder to start decoding
}

void MusicPlayerLibrary::MusicPlayerNative::stop_audio_decode(int mode)
{
	if (audio_decoder_worker_thread.joinable())
	{
		playback_state.store(audio_playback_state_stopped);
		audio_decoder_worker_thread.request_stop();
		notify_all_frame_notifications();
		audio_decoder_worker_thread.join();
	}
}

void MusicPlayerLibrary::MusicPlayerNative::stop_audio_playback(int mode)
{
	if (audio_player_worker_thread.joinable())
	{
		// 播放线程在排空fifo时可能长期持有锁，此处持有锁会导致暂停时ui冻结。
		// 使用std::atomic_bool存储标志位，并在播放线程的几个关键节点手动检测。
		// *重构的时候才想起TryEnterCriticalSection的好。
		user_request_stop.store(true);
		playback_state.store(audio_playback_state_stopped);
		notify_all_frame_notifications();
		audio_player_worker_thread.request_stop();
		audio_player_worker_thread.join();

		if (source_voice)
		{
			UNREFERENCED_PARAMETER(source_voice->Stop(0));
			UNREFERENCED_PARAMETER(source_voice->FlushSourceBuffers());
		}
	}
	// Stop decoder after playback thread exits; pause/reset can then rebuild FIFO
	// and filters without racing the playback worker.
	stop_audio_decode(is_pause ? 1 : 0);
	// terminated xaudio and ffmpeg, do cleanup
	xaudio2_free_buffer();
	xaudio2_destroy_buffer();
	xaudio2_played_samples = xaudio2_played_buffers = xaudio2_played_samples = xaudio2_played_buffers = 0;
	float pts_time_f;
	if (is_pause)
	{
		pts_time_f = static_cast<float>(pts_seconds);
	}
	else {
		elapsed_time = pts_time_f = 0.0f;
	}
	UINT32 raw = *reinterpret_cast<UINT32*>(&pts_time_f);
	suppress_time_events = false;
	managed_music_player->ProcessEvent(WM_PLAYER_TIME_CHANGE, raw, 0);
	// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, raw);
	reset_frame_notifications();
	if (mode == 0)
		reset_audio_context();
	else if (mode == -1)
		release_audio_context();
}

int MusicPlayerLibrary::MusicPlayerNative::initialize_audio_fifo(AVSampleFormat sample_fmt, int channels, int nb_samples)
{
	audio_fifo = av_audio_fifo_alloc(sample_fmt, channels, nb_samples);
	if (!audio_fifo)
	{
		// AfxMessageBox(_T("err: could not allocate audio fifo!"), MB_ICONERROR);
		FFMPEG_CRITICAL_ERROR(-1);
		return -1;
	}
	return 0;
}

int MusicPlayerLibrary::MusicPlayerNative::resize_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int ret_value; (ret_value = av_audio_fifo_realloc(audio_fifo, nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret_value);
		return ret_value;
	}
	return 0;
}

int MusicPlayerLibrary::MusicPlayerNative::add_samples_to_fifo(uint8_t** decoded_data, int nb_samples)
{
	if (!audio_fifo)
		return -1;
	if (int res = av_audio_fifo_write(audio_fifo, reinterpret_cast<void**>(decoded_data), nb_samples); res < 0) {
		// audio fifo will resize automatically
		FFMPEG_CRITICAL_ERROR(res);
		return res;
	}
	// 	ATLTRACE("info: added %d samples to audio fifo\n", res);
	return 0;
}

int MusicPlayerLibrary::MusicPlayerNative::read_samples_from_fifo(uint8_t** output_buffer, int nb_samples)
{
	int ret;
	if (!audio_fifo)
		return -1;
	if ((ret = av_audio_fifo_read(audio_fifo, reinterpret_cast<void**>(output_buffer), nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
		return -1;
	}
	return ret;
}

void MusicPlayerLibrary::MusicPlayerNative::drain_audio_fifo(int nb_samples)
{
	if (!audio_fifo)
		return;
	if (int ret; (ret = av_audio_fifo_drain(audio_fifo, nb_samples)) < 0) {
		FFMPEG_CRITICAL_ERROR(ret);
	}
}

void MusicPlayerLibrary::MusicPlayerNative::reset_audio_fifo()
{
	if (!audio_fifo)
		return;
	av_audio_fifo_reset(audio_fifo);
}

int MusicPlayerLibrary::MusicPlayerNative::get_audio_fifo_cached_samples_size()
{
	if (!audio_fifo)
		return -1;
	return av_audio_fifo_size(audio_fifo);
}

void MusicPlayerLibrary::MusicPlayerNative::uninitialize_audio_fifo()
{
	if (audio_fifo)
	{
		av_audio_fifo_free(audio_fifo);
		audio_fifo = nullptr;
	}
}

inline const char* MusicPlayerLibrary::MusicPlayerNative::get_backend_implement_version() // NOLINT(*-convert-member-functions-to-static)
{
	static char xaudio2_implement_version[] = XAUDIO2_DLL_A;
	return xaudio2_implement_version;
}

void MusicPlayerLibrary::MusicPlayerNative::xaudio2_init_buffer(XAUDIO2_BUFFER* dest_buffer, int size) // NOLINT(*-convert-member-functions-to-static)
{
	if (size < 8192) size = 8192;
	if (int& buffer_size = *reinterpret_cast<int*>(dest_buffer->pContext); size > buffer_size)
	{
		ATLTRACE("info: xaudio2 reallocate_buffer, reallocate_size=%d, original_size=%d\n", size, buffer_size);
		delete[] dest_buffer->pAudioData;
		dest_buffer->pAudioData = DBG_NEW BYTE[size];
		buffer_size = size;
	}
	memset(const_cast<BYTE*>(dest_buffer->pAudioData), 0, size);
}

XAUDIO2_BUFFER* MusicPlayerLibrary::MusicPlayerNative::xaudio2_allocate_buffer(int size)
{
	if (size < 8192) size = 8192;
	// ATLTRACE("info: xaudio2_allocate_buffer, allocate_size=%d\n", size);
	XAUDIO2_BUFFER* dest_buffer = DBG_NEW XAUDIO2_BUFFER{}; // NOLINT(*-use-auto)
	dest_buffer->pAudioData = DBG_NEW BYTE[size];
	dest_buffer->pContext = DBG_NEW int(size);
	xaudio2_init_buffer(dest_buffer);
	return dest_buffer;
}

XAUDIO2_BUFFER* MusicPlayerLibrary::MusicPlayerNative::xaudio2_get_available_buffer(int size)
{
	// std::printf("info: DBG_NEW xaudio2_buffer request, allocated=%lld, played=%lld\n", xaudio2_allocated_buffers, xaudio2_played_buffers);
	if (!xaudio2_free_buffers.empty())
	{
		// std::printf("info: free buffer recycled\n");
		auto dest_buffer = xaudio2_free_buffers.front();
		xaudio2_free_buffers.pop_front();
		xaudio2_init_buffer(dest_buffer, size);
		xaudio2_playing_buffers.push_back(dest_buffer);
		return dest_buffer;
	}
	// Allocate a DBG_NEW XAudio2 buffer.
	xaudio2_playing_buffers.push_back(xaudio2_allocate_buffer(size));
	xaudio2_allocated_buffers++;
	// std::printf("info: DBG_NEW xaudio2 buffer allocated, current allocate: %lld\n", xaudio2_allocated_buffers);
	return xaudio2_playing_buffers.back();
}

void MusicPlayerLibrary::MusicPlayerNative::xaudio2_free_buffer()
{
	for (auto& i : xaudio2_playing_buffers)
	{
		assert(i);
		delete[] i->pAudioData;
		delete reinterpret_cast<int*>(i->pContext);
		delete i;
		i = nullptr;
	}
	xaudio2_allocated_buffers = 0; xaudio2_played_buffers = 0;
	xaudio2_playing_buffers.clear();
}

void MusicPlayerLibrary::MusicPlayerNative::xaudio2_destroy_buffer()
{
	for (auto& i : xaudio2_free_buffers)
	{
		assert(i);
		delete[] i->pAudioData;
		delete reinterpret_cast<int*>(i->pContext);
		delete i;
		i = nullptr;
	}
	xaudio2_free_buffers.clear();
}

int MusicPlayerLibrary::MusicPlayerNative::decoder_query_xaudio2_buffer_size()
{
	std::lock_guard lock(audio_playback_mutex);
	XAUDIO2_VOICE_STATE state;
	source_voice->GetState(&state);
	int buffer_size = static_cast<int>(state.BuffersQueued);
	return buffer_size;
}

bool MusicPlayerLibrary::MusicPlayerNative::is_xaudio2_initialized()
{
	return wfx.nSamplesPerSec > 0 && wfx.nBlockAlign > 0
		&& source_voice && mastering_voice && xaudio2;
}

size_t MusicPlayerLibrary::MusicPlayerNative::get_samples_played_per_session()
{
	XAUDIO2_VOICE_STATE state;
	source_voice->GetState(&state);
	return state.SamplesPlayed - base_offset;
}

void MusicPlayerLibrary::MusicPlayerNative::dialog_ffmpeg_critical_error(int err_code, const char* file, int line) // NOLINT(*-convert-member-functions-to-static)
{
	char buf[1024] = { 0 };
	av_strerror(err_code, buf, 1024);
	CString message = _T("FFmpeg critical error: ");
	CString res{};
	res.Format(_T("%s (file: %s, line: %d)\n"), CString(buf).GetString(), CString(file).GetString(), line);
	message += res;
	throw gcnew System::InvalidOperationException(msclr::interop::marshal_as<String^>(message.GetString()));
}

// this stack_unwind function is not useful, removed.

MusicPlayerLibrary::MusicPlayerNative::MusicPlayerNative() :
	playback_state(audio_playback_state_init),
	xaudio2_thread_task_index(new DWORD(0))
{
	ATLTRACE("info: decode frontend: avformat version %d, avcodec version %d, avutil version %d, swresample version %d\n",
		avformat_version(),
		avcodec_version(),
		avutil_version(),
		swresample_version());
	ATLTRACE("info: audio api backend: XAudio2 version %s\n", get_backend_implement_version());
	for (int i = 0; i < 10; ++i)
	{
		eq_bands.Add(0);
	}
}

/**
 * @brief Initializes the audio filter graph using libavfilter
 *
 * @note in case of pcm buffering using AVAudioFifo,
 * equalizer will only affect decoding after ~1s.
 */
int MusicPlayerLibrary::MusicPlayerNative::init_av_filter_equalizer()
{
	filter_graph = avfilter_graph_alloc();
	if (!filter_graph)
	{
		ATLTRACE("err: avfilter_graph_alloc failed\n");
		return -1;
	}

	if (sample_rate <= 0)
		sample_rate = 48000;

	CStringA layout_str;
	auto layout_str_buffer = layout_str.GetBufferSetLength(256);
	av_channel_layout_describe(&codec_context->ch_layout, layout_str_buffer, 256);
	layout_str.ReleaseBuffer();
	CStringA args;
	args.Format("sample_rate=%d:sample_fmt=%s:channel_layout=%s",
		codec_context->sample_rate,
		av_get_sample_fmt_name(codec_context->sample_fmt),
		layout_str.GetString());
	ATLTRACE("info: init_av_filter_equalizer, filter args: %s\n", args.GetString());
	int ret = avfilter_graph_create_filter(&filter_context_src, avfilter_get_by_name("abuffer"),
		"src", args.GetString(), nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}

	CStringA resample_args;
	resample_args.Format("sample_rate=%d:out_chlayout=stereo:out_sample_fmt=s16", sample_rate);
	ret = avfilter_graph_create_filter(&resample_ctx, avfilter_get_by_name("aresample"),
		"resample", resample_args.GetString(), nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	ATLTRACE("info: resample filter created, param = %s\n", resample_args.GetString());

	if (eq_bands.GetSize() != 10)
	{
		ATLTRACE("warn: invalid eq_bands size, =%d\n", eq_bands.GetSize());
		eq_bands.RemoveAll();
		for (int i = 0; i < 10; i++)
		{
			eq_bands.Add(0);
		}
	}
	else
	{
		ATLTRACE("info: eq_bands already initialized, skip\n");
	}
	for (int i = 0; i < 10; i++)
	{
		constexpr int freq_hz[] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
		av_filter_eq_graph eq_graph{
			.freq = freq_hz[i],
			.gain_values = eq_bands[i],
			.eq_context = nullptr
		};
		eq_graph.eq_name.Format("eq%d", i);
		CStringA arg_str;
		arg_str.Format("f=%d:t=q:w=1:g=%d", freq_hz[i], eq_bands[i]);
		ATLTRACE("info: init_av_filter_equalizer, filter args: %s\n", arg_str.GetString());

		ret = avfilter_graph_create_filter(&eq_graph.eq_context, avfilter_get_by_name("equalizer"),
			eq_graph.eq_name.GetString(),
			arg_str.GetString(), nullptr, filter_graph);
		if (ret < 0)
		{
			FFMPEG_CRITICAL_ERROR(ret);
			return ret;
		}

		filter_graphs.Add(eq_graph);
	}
	ret = avfilter_graph_create_filter(&filter_context_sink, avfilter_get_by_name("abuffersink"),
		"sink", nullptr, nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	ret = avfilter_graph_create_filter(&volume_ctx, avfilter_get_by_name("volume"),
		"pregain", "volume=0.7", nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	if ((ret = avfilter_link(filter_context_src, 0, resample_ctx, 0)) < 0 ||
		(ret = avfilter_link(resample_ctx, 0, volume_ctx, 0)) < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	if ((ret = avfilter_link(volume_ctx, 0, filter_graphs[0].eq_context, 0)) < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	for (int i = 0; i < 9; i++)
	{
		if ((ret = avfilter_link(filter_graphs[i].eq_context, 0, filter_graphs[i + 1].eq_context, 0)) < 0)
		{
			FFMPEG_CRITICAL_ERROR(ret);
			return ret;
		}
	}
	ret = avfilter_graph_create_filter(&limiter_ctx, avfilter_get_by_name("alimiter"),
		"lim", "limit=0.70:attack=5:release=50:level=disabled", nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	if ((ret = avfilter_link(filter_graphs[9].eq_context, 0, limiter_ctx, 0)) < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	ATLTRACE("info: limiter linked\n");
	CStringA fmt_args;
	fmt_args.Format("sample_fmts=s16:sample_rates=%d:channel_layouts=stereo", sample_rate);
	ret = avfilter_graph_create_filter(&format_normalize_ctx,
		avfilter_get_by_name("aformat"),
		"aformat", fmt_args.GetString(), nullptr, filter_graph);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}
	ATLTRACE("info: format filter created, param = %s\n", fmt_args.GetString());
	if ((ret = avfilter_link(limiter_ctx, 0, format_normalize_ctx, 0)) < 0 ||
		(ret = avfilter_link(format_normalize_ctx, 0, filter_context_sink, 0)) < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}

	ret = avfilter_graph_config(filter_graph, nullptr);
	if (ret < 0)
	{
		FFMPEG_CRITICAL_ERROR(ret);
		return ret;
	}

	const AVFilterLink* sink_link = filter_context_sink->inputs[0];
	const int sink_channels = sink_link->ch_layout.nb_channels;
	ATLTRACE("info: filter output format=%s, sample_rate=%d, channels=%d\n",
		av_get_sample_fmt_name(static_cast<AVSampleFormat>(sink_link->format)),
		sink_link->sample_rate,
		sink_channels);
	if (sink_link->sample_rate != sample_rate ||
		sink_channels != fifo_audio_channels ||
		sink_link->format != fifo_audio_sample_fmt)
	{
		ATLTRACE("err: filter output format mismatch, expected format=s16, sample_rate=%d, channels=%d\n",
			sample_rate, fifo_audio_channels);
		return -1;
	}
	return 0;
}

bool MusicPlayerLibrary::MusicPlayerNative::is_av_filter_equalizer_initialized()
{
	return filter_graphs.GetSize() > 0
		&& filter_context_src && filter_context_sink;
}

void MusicPlayerLibrary::MusicPlayerNative::reset_av_filter_equalizer()
{
	if (filter_graph)
		avfilter_graph_free(&filter_graph);
	filter_graph = nullptr;
	filter_context_src = filter_context_sink = nullptr;
	resample_ctx = nullptr;
	volume_ctx = limiter_ctx = format_normalize_ctx = nullptr;
	filter_graphs.RemoveAll();
}

bool MusicPlayerLibrary::MusicPlayerNative::IsInitialized()
{
	return is_audio_context_initialized() && is_xaudio2_initialized();
}

bool MusicPlayerLibrary::MusicPlayerNative::IsPlaying()
{
	return IsInitialized() &&
		playback_state.load() != audio_playback_state_init && playback_state.load() != audio_playback_state_stopped;
}

void MusicPlayerLibrary::MusicPlayerNative::OpenFile(const CString& fileName, const CString& file_extension_in)
{
	if (load_audio_context(fileName, file_extension_in)) {
		// AfxMessageBox(_T("err: load file failed, please check trace message!"), MB_ICONERROR);
		throw gcnew System::InvalidOperationException("Load file failed, please re-run in terminal and check trace message!");
		return;
	}
	if (initialize_audio_engine()) {
		// AfxMessageBox(_T("err: audio engine initialize failed!"), MB_ICONERROR);
		throw gcnew System::InvalidOperationException("Audio engine initialize failed!");
		return;
	};
	managed_music_player->ProcessEvent(WM_PLAYER_FILE_INIT, 0, 0);
	managed_music_player->ProcessEvent(WM_PLAYER_TIME_CHANGE, 0, 0);
	// AfxGetMainWnd()->PostMessage(WM_PLAYER_FILE_INIT);
}

float MusicPlayerLibrary::MusicPlayerNative::GetMusicTimeLength()
{
	if (IsInitialized()) {
		if (fabs(length - 0.0f) < 0.0001f) {
			AVStream* audio_stream = format_context->streams[audio_stream_index];
			int64_t duration = audio_stream->duration;
			AVRational time_base = audio_stream->time_base;
			length = static_cast<float>(static_cast<double>(duration) * av_q2d(time_base));
		}
		return length;
	}
	return 0.0f;
}

float MusicPlayerLibrary::MusicPlayerNative::GetCurrentMusicPosition()
{
	if (IsInitialized())
	{
		return elapsed_time;
	}
	return 0.0f;
}

CString MusicPlayerLibrary::MusicPlayerNative::GetSongTitle()
{
	if (IsInitialized()) {
		return song_title;
	}
	return {};
}

CString MusicPlayerLibrary::MusicPlayerNative::GetSongArtist()
{
	if (IsInitialized()) {
		return song_artist;
	}
	return {};
}

void MusicPlayerLibrary::MusicPlayerNative::Start()
{
	if (IsInitialized() && !IsPlaying()) {
		start_audio_playback();
	}
}

void MusicPlayerLibrary::MusicPlayerNative::Stop()
{
	if (IsInitialized() && IsPlaying()) {
		pts_seconds = 0;
		stop_audio_playback(0);
		managed_music_player->ProcessEvent(WM_PLAYER_STOP, 0, 0);
	}
}

void MusicPlayerLibrary::MusicPlayerNative::SetMasterVolume(float volume)
{
	if (IsInitialized()) {
		if (volume < 0.0f) volume = 0.0f;
		if (volume > 1.0f) volume = 1.0f;
		UNREFERENCED_PARAMETER(mastering_voice->SetVolume(volume));
	}
}

void MusicPlayerLibrary::MusicPlayerNative::SeekToPosition(float time, bool need_stop)
{
	if (IsInitialized()) {
		is_pause = true;
		pts_seconds = time;
		if (IsInitialized())
		{
			if (need_stop && (IsPlaying() || audio_player_worker_thread.joinable()))
			{
				suppress_time_events = true;
				user_request_stop.store(true);
				stop_audio_playback(0);
			}
			else if (!IsPlaying()) {
				if (decoder_is_running) {
					stop_audio_decode(1);
					playback_state.store(audio_playback_state_init);
					reset_audio_context();
					managed_music_player->ProcessEvent(WM_PLAYER_TIME_CHANGE, *reinterpret_cast<UINT*>(&time), 0);
					// AfxGetMainWnd()->PostMessage(WM_PLAYER_TIME_CHANGE, *reinterpret_cast<UINT*>(&time));
				}
			}
		}
	}
}

void MusicPlayerLibrary::MusicPlayerNative::SetSampleRate(int sample_rate)
{
	if (IsInitialized()) {
		// Set sample rate after init is not supported.
		throw gcnew System::InvalidOperationException("SetSampleRate is not supported after initialization!");
	}
	this->sample_rate = sample_rate;
}

int MusicPlayerLibrary::MusicPlayerNative::GetNBlockAlign()
{
	return wfx.nBlockAlign;
}

CString MusicPlayerLibrary::MusicPlayerNative::GetID3Lyric()
{
	return id3_string_lyric;
}

int MusicPlayerLibrary::MusicPlayerNative::GetEqualizerBand(int index)
{
	if (index < 0 || index >= 10) return 0;
	return filter_graphs[index].gain_values;
}

void MusicPlayerLibrary::MusicPlayerNative::SetEqualizerBand(int index, int value)
{
	if (index < 0 || index >= 10) return;
	if (value < -24) value = -24;
	else if (value > 24) value = 24;
	if (eq_bands.GetSize() == 10)
		eq_bands[index] = value;
	if (this->is_av_filter_equalizer_initialized())
	{
		filter_graphs[index].gain_values = value;
		CStringA eq_name, gain_val;
		eq_name.Format("eq%d", index);
		gain_val.Format("%d", value);

		avfilter_graph_send_command(filter_graph, eq_name.GetString(), "gain", gain_val.GetString(), nullptr, 0, 0);
	}
}

void MusicPlayerLibrary::MusicPlayerNative::SetManagedPlayer(MusicPlayer^ managed_player)
{
	this->managed_music_player = managed_player;
}

void MusicPlayerLibrary::MusicPlayerNative::Pause()
{
	if (IsInitialized() && IsPlaying()) {
		is_pause = true;
		pts_seconds = elapsed_time;
		stop_audio_playback(0);
		managed_music_player->ProcessEvent(WM_PLAYER_PAUSE, 0, 0);
	}
}



MusicPlayerLibrary::MusicPlayerNative::~MusicPlayerNative()
{
	if (album_art_worker_thread.joinable())
	{
		album_art_worker_thread.request_stop();
		album_art_worker_thread.join();
	}

	if (playback_state.load() == audio_playback_state_playing) {
		user_request_stop.store(true);
		stop_audio_playback(-1);
	}
	stop_audio_decode();
	uninitialize_audio_engine();

	delete xaudio2_thread_task_index;
	if (audio_fifo) 				uninitialize_audio_fifo();
	reset_av_filter_equalizer();
	release_audio_context();

	if (file_stream)
	{
		file_stream->Close();
		file_stream.reset();
	}
}

MusicPlayerLibrary::MusicPlayer::MusicPlayer()
{
	native_handle = new MusicPlayerNative();
	native_handle->SetManagedPlayer(this);
}

MusicPlayerLibrary::MusicPlayer::MusicPlayer(int sample_rate)
{
	native_handle = new MusicPlayerNative();
	native_handle->SetSampleRate(sample_rate);
	native_handle->SetManagedPlayer(this);
}

void MusicPlayerLibrary::MusicPlayer::check_if_null()
{
	if (!native_handle)
		throw gcnew System::InvalidOperationException("MusicPlayerNative initialization failed!");
}

void MusicPlayerLibrary::MusicPlayer::ProcessEvent(MessageType event_type, WPARAM wParam, LPARAM lParam)
{
	if (!native_handle)
		return; // 析构后或尚未初始化，安静忽略
	
	if (event_type == WM_PLAYER_TIME_CHANGE && native_handle && native_handle->suppress_time_events)
		return;

	ProcessEventState^ state = gcnew ProcessEventState();
	state->EventType = event_type;
	state->WParam = IntPtr(static_cast<long long>(wParam));
	state->LParam = IntPtr(static_cast<long long>(lParam));
	System::Threading::ThreadPool::QueueUserWorkItem(
		gcnew System::Threading::WaitCallback(this, &MusicPlayer::ProcessEventCore), state);
}

void MusicPlayerLibrary::MusicPlayer::ProcessEventCore(Object^ stateObj)
{	
	if (!native_handle)
		return; // native 已被销毁，跳过
	ProcessEventState^ state = safe_cast<ProcessEventState^>(stateObj);
	WPARAM wParam = static_cast<WPARAM>(state->WParam.ToInt64());

	switch (state->EventType) {
	case WM_PLAYER_FILE_INIT:
		if (OnPlayerFileInit)
			OnPlayerFileInit();
		break;
	case WM_PLAYER_ALBUM_ART_INIT:
		if (OnPlayerAlbumArtInit) {
			if (wParam == 0) {
				OnPlayerAlbumArtInit(nullptr);
				break;
			}
			IntPtr hBitmap = static_cast<IntPtr>(static_cast<long long>(wParam));
			System::Drawing::Image^ bitmap = System::Drawing::Image::FromHbitmap(hBitmap);
			DeleteObject(reinterpret_cast<HBITMAP>(wParam));
			OnPlayerAlbumArtInit(bitmap);
		}
		else if (wParam != 0) {
			DeleteObject(reinterpret_cast<HBITMAP>(wParam));
		}
		break;
	case WM_PLAYER_START:
		if (OnPlayerStart)
			OnPlayerStart();
		break;
	case WM_PLAYER_PAUSE:
		if (OnPlayerPause)
			OnPlayerPause();
		break;
	case WM_PLAYER_STOP:
		if (OnPlayerStop)
			OnPlayerStop();
		break;
	case WM_PLAYER_DESTROY:
		if (OnPlayerDestroy)
			OnPlayerDestroy();
		break;
	case WM_PLAYER_TIME_CHANGE:
		if (OnPlayerTimeChange) {
			float time = *reinterpret_cast<float*>(&wParam);
			OnPlayerTimeChange(time);
		}
		break;
	default:
		break;
	}
}

bool MusicPlayerLibrary::MusicPlayer::IsInitialized()
{
	if (!is_native_valid()) return false;
	return native_handle->IsInitialized();
}

bool MusicPlayerLibrary::MusicPlayer::IsPlaying()
{
	if (!is_native_valid()) return false;
	return native_handle->IsPlaying();
}

static bool IsValidPath(const CString& path)
{
	if (path.IsEmpty())
		return false;

	static const CString invalidChars = _T("<>\"|?*");
	for (int i = 0; i < invalidChars.GetLength(); ++i)
	{
		if (path.Find(invalidChars[i]) != -1)
		{
			ATLTRACE("err: invalid character found, char: %c\n", invalidChars[i]);
			return false;
		}
	}

	return PathFileExists(path);
}

void MusicPlayerLibrary::MusicPlayer::OpenFile(const System::String^ fileName)
{
	check_if_null();
	pin_ptr<const wchar_t> wch = PtrToStringChars(fileName);
	CString mfcFileName(wch);
	if (!IsValidPath(mfcFileName)) {
		throw gcnew System::ArgumentException("file does not exist!");
	}
	CString extension = PathFindExtension(mfcFileName);
	extension = extension.Mid(1);
	native_handle->OpenFile(mfcFileName, extension);
}

float MusicPlayerLibrary::MusicPlayer::GetMusicTimeLength()
{
	if (!is_native_valid()) return 0.0f;
	return native_handle->GetMusicTimeLength();
}

float MusicPlayerLibrary::MusicPlayer::GetCurrentMusicPosition()
{
	if (!is_native_valid()) return 0.0f;
	return native_handle->GetCurrentMusicPosition();
}

System::String^ MusicPlayerLibrary::MusicPlayer::GetSongTitle()
{
	if (!is_native_valid()) return nullptr;
	CString title = native_handle->GetSongTitle();
	// TODO: 在此处插入 return 语句
	if (title.IsEmpty()) return nullptr;
	return msclr::interop::marshal_as<System::String^>(title.GetString());
}

System::String^ MusicPlayerLibrary::MusicPlayer::GetSongArtist()
{
	if (!is_native_valid()) return nullptr;
	CString artist = native_handle->GetSongArtist();
	// TODO: 在此处插入 return 语句
	if (artist.IsEmpty()) return nullptr;
	return msclr::interop::marshal_as<System::String^>(artist.GetString());
}

void MusicPlayerLibrary::MusicPlayer::Start()
{
	check_if_null();
	native_handle->Start();
}

void MusicPlayerLibrary::MusicPlayer::Pause()
{
	check_if_null();
	native_handle->Pause();
}

void MusicPlayerLibrary::MusicPlayer::Stop()
{
	check_if_null();
	native_handle->Stop();
}

void MusicPlayerLibrary::MusicPlayer::SetMasterVolume(float volume)
{
	check_if_null();
	native_handle->SetMasterVolume(volume);
}

void MusicPlayerLibrary::MusicPlayer::SeekToPosition(float time, bool need_stop)
{
	check_if_null();
	native_handle->SeekToPosition(time, need_stop);
}

int MusicPlayerLibrary::MusicPlayer::GetNBlockAlign()
{
	if (!is_native_valid()) return -1;
	return native_handle->GetNBlockAlign();
}

System::String^ MusicPlayerLibrary::MusicPlayer::GetID3Lyric()
{
	if (!is_native_valid()) return nullptr;
	CString lyric = native_handle->GetID3Lyric();
	// TODO: 在此处插入 return 语句
	return msclr::interop::marshal_as<System::String^>(lyric.GetString());
}

int MusicPlayerLibrary::MusicPlayer::GetEqualizerBand(int index)
{
	if (!is_native_valid()) return 0;
	return native_handle->GetEqualizerBand(index);
}

void MusicPlayerLibrary::MusicPlayer::SetEqualizerBand(int index, int value)
{
	check_if_null();
	native_handle->SetEqualizerBand(index, value);
}

array<float>^ MusicPlayerLibrary::MusicPlayer::GetAudioFFTData()
{
	if (!is_native_valid())
		return gcnew array<float>(0);
	if (!native_handle->fft_executer)
		return gcnew array<float>(0);
	auto data = native_handle->fft_executer->GetAudioFFTData();
	array<float>^ result = gcnew array<float>(static_cast<int>(data.size()));
	for (int i = 0; i < static_cast<int>(data.size()); ++i)
		result[i] = data[i];
	return result;
}

// {ddb0472d-c911-4a1f-86d9-dc3d71a95f5a} ISystemMediaTransportControlsInterop
static const IID IID_ISystemMediaTransportControlsInterop = 
	{ 0xddb0472d, 0xc911, 0x4a1f, { 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a } };

IntPtr MusicPlayerLibrary::SmtcInteropHelper::GetSmtcForWindow(IntPtr hWnd)
{
	HWND hwnd = static_cast<HWND>(hWnd.ToPointer());

	HSTRING_HEADER hstrHeader;
	HSTRING hstrClassName = nullptr;
	static const wchar_t className[] = L"Windows.Media.SystemMediaTransportControls";
	HRESULT hr = WindowsCreateStringReference(
		className,
		static_cast<UINT32>(wcslen(className)),
		&hstrHeader,
		&hstrClassName);
	if (FAILED(hr))
	{
		ATLTRACE("error: SmtcInteropHelper: WindowsCreateStringReference failed, hr=0x%08X\n", hr);
		return IntPtr::Zero;
	}

	ISystemMediaTransportControlsInterop* interop = nullptr;
	hr = RoGetActivationFactory(
		hstrClassName,
		IID_ISystemMediaTransportControlsInterop,
		reinterpret_cast<void**>(&interop));
	if (FAILED(hr) || interop == nullptr)
	{
		ATLTRACE("error: SmtcInteropHelper: RoGetActivationFactory failed, hr=0x%08X\n", hr);
		return IntPtr::Zero;
	}

	IInspectable* smtc = nullptr;
	hr = interop->GetForWindow(
		hwnd,
		IID_IInspectable,
		reinterpret_cast<void**>(&smtc));
	interop->Release();

	if (FAILED(hr) || smtc == nullptr)
	{
		ATLTRACE("error: SmtcInteropHelper: GetForWindow failed, hr=0x%08X\n", hr);
		return IntPtr::Zero;
	}

	return IntPtr(smtc);
}

void MusicPlayerLibrary::AtlTraceRedirectManager::Init(System::Object^ logger)
{
	if (m_pRedirector == nullptr)
	{
		m_pRedirector = new AtlTraceRedirect(logger);
		AtlTraceRedirect::SetAtlTraceRedirector(m_pRedirector);
	}
}
