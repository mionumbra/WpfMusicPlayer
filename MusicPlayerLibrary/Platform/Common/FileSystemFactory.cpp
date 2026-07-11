// SPDX-License-Identifier: MIT

#include "pch.h"

#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS_)
#include "Platform/Windows/WindowsFileSystem.h"
#endif

namespace MusicPlayerLibrary
{

    static IFileSystem* custom_file_system = nullptr;
    static FileSystemImplementation default_file_system_implementation =
        
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS_)
        FileSystemImplementation::WindowsApi
#else
        FileSystemImplementation::FastIO
#endif
    ;
    
    IFileSystem& GetBuiltInFileSystem(FileSystemImplementation implementation)
    {
        
#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS_)
        static WindowsApiFileSystem windows_fs;
#endif
        
        // static FastIOFileSystem fast_io_fs;

        switch (implementation)
        {
        case FileSystemImplementation::WindowsApi:
            return windows_fs;
        default:
            // return fast_io_fs;
            return windows_fs;
        }
    }

    FileSystemImplementation GetDefaultFileSystemImplementation()
    {
        return default_file_system_implementation;
    }

    void SetDefaultFileSystemImplementation(FileSystemImplementation implementation)
    {
        default_file_system_implementation = implementation;
        custom_file_system = nullptr;
    }

    IFileSystem& GetDefaultFileSystem()
    {
        if (custom_file_system)
            return *custom_file_system;
        return GetBuiltInFileSystem(default_file_system_implementation);
    }

    void SetDefaultFileSystem(IFileSystem* file_system)
    {
        custom_file_system = file_system;
    }
}