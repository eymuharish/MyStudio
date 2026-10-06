#include "telestudio/media/RemoteVideoSurface.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
}

int main() {
    try {
        const auto decoded = telestudio::DecodeSessionTokenBase64Url("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
        Require(decoded.has_value(), "Canonical unpadded base64url must decode to 32 token bytes");
        for (size_t i = 0; i < decoded->size(); ++i) Require(std::to_integer<uint8_t>((*decoded)[i]) == i, "Token bytes must decode exactly");
        Require(!telestudio::DecodeSessionTokenBase64Url("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=").has_value(), "Padded token is not canonical");
        Require(!telestudio::DecodeSessionTokenBase64Url("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9").has_value(), "Non-zero trailing base64url pad bits must be rejected");
        const telestudio::HostRegistration registration {GetCurrentProcessId(), *decoded};
        telestudio::RemoteVideoSurface surface(registration);
        bool rejected = false;
        try { (void)surface.PublishForHost(); } catch (const std::logic_error&) { rejected = true; }
        Require(rejected, "Publish before exact host registration must fail");
        auto wrong = registration; wrong.sessionToken[0] = std::byte{0xff};
        Require(!surface.RegisterHost(wrong), "Wrong authenticated session token must be rejected");
        wrong = registration; wrong.desktopPid = 0;
        Require(!surface.RegisterHost(wrong), "Zero native PID must be rejected");
        Require(surface.RegisterHost(registration), "Live exact-process registration must succeed");

        telestudio::DecodedBgraFrame first {2, 2, 8, 1, std::vector<uint8_t>(16, 0x11)};
        Require(surface.UpdateOnRenderThread(first), "First BGRA upload must acquire key 0 and publish key 1");
        const auto published = surface.PublishForHost();
        Require(published.sequence == 1 && published.hostHandleValue != 0 && published.epoch == 1 && published.width == 2 && published.height == 2 &&
                published.format == telestudio::SurfaceFormat::Bgra8Unorm,
                "Published surface must contain target-process handle and epoch geometry");

        Microsoft::WRL::ComPtr<ID3D11Device> hostDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> hostContext;
        D3D_FEATURE_LEVEL selected{};
        constexpr D3D_FEATURE_LEVEL levels[] {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                           &hostDevice, &selected, &hostContext)), "Host D3D11 device must be available");
        Microsoft::WRL::ComPtr<ID3D11Device1> hostDevice1;
        Require(SUCCEEDED(hostDevice.As(&hostDevice1)), "Host must expose ID3D11Device1");
        Microsoft::WRL::ComPtr<ID3D11Texture2D> hostTexture;
        Require(SUCCEEDED(hostDevice1->OpenSharedResource1(reinterpret_cast<HANDLE>(published.hostHandleValue),
                                                           IID_PPV_ARGS(&hostTexture))),
                "Duplicated target-process handle must open in the exact registered process");
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> hostMutex;
        Require(SUCCEEDED(hostTexture.As(&hostMutex)), "Host resource must expose the keyed mutex");
        Require(SUCCEEDED(hostMutex->AcquireSync(1, 100)), "Host must acquire producer-completed key 1");
        Require(SUCCEEDED(hostMutex->ReleaseSync(0)), "Host must return producer key 0");
        surface.Opened(published.epoch);
        surface.Opened(published.epoch); // exactly-once transition; duplicate is a no-op
        surface.Released(published.epoch); // current epoch is deliberately retained
        Require(surface.CurrentEpoch() == published.epoch, "Current epoch cannot be released by acknowledgement");

        telestudio::DecodedBgraFrame resized {4, 2, 16, 2, std::vector<uint8_t>(32, 0x22)};
        Require(surface.UpdateOnRenderThread(resized), "Format change must fill a new slot before replacing the last good surface");
        const auto replacement = surface.PublishForHost();
        Require(replacement.sequence == 2 && replacement.epoch == 2 && replacement.width == 4 && replacement.height == 2,
                "Format change must publish a monotonic epoch");
        surface.Opened(999999); surface.Released(999999); surface.Removed(999999);
        Require(surface.CurrentEpoch() == replacement.epoch, "Stale epoch acknowledgements must be ignored");
        surface.Released(published.epoch);
        Require(surface.CurrentEpoch() == replacement.epoch, "In-order retired release must not disturb the current epoch");
        surface.Removed(replacement.epoch);
        Require(surface.CurrentEpoch() == replacement.epoch, "Removed(current) is a wrong-state no-op");
        surface.BeginRemoval();
        Require(surface.CurrentEpoch() == 0, "Terminal removal moves current out of the live slot");
        surface.Removed(replacement.epoch);
        hostMutex.Reset(); hostTexture.Reset();
        CloseHandle(reinterpret_cast<HANDLE>(published.hostHandleValue));
        CloseHandle(reinterpret_cast<HANDLE>(replacement.hostHandleValue));
        std::cout << "media_remote_surface_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_remote_surface_tests: " << error.what() << '\n';
        return 1;
    }
}
