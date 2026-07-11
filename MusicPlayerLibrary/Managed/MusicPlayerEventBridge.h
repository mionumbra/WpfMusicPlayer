// SPDX-License-Identifier: MIT

#pragma once

#include <vcclr.h>

#include "Audio/MusicPlayerMessage.h"

namespace MusicPlayerLibrary
{
    ref class MusicPlayerManaged;
    
    class MusicPlayerEventBridge final : public IMusicPlayerMessageSink
    {
        gcroot<System::WeakReference^> managed_player_;

    public:
        explicit MusicPlayerEventBridge(MusicPlayerManaged^ managed_player);
        void Publish(const PlayerMessage& message) override;
    };
}
