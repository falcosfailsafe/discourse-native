// Can the video processor turn an HDR desktop capture into usable SDR NV12?
//
// Duplicates a monitor the way the shared screen share does, but with
// DuplicateOutput1 asking for FP16 as well as BGRA8, then converts one frame
// through ID3D11VideoProcessor to a half-size NV12 texture with explicit colour
// spaces (input: scRGB, output: BT.709 limited range), reads it back, and prints
// how a known ramp (see hdr_ramp.cc) maps: source scRGB value -> output luma.
//
//   hdr_capture_probe.exe [display-index] [output.nv12]
// With a clip at SDR white the mapping stops rising at scRGB 1.0; with tone
// mapping it keeps rising and compresses the range above 1.0.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static uint16_t* g_unused;
static float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
  float f;
  if (exp == 0) f = std::ldexp(static_cast<float>(man), -24);
  else if (exp == 31) f = man ? NAN : INFINITY;
  else f = std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
  return sign ? -f : f;
}

#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s -> 0x%08lX\n", #x, static_cast<unsigned long>(hr_)); return 1; } } while (0)

int main(int argc, char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const int want = argc > 1 ? std::atoi(argv[1]) : 0;
  const char* out_path = argc > 2 ? argv[2] : "hdr_probe.nv12";
  const bool legacy = argc > 3 && std::strcmp(argv[3], "--legacy") == 0;

  ComPtr<IDXGIFactory1> factory;
  CK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  int seen = 0;
  for (UINT a = 0; !output; ++a) {
    ComPtr<IDXGIAdapter1> cand;
    if (factory->EnumAdapters1(a, &cand) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d; cand->GetDesc1(&d);
    if (d.VendorId != 0x10DE) continue;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> out;
      if (FAILED(cand->EnumOutputs(o, &out))) break;
      DXGI_OUTPUT_DESC od; out->GetDesc(&od);
      if (!od.AttachedToDesktop) continue;
      if (seen++ == want) { adapter = cand; output = out; break; }
    }
  }
  if (!output) { std::printf("no NVIDIA output %d\n", want); return 1; }
  ComPtr<IDXGIOutput6> out6;
  output.As(&out6);
  DXGI_OUTPUT_DESC1 d1 = {};
  if (out6) {
    out6->GetDesc1(&d1);
    std::printf("output: colour space %d, bits/colour %u, max luminance %.0f nits, min %.4f, SDR white n/a here\n",
                static_cast<int>(d1.ColorSpace), d1.BitsPerColor, d1.MaxLuminance, d1.MinLuminance);
  }
  DXGI_OUTPUT_DESC od; output->GetDesc(&od);
  const int src_w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
  const int src_h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
  std::printf("desktop %dx%d\n", src_w, src_h);

  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  CK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2,
                       D3D11_SDK_VERSION, &dev, nullptr, &ctx));

  ComPtr<IDXGIOutput5> out5;
  CK(output.As(&out5));
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
  ComPtr<IDXGIOutputDuplication> dupl;
  if (legacy) {
    ComPtr<IDXGIOutput1> out1;
    CK(output.As(&out1));
    CK(out1->DuplicateOutput(dev.Get(), &dupl));
    std::printf("(using the legacy DuplicateOutput)\n");
  } else {
    CK(out5->DuplicateOutput1(dev.Get(), 0, 2, formats, &dupl));
  }
  DXGI_OUTDUPL_DESC dd; dupl->GetDesc(&dd);
  std::printf("duplication mode %ux%u format %d (10 = FP16 scRGB, 87 = BGRA8)\n", dd.ModeDesc.Width, dd.ModeDesc.Height,
              static_cast<int>(dd.ModeDesc.Format));

  ComPtr<ID3D11Texture2D> frame;
  for (int i = 0; i < 100 && !frame; ++i) {
    DXGI_OUTDUPL_FRAME_INFO info;
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl->AcquireNextFrame(100, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
    CK(hr);
    ComPtr<ID3D11Texture2D> tex; res.As(&tex);
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    // As the real pipeline's textures: usable by shaders and as video processor input.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = 0;
    CK(dev->CreateTexture2D(&td, nullptr, &frame));
    ctx->CopyResource(frame.Get(), tex.Get());
    dupl->ReleaseFrame();
  }
  if (!frame) { std::printf("no frame acquired\n"); return 1; }
  D3D11_TEXTURE2D_DESC fd; frame->GetDesc(&fd);
  std::printf("frame texture %ux%u format %d\n", fd.Width, fd.Height, static_cast<int>(fd.Format));

  // Source samples: the middle row, 17 columns across the width.
  D3D11_TEXTURE2D_DESC sd = fd;
  sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> src_stage;
  CK(dev->CreateTexture2D(&sd, nullptr, &src_stage));
  ctx->CopyResource(src_stage.Get(), frame.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  CK(ctx->Map(src_stage.Get(), 0, D3D11_MAP_READ, 0, &m));
  const int row = fd.Height / 2;
  std::vector<float> src_vals;
  for (int k = 0; k <= 16; ++k) {
    const int col = std::min<int>(fd.Width - 1, k * fd.Width / 16);
    float v;
    if (fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + row * m.RowPitch) + col * 4;
      v = HalfToFloat(p[0]);
    } else {
      const uint8_t* p = static_cast<const uint8_t*>(m.pData) + row * m.RowPitch + col * 4;
      v = p[2] / 255.0f;
    }
    src_vals.push_back(v);
  }
  ctx->Unmap(src_stage.Get(), 0);

  // Convert through the video processor.
  const int out_w = (src_w / 2) & ~1, out_h = (src_h / 2) & ~1;
  ComPtr<ID3D11VideoDevice> vdev; ComPtr<ID3D11VideoContext> vctx; ComPtr<ID3D11VideoContext1> vctx1;
  CK(dev.As(&vdev)); CK(ctx.As(&vctx)); CK(vctx.As(&vctx1));
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputFrameRate = {60, 1}; cd.OutputFrameRate = {60, 1};
  cd.InputWidth = fd.Width; cd.InputHeight = fd.Height; cd.OutputWidth = out_w; cd.OutputHeight = out_h;
  const bool fp16_unused = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  ComPtr<ID3D11VideoProcessorEnumerator> en;
  CK(vdev->CreateVideoProcessorEnumerator(&cd, &en));
  UINT in_sup = 0, out_sup = 0;
  en->CheckVideoProcessorFormat(fd.Format, &in_sup);
  en->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &out_sup);
  std::printf("video processor: input format supported 0x%x, NV12 output 0x%x\n", in_sup, out_sup);
  ComPtr<ID3D11VideoProcessorEnumerator1> en1;
  if (SUCCEEDED(en.As(&en1))) {
    BOOL ok = FALSE;
    const DXGI_COLOR_SPACE_TYPE in_cs = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                                    : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HRESULT hr = en1->CheckVideoProcessorFormatConversion(fd.Format, in_cs, DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709, &ok);
    std::printf("conversion %d -> NV12 BT.709 supported: hr=0x%08lX %s\n", static_cast<int>(in_cs), static_cast<unsigned long>(hr), ok ? "yes" : "NO");
  }
  // The video processor cannot take FP16 input, so a pixel shader tone-maps the
  // scRGB frame to BGRA8 first (SDR range kept, highlights rolled off).
  ComPtr<ID3D11Texture2D> sdr;
  bool shader_pass = !(in_sup & 1);
  double shader_ms = 0;
  if (shader_pass) {
    static const char* kHlsl = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 VS(uint id : SV_VertexID, out float2 uv : TEXCOORD) : SV_Position {
  uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 Shoulder(float3 x) {
  // Identity up to 0.8 (an SDR picture is untouched), then a smooth roll-off
  // that approaches 1.0, so highlights above SDR white are compressed, not clipped.
  float3 over = max(x - 0.8, 0);
  // A long, gentle shoulder: the next 0.2 of output range is spread over a wide
  // span of highlight values, so gradation above SDR white survives.
  return min(x, 0.8) + 0.2 * (1.0 - exp(-over / 1.5));
}
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
  float3 c = max(tex.Sample(smp, uv).rgb, 0);          // scRGB, 1.0 = 80 nits
  float3 m = Shoulder(c);
  return float4(pow(saturate(m), 1.0 / 2.2), 1);       // sRGB-like transfer for the BT.709 conversion
}
)";
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psb, &err))) {
      if (err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
      return 1;
    }
    ComPtr<ID3D11VertexShader> vsh; ComPtr<ID3D11PixelShader> psh;
    CK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vsh));
    CK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &psh));
    D3D11_TEXTURE2D_DESC bd2 = {};
    bd2.Width = out_w; bd2.Height = out_h; bd2.MipLevels = 1; bd2.ArraySize = 1;
    bd2.Format = DXGI_FORMAT_B8G8R8A8_UNORM; bd2.SampleDesc.Count = 1; bd2.Usage = D3D11_USAGE_DEFAULT;
    bd2.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CK(dev->CreateTexture2D(&bd2, nullptr, &sdr));
    ComPtr<ID3D11RenderTargetView> rtv2; CK(dev->CreateRenderTargetView(sdr.Get(), nullptr, &rtv2));
    ComPtr<ID3D11ShaderResourceView> srv; CK(dev->CreateShaderResourceView(frame.Get(), nullptr, &srv));
    D3D11_SAMPLER_DESC sm = {}; sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> samp; CK(dev->CreateSamplerState(&sm, &samp));
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; ComPtr<ID3D11Query> done; CK(dev->CreateQuery(&qd, &done));
    auto draw = [&] {
      D3D11_VIEWPORT vpt = {0, 0, static_cast<float>(out_w), static_cast<float>(out_h), 0, 1};
      ctx->RSSetViewports(1, &vpt);
      ctx->OMSetRenderTargets(1, rtv2.GetAddressOf(), nullptr);
      ctx->VSSetShader(vsh.Get(), nullptr, 0); ctx->PSSetShader(psh.Get(), nullptr, 0);
      ctx->PSSetShaderResources(0, 1, srv.GetAddressOf()); ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->Draw(3, 0);
      ctx->End(done.Get()); ctx->Flush();
      BOOL fin = FALSE;
      while (ctx->GetData(done.Get(), &fin, sizeof(fin), 0) != S_OK || !fin) {}
    };
    draw();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) draw();
    shader_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 200;
    std::printf("FP16 input is not supported by the video processor: using a tone-mapping pixel pass first (%.2f ms per frame at %dx%d -> %dx%d)\n",
                shader_ms, fd.Width, fd.Height, out_w, out_h);
  }
  ComPtr<ID3D11VideoProcessor> vp;
  CK(vdev->CreateVideoProcessor(en.Get(), 0, &vp));
  const DXGI_COLOR_SPACE_TYPE in_cs = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                                                  : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, shader_pass ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 : in_cs);
  vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
  vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
  const RECT sr = {0, 0, static_cast<LONG>(shader_pass ? out_w : fd.Width), static_cast<LONG>(shader_pass ? out_h : fd.Height)}, dr = {0, 0, out_w, out_h};
  vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &sr);
  vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dr);
  vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dr);

  D3D11_TEXTURE2D_DESC nd = {};
  nd.Width = out_w; nd.Height = out_h; nd.MipLevels = 1; nd.ArraySize = 1;
  nd.Format = DXGI_FORMAT_NV12; nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> nv12;
  CK(dev->CreateTexture2D(&nd, nullptr, &nv12));
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {}; ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorOutputView> outv;
  CK(vdev->CreateVideoProcessorOutputView(nv12.Get(), en.Get(), &ov, &outv));
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorInputView> inv;
  CK(vdev->CreateVideoProcessorInputView(shader_pass ? sdr.Get() : frame.Get(), en.Get(), &iv, &inv));
  D3D11_VIDEO_PROCESSOR_STREAM st = {}; st.Enable = TRUE; st.pInputSurface = inv.Get();
  HRESULT hr = vctx->VideoProcessorBlt(vp.Get(), outv.Get(), 0, 1, &st);
  std::printf("VideoProcessorBlt: 0x%08lX\n", static_cast<unsigned long>(hr));
  if (FAILED(hr)) return 1;

  nd.Usage = D3D11_USAGE_STAGING; nd.BindFlags = 0; nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> nv_stage;
  CK(dev->CreateTexture2D(&nd, nullptr, &nv_stage));
  ctx->CopyResource(nv_stage.Get(), nv12.Get());
  CK(ctx->Map(nv_stage.Get(), 0, D3D11_MAP_READ, 0, &m));
  const uint8_t* base = static_cast<const uint8_t*>(m.pData);
  FILE* f = std::fopen(out_path, "wb");
  for (int y = 0; y < out_h; ++y) std::fwrite(base + y * m.RowPitch, 1, out_w, f);
  for (int y = 0; y < out_h / 2; ++y) std::fwrite(base + (out_h + y) * m.RowPitch, 1, out_w, f);
  std::fclose(f);
  std::printf("\nsource scRGB (80 nits = 1.0) -> output luma Y (BT.709 limited: 16 = black, 235 = white)\n");
  for (int k = 0; k <= 16; ++k) {
    const int col = std::min(out_w - 1, k * out_w / 16);
    const int Y = base[(out_h / 2) * m.RowPitch + col];
    std::printf("  column %2d/16: source %7.3f (%6.0f nits) -> Y %3d\n", k, src_vals[k], src_vals[k] * 80.0f, Y);
  }
  ctx->Unmap(nv_stage.Get(), 0);
  std::printf("wrote %s (%dx%d NV12)\n", out_path, out_w, out_h);
  return 0;
}
