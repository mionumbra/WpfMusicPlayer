// SPDX-License-Identifier: MIT

#pragma once

#include "Core/NativeLogWriter.h"

namespace MusicPlayerLibrary
{
    
    class ManagedLoggerFactory: public INativeLogWriterFactory
    {
    public:
        std::unique_ptr<INativeLogWriter> create_native_log_writer() override;
    };

}
