// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
    class INativeLogWriter
    {
    public:
        virtual ~INativeLogWriter() = default;
    
        virtual void write_log(const std::string& message) = 0;
    };
    
    class INativeLogWriterFactory
    {
    public:
        virtual ~INativeLogWriterFactory() = default;
        virtual std::unique_ptr<INativeLogWriter> create_native_log_writer() = 0;
    };
    
    INativeLogWriterFactory* GetNativeLogWriterFactory();
    void SetNativeLogWriterFactory(INativeLogWriterFactory* factory);
}