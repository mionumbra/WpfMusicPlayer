// SPDX-License-Identifier: MIT

#pragma once

namespace MusicPlayerLibrary
{
    public ref class SmtcInteropHelper abstract sealed
    {
        public:
        static System::IntPtr GetSmtcForWindow(System::IntPtr hWnd);
    };
}
