// SPDX-License-Identifier: MIT

#pragma once
#include "Lyric/LrcFileController.h"

namespace MusicPlayerLibrary
{
    public ref class LrcFileControllerManaged:
        System::IDisposable
    {
        LrcFileController* native_handle;

        void check_if_null();

    public:
        LrcFileControllerManaged();
        LrcFileControllerManaged(int songEndTimeMs);

        System::String^ ParseLrcToIntermediateJson(System::String^ lrcString);

        ~LrcFileControllerManaged();
        !LrcFileControllerManaged();
    };
}
