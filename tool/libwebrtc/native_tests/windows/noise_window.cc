// A borderless window that fills a monitor with high-entropy noise, drawn on the
// GPU at the display's refresh rate. Used to give a screen capture a realistic
// worst case (a constant, high bitrate) instead of the near-static desktop.
//
//   noise_window.exe X Y WIDTH HEIGHT [SECONDS] [BLOCK_PIXELS] [AMPLITUDE]
//
// X,Y,WIDTH,HEIGHT is the monitor's rectangle in virtual-desktop coordinates
// (display_tool list). BLOCK_PIXELS (default 4) is the size of each random
// colour block: 1 is pure per-pixel noise, larger blocks compress better.
// AMPLITUDE (default 1.0) scales the noise: 1.0 is full-range white noise that
// no constant-bitrate encoder can squeeze to a normal target (it is already at
// the maximum quantiser); 0.1 to 0.3 is heavy but controllable.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

static const char* kShader = R"(
cbuffer C : register(b0) { uint frame; uint block; float amp; uint pad; };
float4 VS(uint id : SV_VertexID, out float2 uv : TEXCOORD) : SV_Position {
  uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint hash(uint x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}
float4 PS(float4 pos : SV_Position) : SV_Target {
  uint2 p = uint2(pos.xy) / block;
  uint h = hash(p.x * 73856093U ^ p.y * 19349663U ^ frame * 83492791U);
  float3 n = float3(h & 255, (h >> 8) & 255, (h >> 16) & 255) / 255.0;
  // A slowly drifting base colour keeps the picture from being pure noise.
  float3 base = 0.5 + 0.4 * sin(float3(pos.x, pos.y, pos.x + pos.y) * 0.003 + frame * 0.02);
  return float4(saturate(base + (n - 0.5) * amp), 1);
}
)";

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProc(h, m, w, l);
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: noise_window X Y W H [seconds] [block]\n");
    return 2;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const int x = std::atoi(argv[1]), y = std::atoi(argv[2]);
  const int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
  const int seconds = argc > 5 ? std::atoi(argv[5]) : 60;
  const unsigned block = argc > 6 ? std::atoi(argv[6]) : 4;
  const float amplitude = argc > 7 ? static_cast<float>(std::atof(argv[7])) : 1.0f;

  WNDCLASSA wc = {};
  wc.lpfnWndProc = Proc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = "noise_window";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, "noise",
                              WS_POPUP | WS_VISIBLE, x, y, w, h, nullptr, nullptr,
                              wc.hInstance, nullptr);
  if (!hwnd) return 3;
  ShowWindow(hwnd, SW_SHOW);

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  // The window's own GPU: let DXGI pick the adapter that owns the monitor.
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 4;
  ComPtr<IDXGIDevice> dxgi_dev;
  dev.As(&dxgi_dev);
  ComPtr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = w;
  sd.Height = h;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc;
  if (FAILED(factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &sc))) return 5;

  ComPtr<ID3DBlob> vs_blob, ps_blob, err;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs_blob, &err)) ||
      FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps_blob, &err))) {
    if (err) std::fprintf(stderr, "%s\n", static_cast<const char*>(err->GetBufferPointer()));
    return 6;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
  dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);
  struct { unsigned frame, block; float amp; unsigned pad; } cb = {0, block ? block : 1, amplitude, 0};
  D3D11_BUFFER_DESC bd = {sizeof(cb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  ComPtr<ID3D11Buffer> cbuf;
  dev->CreateBuffer(&bd, nullptr, &cbuf);

  ComPtr<ID3D11Texture2D> back;
  sc->GetBuffer(0, IID_PPV_ARGS(&back));
  ComPtr<ID3D11RenderTargetView> rtv;
  dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);

  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  MSG msg;
  unsigned frames = 0;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < end) {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) goto done;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    ctx->Map(cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    cb.frame = frames;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(cbuf.Get(), 0);
    D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    sc->Present(1, 0);
    ++frames;
  }
done:
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("noise_window: %u frames in %.1f s = %.1f fps\n", frames, secs, frames / secs);
  return 0;
}
