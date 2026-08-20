#include "PCH.h"
#include "render/WicImageLoad.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <sstream>
#include <string>

namespace SH::hw {
    namespace {
        using Microsoft::WRL::ComPtr;

        std::string HrText(const char* operation, HRESULT hr) {
            std::ostringstream out;
            out << operation << " failed (0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(hr) << ')';
            return out.str();
        }

        std::wstring WidenPath(std::string_view path) {
            const std::string s(path);
            int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        s.c_str(), -1, nullptr, 0);
            UINT cp = CP_UTF8;
            DWORD flags = MB_ERR_INVALID_CHARS;
            if (n <= 0) {
                cp = CP_ACP;
                flags = 0;
                n = MultiByteToWideChar(cp, flags, s.c_str(), -1, nullptr, 0);
            }
            if (n <= 0) return {};
            std::wstring w(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(cp, flags, s.c_str(), -1, w.data(), n);
            w.resize(static_cast<std::size_t>(n - 1));
            return w;
        }

        struct ComInit {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool owns = SUCCEEDED(hr);
            ~ComInit() {
                if (owns) CoUninitialize();
            }
            bool Ok() const {
                return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            }
        };
    }

    bool LoadImageRGBA8(std::string_view path,
                        std::vector<std::uint8_t>& outPixels,
                        std::uint32_t& outWidth, std::uint32_t& outHeight) {
        outPixels.clear();
        outWidth = 0;
        outHeight = 0;

        const std::wstring wide = WidenPath(path);
        if (wide.empty()) {
            spdlog::warn("[render] image path is invalid: {}",
                         std::string(path));
            return false;
        }

        ComInit com;
        if (!com.Ok()) {
            spdlog::warn("[render] image WIC init: {}",
                         HrText("CoInitializeEx", com.hr));
            return false;
        }

        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            spdlog::warn("[render] image WIC factory: {}",
                         HrText("CoCreateInstance", hr));
            return false;
        }

        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(
            wide.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) {
            spdlog::warn("[render] image could not be loaded: {} ({})",
                         std::string(path), HrText("CreateDecoder", hr));
            return false;
        }

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) {
            spdlog::warn("[render] image frame: {}", HrText("GetFrame", hr));
            return false;
        }

        UINT width = 0;
        UINT height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0) {
            spdlog::warn("[render] image size: {}",
                         FAILED(hr) ? HrText("GetSize", hr) : "empty image");
            return false;
        }

        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) {
            spdlog::warn("[render] image converter: {}",
                         HrText("CreateFormatConverter", hr));
            return false;
        }
        hr = converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            spdlog::warn("[render] image conversion: {}",
                         HrText("Initialize", hr));
            return false;
        }

        const UINT stride = width * 4;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) * height);
        hr = converter->CopyPixels(nullptr, stride,
                                   static_cast<UINT>(pixels.size()),
                                   pixels.data());
        if (FAILED(hr)) {
            spdlog::warn("[render] image pixels: {}",
                         HrText("CopyPixels", hr));
            return false;
        }

        outPixels = std::move(pixels);
        outWidth = width;
        outHeight = height;
        return true;
    }
}
