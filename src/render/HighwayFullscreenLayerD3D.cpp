#include "PCH.h"
#include "render/HighwayFullscreenLayerD3D.h"

#include "render/WicImageLoad.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace SH::hw {
    namespace {
        using Microsoft::WRL::ComPtr;

        std::string HrText(const char* operation, HRESULT hr) {
            std::ostringstream out;
            out << operation << " failed (0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(hr) << ')';
            return out.str();
        }

        struct Vertex {
            float x, y;
            float u, v;
        };

        struct Constants {
            float viewport[4];  // width, height, unused, unused
            float tint[4];
        };

        static_assert(sizeof(Constants) % 16 == 0);

        constexpr char kShader[] = R"(
cbuffer FullscreenLayerConstants : register(b0) {
    float4 gViewport;
    float4 gTint;
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn input) {
    VSOut output;
    float2 ndc;
    ndc.x = input.pos.x / gViewport.x * 2.0f - 1.0f;
    ndc.y = 1.0f - input.pos.y / gViewport.y * 2.0f;
    output.pos = float4(ndc, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);

float4 PSMain(VSOut input) : SV_Target {
    return gTexture.Sample(gSampler, input.uv) * gTint;
}
)";

        bool CompileShader(const char* entry, const char* target,
                           ComPtr<ID3DBlob>& blob) {
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(
                kShader, std::strlen(kShader), "BardHeroFullscreenLayer",
                nullptr, nullptr, entry, target,
                D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &blob, &errors);
            if (FAILED(hr)) {
                const char* text = errors
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "";
                spdlog::warn(
                    "[render] fullscreen layer shader compile: {} {}",
                    HrText(entry, hr), text);
                return false;
            }
            return true;
        }

        // Device state save/restore around one manual draw call. Mirrors
        // HighwaySurfaceD3D's backup exactly (same reasoning: this runs
        // inside FLICK's own frame, so it must hand the pipeline back
        // exactly as it found it).
        class D3DStateBackup {
        public:
            explicit D3DStateBackup(ID3D11DeviceContext* context) :
                _context(context) {
                _context->IAGetInputLayout(&_inputLayout);
                _context->IAGetVertexBuffers(0, 1, &_vertexBuffer, &_stride,
                                             &_vertexOffset);
                _context->IAGetIndexBuffer(&_indexBuffer, &_indexFormat,
                                           &_indexOffset);
                _context->IAGetPrimitiveTopology(&_topology);

                _vsClassCount = static_cast<UINT>(_vsClasses.size());
                _psClassCount = static_cast<UINT>(_psClasses.size());
                _context->VSGetShader(&_vs, _vsClasses.data(),
                                      &_vsClassCount);
                _context->PSGetShader(&_ps, _psClasses.data(),
                                      &_psClassCount);
                _gsClassCount = static_cast<UINT>(_gsClasses.size());
                _hsClassCount = static_cast<UINT>(_hsClasses.size());
                _dsClassCount = static_cast<UINT>(_dsClasses.size());
                _context->GSGetShader(&_gs, _gsClasses.data(),
                                      &_gsClassCount);
                _context->HSGetShader(&_hs, _hsClasses.data(),
                                      &_hsClassCount);
                _context->DSGetShader(&_ds, _dsClasses.data(),
                                      &_dsClassCount);
                _context->VSGetConstantBuffers(0, 1, &_vsConstantBuffer);
                _context->PSGetConstantBuffers(0, 1, &_psConstantBuffer);
                _context->PSGetShaderResources(0, 1, &_psResource);
                _context->PSGetSamplers(0, 1, &_psSampler);

                _viewportCount =
                    D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                _context->RSGetViewports(&_viewportCount, _viewports.data());
                _context->RSGetState(&_rasterizerState);

                _context->OMGetBlendState(&_blendState, _blendFactor.data(),
                                          &_sampleMask);
                _context->OMGetDepthStencilState(&_depthStencilState,
                                                 &_stencilRef);
                std::array<ID3D11RenderTargetView*,
                           D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
                    rtvs{};
                _context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()),
                                             rtvs.data(),
                                             &_depthStencilView);
                for (std::size_t i = 0; i < rtvs.size(); ++i) {
                    _renderTargets[i].Attach(rtvs[i]);
                }
            }

            ~D3DStateBackup() {
                _context->IASetInputLayout(_inputLayout.Get());
                ID3D11Buffer* vb = _vertexBuffer.Get();
                _context->IASetVertexBuffers(0, 1, &vb, &_stride,
                                             &_vertexOffset);
                _context->IASetIndexBuffer(_indexBuffer.Get(), _indexFormat,
                                           _indexOffset);
                _context->IASetPrimitiveTopology(_topology);

                _context->VSSetShader(_vs.Get(), _vsClasses.data(),
                                      _vsClassCount);
                _context->PSSetShader(_ps.Get(), _psClasses.data(),
                                      _psClassCount);
                _context->GSSetShader(_gs.Get(), _gsClasses.data(),
                                      _gsClassCount);
                _context->HSSetShader(_hs.Get(), _hsClasses.data(),
                                      _hsClassCount);
                _context->DSSetShader(_ds.Get(), _dsClasses.data(),
                                      _dsClassCount);
                ID3D11Buffer* vsCb = _vsConstantBuffer.Get();
                ID3D11Buffer* psCb = _psConstantBuffer.Get();
                _context->VSSetConstantBuffers(0, 1, &vsCb);
                _context->PSSetConstantBuffers(0, 1, &psCb);
                ID3D11ShaderResourceView* srv = _psResource.Get();
                ID3D11SamplerState* sampler = _psSampler.Get();
                _context->PSSetShaderResources(0, 1, &srv);
                _context->PSSetSamplers(0, 1, &sampler);

                _context->RSSetViewports(
                    _viewportCount,
                    _viewportCount > 0 ? _viewports.data() : nullptr);
                _context->RSSetState(_rasterizerState.Get());

                std::array<ID3D11RenderTargetView*,
                           D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
                    rtvs{};
                for (std::size_t i = 0; i < rtvs.size(); ++i) {
                    rtvs[i] = _renderTargets[i].Get();
                }
                _context->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),
                                             rtvs.data(),
                                             _depthStencilView.Get());
                _context->OMSetBlendState(_blendState.Get(),
                                          _blendFactor.data(), _sampleMask);
                _context->OMSetDepthStencilState(_depthStencilState.Get(),
                                                 _stencilRef);

                for (UINT i = 0; i < _vsClassCount; ++i) {
                    if (_vsClasses[i]) _vsClasses[i]->Release();
                }
                for (UINT i = 0; i < _psClassCount; ++i) {
                    if (_psClasses[i]) _psClasses[i]->Release();
                }
                for (UINT i = 0; i < _gsClassCount; ++i) {
                    if (_gsClasses[i]) _gsClasses[i]->Release();
                }
                for (UINT i = 0; i < _hsClassCount; ++i) {
                    if (_hsClasses[i]) _hsClasses[i]->Release();
                }
                for (UINT i = 0; i < _dsClassCount; ++i) {
                    if (_dsClasses[i]) _dsClasses[i]->Release();
                }
            }

            ID3D11RenderTargetView* RenderTarget() const {
                return _renderTargets[0].Get();
            }

            D3D11_VIEWPORT FirstViewport(const View& fallback) const {
                if (_viewportCount > 0 &&
                    _viewports[0].Width > 0.0f &&
                    _viewports[0].Height > 0.0f) {
                    return _viewports[0];
                }
                return D3D11_VIEWPORT{ 0.0f, 0.0f, fallback.w, fallback.h,
                                       0.0f, 1.0f };
            }

        private:
            ComPtr<ID3D11DeviceContext> _context;
            ComPtr<ID3D11InputLayout> _inputLayout;
            ComPtr<ID3D11Buffer> _vertexBuffer;
            UINT _stride = 0;
            UINT _vertexOffset = 0;
            ComPtr<ID3D11Buffer> _indexBuffer;
            DXGI_FORMAT _indexFormat = DXGI_FORMAT_UNKNOWN;
            UINT _indexOffset = 0;
            D3D11_PRIMITIVE_TOPOLOGY _topology =
                D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

            ComPtr<ID3D11VertexShader> _vs;
            ComPtr<ID3D11PixelShader> _ps;
            ComPtr<ID3D11GeometryShader> _gs;
            ComPtr<ID3D11HullShader> _hs;
            ComPtr<ID3D11DomainShader> _ds;
            std::array<ID3D11ClassInstance*, 256> _vsClasses{};
            std::array<ID3D11ClassInstance*, 256> _psClasses{};
            std::array<ID3D11ClassInstance*, 256> _gsClasses{};
            std::array<ID3D11ClassInstance*, 256> _hsClasses{};
            std::array<ID3D11ClassInstance*, 256> _dsClasses{};
            UINT _vsClassCount = 0;
            UINT _psClassCount = 0;
            UINT _gsClassCount = 0;
            UINT _hsClassCount = 0;
            UINT _dsClassCount = 0;
            ComPtr<ID3D11Buffer> _vsConstantBuffer;
            ComPtr<ID3D11Buffer> _psConstantBuffer;
            ComPtr<ID3D11ShaderResourceView> _psResource;
            ComPtr<ID3D11SamplerState> _psSampler;

            std::array<D3D11_VIEWPORT,
                       D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                _viewports{};
            UINT _viewportCount = 0;
            ComPtr<ID3D11RasterizerState> _rasterizerState;

            ComPtr<ID3D11BlendState> _blendState;
            std::array<float, 4> _blendFactor{};
            UINT _sampleMask = 0xFFFFFFFFu;
            ComPtr<ID3D11DepthStencilState> _depthStencilState;
            UINT _stencilRef = 0;
            std::array<ComPtr<ID3D11RenderTargetView>,
                       D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
                _renderTargets{};
            ComPtr<ID3D11DepthStencilView> _depthStencilView;
        };
    }

    struct HighwayFullscreenLayerD3D::Impl {
        ID3D11Device* deviceRaw = nullptr;
        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ComPtr<ID3D11InputLayout> inputLayout;
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
        ComPtr<ID3D11Buffer> constantBuffer;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11BlendState> blendState;
        ComPtr<ID3D11DepthStencilState> depthStencilState;
        ComPtr<ID3D11RasterizerState> rasterizerState;
        ComPtr<ID3D11ShaderResourceView> texture;
        std::string texturePath;
        std::uint32_t texWidth = 0;
        std::uint32_t texHeight = 0;
        bool textureAttempted = false;
        bool deviceFailed = false;
        bool loggedNoDevice = false;
        bool loggedNoTarget = false;

        void ResetDeviceResources() {
            vertexShader.Reset();
            pixelShader.Reset();
            inputLayout.Reset();
            vertexBuffer.Reset();
            indexBuffer.Reset();
            constantBuffer.Reset();
            sampler.Reset();
            blendState.Reset();
            depthStencilState.Reset();
            rasterizerState.Reset();
            texture.Reset();
            textureAttempted = false;
            deviceFailed = false;
            deviceRaw = nullptr;
        }

        void Refresh() {
            texture.Reset();
            texturePath.clear();
            texWidth = texHeight = 0;
            textureAttempted = false;
        }

        bool EnsureDeviceResources(ID3D11Device* device) {
            if (!device) return false;
            if (deviceRaw == device && deviceFailed) return false;
            if (deviceRaw == device && vertexShader && pixelShader &&
                inputLayout && vertexBuffer && indexBuffer && constantBuffer &&
                sampler && blendState && depthStencilState &&
                rasterizerState) {
                return true;
            }
            ResetDeviceResources();
            deviceRaw = device;

            const auto fail = [this](std::string_view message) {
                deviceFailed = true;
                spdlog::warn("[render] fullscreen layer {}", message);
                return false;
            };

            ComPtr<ID3DBlob> vsBlob;
            ComPtr<ID3DBlob> psBlob;
            if (!CompileShader("VSMain", "vs_4_0", vsBlob) ||
                !CompileShader("PSMain", "ps_4_0", psBlob)) {
                deviceFailed = true;
                return false;
            }
            HRESULT hr = device->CreateVertexShader(
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                nullptr, &vertexShader);
            if (FAILED(hr)) {
                return fail(HrText("CreateVertexShader", hr));
            }
            hr = device->CreatePixelShader(
                psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                &pixelShader);
            if (FAILED(hr)) {
                return fail(HrText("CreatePixelShader", hr));
            }

            const D3D11_INPUT_ELEMENT_DESC inputDesc[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
                  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = device->CreateInputLayout(
                inputDesc, 2, vsBlob->GetBufferPointer(),
                vsBlob->GetBufferSize(), &inputLayout);
            if (FAILED(hr)) {
                return fail(HrText("CreateInputLayout", hr));
            }

            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(Vertex) * 4;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&desc, nullptr, &vertexBuffer);
            if (FAILED(hr)) {
                return fail(HrText("CreateBuffer(vertex)", hr));
            }

            const std::uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
            desc = {};
            desc.ByteWidth = sizeof(indices);
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = indices;
            hr = device->CreateBuffer(&desc, &init, &indexBuffer);
            if (FAILED(hr)) {
                return fail(HrText("CreateBuffer(index)", hr));
            }

            desc = {};
            desc.ByteWidth = sizeof(Constants);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            hr = device->CreateBuffer(&desc, nullptr, &constantBuffer);
            if (FAILED(hr)) {
                return fail(HrText("CreateBuffer(constants)", hr));
            }

            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            hr = device->CreateSamplerState(&samplerDesc, &sampler);
            if (FAILED(hr)) {
                return fail(HrText("CreateSamplerState", hr));
            }

            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha =
                D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask =
                D3D11_COLOR_WRITE_ENABLE_ALL;
            hr = device->CreateBlendState(&blend, &blendState);
            if (FAILED(hr)) {
                return fail(HrText("CreateBlendState", hr));
            }

            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = FALSE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            hr = device->CreateDepthStencilState(&depth, &depthStencilState);
            if (FAILED(hr)) {
                return fail(HrText("CreateDepthStencilState", hr));
            }

            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = FALSE;
            raster.ScissorEnable = FALSE;
            hr = device->CreateRasterizerState(&raster, &rasterizerState);
            if (FAILED(hr)) {
                return fail(HrText("CreateRasterizerState", hr));
            }
            return true;
        }

        bool EnsureTexture(ID3D11Device* device, std::string_view path) {
            if (texture && texturePath == path) return true;
            if (texturePath == path && textureAttempted) return false;

            texture.Reset();
            texturePath = std::string(path);
            textureAttempted = true;

            std::vector<std::uint8_t> pixels;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            if (!LoadImageRGBA8(path, pixels, width, height)) return false;

            D3D11_TEXTURE2D_DESC texDesc{};
            texDesc.Width = width;
            texDesc.Height = height;
            texDesc.MipLevels = 1;
            texDesc.ArraySize = 1;
            texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texDesc.SampleDesc.Count = 1;
            texDesc.Usage = D3D11_USAGE_IMMUTABLE;
            texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = pixels.data();
            data.SysMemPitch = width * 4;
            ComPtr<ID3D11Texture2D> d3dTexture;
            HRESULT hr = device->CreateTexture2D(&texDesc, &data, &d3dTexture);
            if (FAILED(hr)) {
                spdlog::warn("[render] fullscreen layer texture: {}",
                             HrText("CreateTexture2D", hr));
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = texDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            hr = device->CreateShaderResourceView(d3dTexture.Get(), &srvDesc,
                                                  &texture);
            if (FAILED(hr)) {
                spdlog::warn("[render] fullscreen layer SRV: {}",
                             HrText("CreateShaderResourceView", hr));
                return false;
            }
            texWidth = width;
            texHeight = height;
            return true;
        }
    };

    HighwayFullscreenLayerD3D::HighwayFullscreenLayerD3D() :
        _impl(std::make_unique<Impl>()) {}
    HighwayFullscreenLayerD3D::~HighwayFullscreenLayerD3D() = default;

    void HighwayFullscreenLayerD3D::Refresh() {
        if (_impl) _impl->Refresh();
    }

    bool HighwayFullscreenLayerD3D::Render(std::string_view path,
                                           const RGBA& tint,
                                           const View& view) {
        if (!_impl || path.empty() || view.w <= 0.0f || view.h <= 0.0f) {
            return false;
        }

        auto* manager = RE::BSRenderManager::GetSingleton();
        if (!manager) return false;
        auto& runtime = manager->GetRuntimeData();
        ID3D11Device* device = runtime.forwarder;
        ID3D11DeviceContext* context = runtime.context;
        if (!device || !context) {
            if (!_impl->loggedNoDevice) {
                _impl->loggedNoDevice = true;
                spdlog::warn(
                    "[render] fullscreen layer D3D device/context is not "
                    "available");
            }
            return false;
        }

        if (!_impl->EnsureDeviceResources(device) ||
            !_impl->EnsureTexture(device, path)) {
            return false;
        }
        if (_impl->texWidth == 0 || _impl->texHeight == 0) return false;

        D3DStateBackup state(context);
        ID3D11RenderTargetView* rtv = state.RenderTarget();
        if (!rtv) {
            rtv = reinterpret_cast<ID3D11RenderTargetView*>(
                runtime.renderView);
        }
        if (!rtv) {
            if (!_impl->loggedNoTarget) {
                _impl->loggedNoTarget = true;
                spdlog::warn(
                    "[render] fullscreen layer render target is not "
                    "available");
            }
            return false;
        }

        const D3D11_VIEWPORT vp = state.FirstViewport(view);
        if (vp.Width <= 0.0f || vp.Height <= 0.0f) return false;

        // Scale to fit, never stretch: pick the smaller of the two axis
        // scales so the whole image lands on screen, then center it.
        // Nothing is drawn in the leftover margin.
        const float scale = std::min(
            vp.Width / static_cast<float>(_impl->texWidth),
            vp.Height / static_cast<float>(_impl->texHeight));
        const float drawW = static_cast<float>(_impl->texWidth) * scale;
        const float drawH = static_cast<float>(_impl->texHeight) * scale;
        const float x0 = (vp.Width - drawW) * 0.5f;
        const float y0 = (vp.Height - drawH) * 0.5f;

        const Vertex vertices[4] = {
            { x0, y0, 0.0f, 0.0f },
            { x0 + drawW, y0, 1.0f, 0.0f },
            { x0 + drawW, y0 + drawH, 1.0f, 1.0f },
            { x0, y0 + drawH, 0.0f, 1.0f },
        };

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = context->Map(_impl->vertexBuffer.Get(), 0,
                                  D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::warn("[render] fullscreen layer {}",
                         HrText("Map(vertex)", hr));
            return false;
        }
        std::memcpy(mapped.pData, vertices, sizeof(vertices));
        context->Unmap(_impl->vertexBuffer.Get(), 0);

        const Constants constants{
            { vp.Width, vp.Height, 0.0f, 0.0f },
            { tint.r, tint.g, tint.b, tint.a },
        };
        hr = context->Map(_impl->constantBuffer.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            spdlog::warn("[render] fullscreen layer {}",
                         HrText("Map(constants)", hr));
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(_impl->constantBuffer.Get(), 0);

        context->RSSetViewports(1, &vp);
        context->RSSetState(_impl->rasterizerState.Get());
        context->OMSetRenderTargets(1, &rtv, nullptr);
        const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(_impl->blendState.Get(), blendFactor,
                                 0xFFFFFFFFu);
        context->OMSetDepthStencilState(_impl->depthStencilState.Get(), 0);

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ID3D11Buffer* vb = _impl->vertexBuffer.Get();
        context->IASetInputLayout(_impl->inputLayout.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(_impl->indexBuffer.Get(),
                                  DXGI_FORMAT_R16_UINT, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11Buffer* cb = _impl->constantBuffer.Get();
        ID3D11ShaderResourceView* srv = _impl->texture.Get();
        ID3D11SamplerState* sampler = _impl->sampler.Get();
        context->VSSetShader(_impl->vertexShader.Get(), nullptr, 0);
        context->PSSetShader(_impl->pixelShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, &sampler);
        context->DrawIndexed(6, 0, 0);
        return true;
    }
}
