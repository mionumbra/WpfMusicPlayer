// SPDX-License-Identifier: MIT

#include "pch.h"
#include "Platform/Windows/WindowsCommon.h"
#include "Managed/SmtcInteropHelper.h"

namespace MusicPlayerLibrary
{
    // {ddb0472d-c911-4a1f-86d9-dc3d71a95f5a} ISystemMediaTransportControlsInterop
    static const IID IID_ISystemMediaTransportControlsInterop = 
    { 0xddb0472d, 0xc911, 0x4a1f, { 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a } };

    System::IntPtr MusicPlayerLibrary::SmtcInteropHelper::GetSmtcForWindow(System::IntPtr hWnd)
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
            NATIVE_TRACE("error: SmtcInteropHelper: WindowsCreateStringReference failed, hr=0x%08X\n", hr);
            return System::IntPtr::Zero;
        }

        ISystemMediaTransportControlsInterop* interop = nullptr;
        hr = RoGetActivationFactory(
            hstrClassName,
            IID_ISystemMediaTransportControlsInterop,
            reinterpret_cast<void**>(&interop));
        if (FAILED(hr) || interop == nullptr)
        {
            NATIVE_TRACE("error: SmtcInteropHelper: RoGetActivationFactory failed, hr=0x%08X\n", hr);
            return System::IntPtr::Zero;
        }

        IInspectable* smtc = nullptr;
        hr = interop->GetForWindow(
            hwnd,
            IID_IInspectable,
            reinterpret_cast<void**>(&smtc));
        interop->Release();

        if (FAILED(hr) || smtc == nullptr)
        {
            NATIVE_TRACE("error: SmtcInteropHelper: GetForWindow failed, hr=0x%08X\n", hr);
            return System::IntPtr::Zero;
        }

        return System::IntPtr(smtc);
    }

}
