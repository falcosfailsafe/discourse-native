// Draws a known HDR test pattern on a monitor: a horizontal ramp of grey in
// linear scRGB (1.0 = 80 nits, SDR white at the default level) from 0.0 to
// RAMP_MAX across the window, in a floating point swap chain tagged as scRGB.
// With Windows HDR on, desktop duplication then returns FP16 frames whose
// values are known, so a conversion to SDR can be measured.
//
//   hdr_ramp.exe X Y WIDTH HEIGHT [SECONDS] [RAMP_MAX]
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

static const char* kShader = R"(
cbuffer C : register(b0) { float ramp_max; float w; float2 pad; };
float4 VS(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position) : SV_Target {
  // A thin black line every 1/16 of the width marks the sample columns.
  float t = pos.x / w;
  float v = t * ramp_max;
  return float4(v, v, v, 1);
}
)";

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProc(h, m, w, l);
}

int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: hdr_ramp X Y W H [seconds] [ramp_max]\n"); return 2; }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const int x = std::atoi(argv[1]), y = std::atoi(argv[2]), w = std::atoi(argv[3]), h = std::atoi(argv[4]);
  const int seconds = argc > 5 ? std::atoi(argv[5]) : 60;
  const float ramp_max = argc > 6 ? static_cast<float>(std::atof(argv[6])) : 6.0f;

  WNDCLASSA wc = {};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = "hdr_ramp";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, "hdr", WS_POPUP | WS_VISIBLE,
                              x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) return 3;

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 4;
  ComPtr<IDXGIDevice> dd; dev.As(&dd);
  ComPtr<IDXGIAdapter> ad; dd->GetAdapter(&ad);
  ComPtr<IDXGIFactory2> fac; ad->GetParent(IID_PPV_ARGS(&fac));
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = w; sd.Height = h;
  sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc1;
  if (FAILED(fac->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) return 5;
  ComPtr<IDXGISwapChain3> sc;
  sc1.As(&sc);
  UINT support = 0;
  sc->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
  std::printf("scRGB present support: %u\n", support);
  sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

  ComPtr<ID3DBlob> vsb, psb, err;
  if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err))) return 6;
  ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
  dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
  dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
  struct { float ramp_max, w, pad[2]; } cb = {ramp_max, static_cast<float>(w), {0, 0}};
  D3D11_BUFFER_DESC bd = {sizeof(cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SUBRESOURCE_DATA init = {&cb, 0, 0};
  ComPtr<ID3D11Buffer> cbuf; dev->CreateBuffer(&bd, &init, &cbuf);
  ComPtr<ID3D11Texture2D> back; sc->GetBuffer(0, IID_PPV_ARGS(&back));
  ComPtr<ID3D11RenderTargetView> rtv; dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  MSG msg;
  while (std::chrono::steady_clock::now() < end) {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    sc->Present(1, 0);
  }
  return 0;
}
