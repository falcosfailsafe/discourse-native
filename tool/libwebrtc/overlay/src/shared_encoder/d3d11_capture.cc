#include "d3d11_capture.h"

#include <utility>

#ifndef _WIN32

namespace discourse {
namespace shared_encoder {

class D3D11Capture::Impl {};

std::unique_ptr<D3D11Capture> D3D11Capture::Create(int, int, int, int,
                                                   std::string* error) {
  if (error) *error = "D3D11 desktop capture is only available on Windows";
  return nullptr;
}
std::unique_ptr<D3D11Capture> D3D11Capture::Create(const std::string&, int,
                                                   int, int,
                                                   std::string* error) {
  if (error) *error = "D3D11 desktop capture is only available on Windows";
  return nullptr;
}
bool D3D11Capture::QueryOutput(const std::string&, int*, int*,
                               std::string* error) {
  if (error) *error = "D3D11 desktop capture is only available on Windows";
  return false;
}
const std::string& D3D11Capture::adapter_name() const {
  static const std::string empty;
  return empty;
}
D3D11Capture::D3D11Capture(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
D3D11Capture::~D3D11Capture() = default;
void* D3D11Capture::device() const { return nullptr; }
const std::vector<void*>& D3D11Capture::textures() const {
  static const std::vector<void*> empty;
  return empty;
}
void* D3D11Capture::CaptureNext(std::string* error, size_t*) {
  if (error) *error = "D3D11 desktop capture is only available on Windows";
  return nullptr;
}
bool D3D11Capture::failed() const { return true; }
int D3D11Capture::width() const { return 0; }
int D3D11Capture::height() const { return 0; }
int D3D11Capture::target_width() const { return 0; }
int D3D11Capture::target_height() const { return 0; }
int D3D11Capture::frame_rate() const { return 0; }

}  // namespace shared_encoder
}  // namespace discourse

#else  // _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace discourse {
namespace shared_encoder {
namespace {

constexpr int kMaxTargetDimension = 8192;
constexpr int kMaxFrameRate = 240;
// Consecutive lost-duplication recoveries tolerated before giving up.
constexpr int kMaxConsecutiveRecoveryFailures = 20;

std::string Hex(HRESULT hr) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

std::string Failure(const char* what, HRESULT hr) {
  return std::string(what) + " failed (" + Hex(hr) + ")";
}

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

// Which monitor to capture: the n-th attached output, or a screen id string.
struct OutputSelector {
  int index = -1;
  std::string id;
  bool by_id = false;
};

bool ParseDecimal(const std::string& text, unsigned long long* value) {
  if (text.empty() || text.size() > 20) return false;
  unsigned long long result = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const unsigned digit = static_cast<unsigned>(ch - '0');
    if (result > (~0ULL - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

std::string Utf8(const wchar_t* wide) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return std::string();
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], size, nullptr, nullptr);
  out.resize(static_cast<size_t>(size) - 1);
  return out;
}

bool IsPrimary(const DXGI_OUTPUT_DESC& desc) {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  return GetMonitorInfoW(desc.Monitor, &info) &&
         (info.dwFlags & MONITORINFOF_PRIMARY);
}

// Whether `desc` is the output a screen id names (see the header).
bool MatchesSourceId(const std::string& id, const DXGI_OUTPUT_DESC& desc) {
  if (id.empty()) return IsPrimary(desc);
  unsigned long long value = 0;
  if (!ParseDecimal(id, &value)) return false;
  if (value == static_cast<unsigned long long>(
                   reinterpret_cast<uintptr_t>(desc.Monitor))) {
    return true;
  }
  if (value < 1024) {
    DISPLAY_DEVICEW device = {};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(nullptr, static_cast<DWORD>(value), &device, 0)) {
      return wcsncmp(device.DeviceName, desc.DeviceName,
                     ARRAYSIZE(desc.DeviceName)) == 0;
    }
  }
  return false;
}

// Selects an output attached to the desktop, across adapters, so the monitor
// is always paired with the GPU that drives it.
bool FindOutput(const OutputSelector& selector, ComPtr<IDXGIAdapter1>* adapter,
                ComPtr<IDXGIOutput>* output, std::string* error) {
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    SetError(error, Failure("CreateDXGIFactory1", hr));
    return false;
  }
  int seen = 0;
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> candidate;
    if (factory->EnumAdapters1(a, &candidate) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (FAILED(candidate->GetDesc1(&adapter_desc)) ||
        (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      continue;  // Never duplicate through the WARP software adapter.
    }
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> candidate_output;
      if (candidate->EnumOutputs(o, &candidate_output) ==
          DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_OUTPUT_DESC desc;
      if (FAILED(candidate_output->GetDesc(&desc)) ||
          !desc.AttachedToDesktop) {
        continue;
      }
      const bool match = selector.by_id ? MatchesSourceId(selector.id, desc)
                                        : seen == selector.index;
      ++seen;
      if (match) {
        *adapter = candidate;
        *output = candidate_output;
        return true;
      }
    }
  }
  if (selector.by_id) {
    SetError(error, "no attached display matches screen id '" + selector.id +
                        "'");
  } else {
    SetError(error, "no desktop output with index " +
                        std::to_string(selector.index) + " (found " +
                        std::to_string(seen) + ")");
  }
  return false;
}

// Refuses rotated outputs and non-SDR color spaces. The duplication mode can
// report a different format than the texture returned by AcquireNextFrame.
// CaptureNext checks the texture's BGRA8 format before conversion.
bool CheckOutputSupported(IDXGIOutput* output,
                          const DXGI_OUTDUPL_DESC* duplication,
                          std::string* error) {
  DXGI_OUTPUT_DESC desc;
  HRESULT hr = output->GetDesc(&desc);
  if (FAILED(hr)) {
    SetError(error, Failure("IDXGIOutput::GetDesc", hr));
    return false;
  }
  if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
      desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
    SetError(error, "rotated displays are not supported");
    return false;
  }
  if (duplication != nullptr) {
    if (duplication->Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        duplication->Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
      SetError(error, "rotated displays are not supported");
      return false;
    }
  }
  ComPtr<IDXGIOutput6> output6;
  if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
    DXGI_OUTPUT_DESC1 desc1;
    if (SUCCEEDED(output6->GetDesc1(&desc1)) &&
        desc1.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
      SetError(error,
               "the display uses HDR or a wide color space; screen share "
               "needs an SDR display");
      return false;
    }
  }
  return true;
}

}  // namespace

class D3D11Capture::Impl {
 public:
  bool Init(const OutputSelector& selector, int target_width,
            int target_height, int frame_rate, std::string* error);
  void* CaptureNext(std::string* error, size_t* texture_index);

  bool failed_ = false;
  int source_width_ = 0;
  int source_height_ = 0;
  int target_width_ = 0;
  int target_height_ = 0;
  int frame_rate_ = 0;
  ComPtr<ID3D11Device> device_;
  std::vector<void*> textures_;
  std::string adapter_name_;

 private:
  // RAII for IDXGIOutputDuplication::AcquireNextFrame/ReleaseFrame.
  class AcquiredFrame {
   public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication)
        : duplication_(duplication) {}
    ~AcquiredFrame() {
      if (held_) duplication_->ReleaseFrame();
    }
    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;

    HRESULT Acquire(UINT timeout_ms) {
      HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &info_,
                                                  &resource_);
      held_ = SUCCEEDED(hr);
      return hr;
    }
    const DXGI_OUTDUPL_FRAME_INFO& info() const { return info_; }
    IDXGIResource* resource() const { return resource_.Get(); }

   private:
    IDXGIOutputDuplication* duplication_;
    DXGI_OUTDUPL_FRAME_INFO info_ = {};
    ComPtr<IDXGIResource> resource_;
    bool held_ = false;
  };

  struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> view;
    // Signalled when the blit into this slot has finished on the GPU.
    ComPtr<ID3D11Query> done;
  };

  bool OpenDuplication(std::string* error);
  bool CreateVideoProcessor(std::string* error);
  bool CreatePool(std::string* error);
  void WaitForNextTick();
  // Blits `source` into the next ring slot and waits for the GPU.
  bool Convert(ID3D11Texture2D* source, std::string* error);
  void Fail(std::string* error, std::string message) {
    failed_ = true;
    SetError(error, std::move(message));
  }

  ComPtr<IDXGIAdapter1> adapter_;
  ComPtr<IDXGIOutput> output_;
  ComPtr<IDXGIOutputDuplication> duplication_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  Slot slots_[kPoolSize];
  size_t next_slot_ = 0;
  bool have_frame_ = false;
  size_t latest_slot_ = 0;
  int recovery_failures_ = 0;
  std::chrono::steady_clock::duration interval_;
  std::chrono::steady_clock::time_point next_tick_;
  bool ticking_ = false;
};

bool D3D11Capture::Impl::Init(const OutputSelector& selector,
                              int target_width, int target_height,
                              int frame_rate, std::string* error) {
  if (!selector.by_id && selector.index < 0) {
    SetError(error, "output_index must not be negative");
    return false;
  }
  if (target_width < 2 || target_height < 2 ||
      target_width > kMaxTargetDimension ||
      target_height > kMaxTargetDimension || (target_width % 2) != 0 ||
      (target_height % 2) != 0) {
    SetError(error, "target size must be even and within 2..8192");
    return false;
  }
  if (frame_rate < 1 || frame_rate > kMaxFrameRate) {
    SetError(error, "frame_rate must be within 1..240");
    return false;
  }
  target_width_ = target_width;
  target_height_ = target_height;
  frame_rate_ = frame_rate;
  interval_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(1.0 / frame_rate));

  if (!FindOutput(selector, &adapter_, &output_, error)) return false;
  {
    DXGI_ADAPTER_DESC1 adapter_desc;
    if (SUCCEEDED(adapter_->GetDesc1(&adapter_desc))) {
      adapter_name_ = Utf8(adapter_desc.Description);
    }
  }

  // Recovery recreates the duplication from output_, so the adapter and output
  // objects stay alive for the capture's lifetime.
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL obtained;
  UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
               D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, flags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &device_, &obtained,
                                 &context_);
  if (FAILED(hr)) {
    SetError(error, Failure("D3D11CreateDevice", hr));
    return false;
  }
  // NVENC drives the same device from its own thread.
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }

  if (!OpenDuplication(error)) return false;
  if (!CreateVideoProcessor(error)) return false;
  return CreatePool(error);
}

bool D3D11Capture::Impl::OpenDuplication(std::string* error) {
  ComPtr<IDXGIOutput1> output1;
  HRESULT hr = output_.As(&output1);
  if (FAILED(hr)) {
    SetError(error, Failure("IDXGIOutput1 (needs Windows 8 or later)", hr));
    return false;
  }
  duplication_.Reset();
  hr = output1->DuplicateOutput(device_.Get(), &duplication_);
  if (FAILED(hr)) {
    std::string reason = Failure("IDXGIOutput1::DuplicateOutput", hr);
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
      reason += ": too many desktop duplications are already open";
    } else if (hr == E_ACCESSDENIED) {
      reason += ": the secure desktop or another session owns the display";
    } else if (hr == DXGI_ERROR_UNSUPPORTED) {
      reason += ": the output is driven by a different GPU";
    }
    SetError(error, std::move(reason));
    return false;
  }
  DXGI_OUTDUPL_DESC desc;
  duplication_->GetDesc(&desc);
  if (!CheckOutputSupported(output_.Get(), &desc, error)) {
    duplication_.Reset();
    return false;
  }
  const int width = static_cast<int>(desc.ModeDesc.Width);
  const int height = static_cast<int>(desc.ModeDesc.Height);
  if (source_width_ != 0 &&
      (width != source_width_ || height != source_height_)) {
    SetError(error, "the display resolution changed during capture");
    duplication_.Reset();
    return false;
  }
  if (desc.DesktopImageInSystemMemory) {
    // The image would live in CPU memory; this pipeline never uploads.
    SetError(error, "the desktop image is in system memory, not on the GPU");
    duplication_.Reset();
    return false;
  }
  source_width_ = width;
  source_height_ = height;
  return true;
}

bool D3D11Capture::Impl::CreateVideoProcessor(std::string* error) {
  HRESULT hr = device_.As(&video_device_);
  if (FAILED(hr)) {
    SetError(error, Failure("ID3D11VideoDevice", hr));
    return false;
  }
  hr = context_.As(&video_context_);
  if (FAILED(hr)) {
    SetError(error, Failure("ID3D11VideoContext", hr));
    return false;
  }
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate = {static_cast<UINT>(frame_rate_), 1};
  content.InputWidth = static_cast<UINT>(source_width_);
  content.InputHeight = static_cast<UINT>(source_height_);
  content.OutputFrameRate = content.InputFrameRate;
  content.OutputWidth = static_cast<UINT>(target_width_);
  content.OutputHeight = static_cast<UINT>(target_height_);
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  hr = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
  if (FAILED(hr)) {
    SetError(error, Failure("CreateVideoProcessorEnumerator", hr));
    return false;
  }
  UINT input_support = 0;
  UINT output_support = 0;
  enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                         &input_support);
  enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_support);
  if (!(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
      !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
    SetError(error,
             "the GPU's video processor cannot convert BGRA to NV12");
    return false;
  }
  hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
  if (FAILED(hr)) {
    SetError(error, Failure("CreateVideoProcessor", hr));
    return false;
  }

  // Input: full-range sRGB. Output: BT.709, limited (16-235) range.
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space = {};
  input_space.RGB_Range = 0;  // 0-255
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space = {};
  output_space.YCbCr_Matrix = 1;  // BT.709
  output_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0,
                                                    &input_space);
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(),
                                                    &output_space);
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  // Plain scale + convert: no driver "enhancement" of the picture.
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(),
                                                            0, FALSE);
  const RECT source_rect = {0, 0, source_width_, source_height_};
  const RECT target_rect = {0, 0, target_width_, target_height_};
  video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                    &source_rect);
  video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE,
                                                  &target_rect);
  video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE,
                                                    &target_rect);
  return true;
}

bool D3D11Capture::Impl::CreatePool(std::string* error) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(target_width_);
  desc.Height = static_cast<UINT>(target_height_);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc = {};
  view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  view_desc.Texture2D.MipSlice = 0;

  D3D11_QUERY_DESC query_desc = {};
  query_desc.Query = D3D11_QUERY_EVENT;

  for (Slot& slot : slots_) {
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot.texture);
    if (FAILED(hr)) {
      SetError(error, Failure("CreateTexture2D(NV12)", hr));
      return false;
    }
    hr = video_device_->CreateVideoProcessorOutputView(
        slot.texture.Get(), enumerator_.Get(), &view_desc, &slot.view);
    if (FAILED(hr)) {
      SetError(error, Failure("CreateVideoProcessorOutputView", hr));
      return false;
    }
    hr = device_->CreateQuery(&query_desc, &slot.done);
    if (FAILED(hr)) {
      SetError(error, Failure("CreateQuery", hr));
      return false;
    }
    textures_.push_back(slot.texture.Get());
  }
  return true;
}

void D3D11Capture::Impl::WaitForNextTick() {
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  if (!ticking_) {
    ticking_ = true;
    next_tick_ = now;
  } else {
    next_tick_ += interval_;
    // Running late (a stall, or the consumer was slow): resume from now
    // instead of firing a burst of catch-up frames.
    if (next_tick_ < now - interval_) next_tick_ = now;
  }
  // Sleep coarsely, then yield-spin: Windows timers are ~15 ms granular.
  const auto coarse = next_tick_ - std::chrono::milliseconds(2);
  if (clock::now() < coarse) std::this_thread::sleep_until(coarse);
  while (clock::now() < next_tick_) std::this_thread::yield();
}

bool D3D11Capture::Impl::Convert(ID3D11Texture2D* source,
                                 std::string* error) {
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  HRESULT hr = video_device_->CreateVideoProcessorInputView(
      source, enumerator_.Get(), &input_desc, &input_view);
  if (FAILED(hr)) {
    Fail(error, Failure("CreateVideoProcessorInputView", hr));
    return false;
  }
  Slot& slot = slots_[next_slot_];
  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.Get();
  hr = video_context_->VideoProcessorBlt(processor_.Get(), slot.view.Get(), 0,
                                         1, &stream);
  if (FAILED(hr)) {
    Fail(error, Failure("VideoProcessorBlt", hr));
    return false;
  }
  // Make the result complete before anyone encodes it: NVENC reads the
  // texture right away. This waits for the GPU, it moves no pixels.
  context_->End(slot.done.Get());
  context_->Flush();
  BOOL finished = FALSE;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    hr = context_->GetData(slot.done.Get(), &finished, sizeof(finished), 0);
    if (hr == S_OK && finished) break;
    if (FAILED(hr)) {
      Fail(error, Failure("GPU completion query", hr));
      return false;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      Fail(error, "the GPU did not finish the NV12 conversion within 2 s");
      return false;
    }
    std::this_thread::yield();
  }
  latest_slot_ = next_slot_;
  next_slot_ = (next_slot_ + 1) % kPoolSize;
  have_frame_ = true;
  return true;
}

void* D3D11Capture::Impl::CaptureNext(std::string* error,
                                      size_t* texture_index) {
  if (error) error->clear();
  if (failed_) {
    SetError(error, "capture has failed and cannot continue");
    return nullptr;
  }
  WaitForNextTick();

  if (!duplication_) {
    // A previous call lost the duplication; try to get it back.
    std::string reason;
    if (!OpenDuplication(&reason)) {
      if (source_width_ != 0 && reason.find("resolution changed") !=
                                    std::string::npos) {
        Fail(error, reason);
      } else if (++recovery_failures_ >= kMaxConsecutiveRecoveryFailures) {
        Fail(error, "capture could not be restored: " + reason);
      } else {
        SetError(error, "capture is recovering: " + reason);
      }
      return nullptr;
    }
    recovery_failures_ = 0;
  }

  {
    AcquiredFrame frame(duplication_.Get());
    // The duplication keeps only the newest image, so one acquire at the tick
    // yields the desktop as of now, however fast the display refreshes.
    HRESULT hr = frame.Acquire(0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // Unchanged since the last tick.
    } else if (hr == DXGI_ERROR_ACCESS_LOST) {
      duplication_.Reset();
      SetError(error, "capture is recovering: duplication was lost");
      return nullptr;
    } else if (FAILED(hr)) {
      Fail(error, Failure("AcquireNextFrame", hr));
      return nullptr;
    } else if (frame.info().LastPresentTime.QuadPart != 0) {
      // A zero present time means only the pointer moved: no new pixels.
      ComPtr<ID3D11Texture2D> texture;
      hr = frame.resource()->QueryInterface(IID_PPV_ARGS(&texture));
      if (FAILED(hr)) {
        Fail(error, Failure("desktop frame is not a D3D11 texture", hr));
        return nullptr;
      }
      D3D11_TEXTURE2D_DESC desc;
      texture->GetDesc(&desc);
      if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
          static_cast<int>(desc.Width) != source_width_ ||
          static_cast<int>(desc.Height) != source_height_) {
        Fail(error, "the desktop frame changed format or size");
        return nullptr;
      }
      if (!Convert(texture.Get(), error)) return nullptr;
    }
    // `frame` releases here, before the (cheap) return path.
  }

  if (!have_frame_) return nullptr;  // Nothing presented yet; no error.
  if (texture_index) *texture_index = latest_slot_;
  return textures_[latest_slot_];
}

D3D11Capture::D3D11Capture(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
D3D11Capture::~D3D11Capture() = default;

std::unique_ptr<D3D11Capture> D3D11Capture::Create(int output_index,
                                                   int target_width,
                                                   int target_height,
                                                   int frame_rate,
                                                   std::string* error) {
  OutputSelector selector;
  selector.index = output_index;
  auto impl = std::make_unique<Impl>();
  if (!impl->Init(selector, target_width, target_height, frame_rate, error)) {
    return nullptr;
  }
  return std::unique_ptr<D3D11Capture>(new D3D11Capture(std::move(impl)));
}

std::unique_ptr<D3D11Capture> D3D11Capture::Create(
    const std::string& source_id, int target_width, int target_height,
    int frame_rate, std::string* error) {
  OutputSelector selector;
  selector.by_id = true;
  selector.id = source_id;
  auto impl = std::make_unique<Impl>();
  if (!impl->Init(selector, target_width, target_height, frame_rate, error)) {
    return nullptr;
  }
  return std::unique_ptr<D3D11Capture>(new D3D11Capture(std::move(impl)));
}

bool D3D11Capture::QueryOutput(const std::string& source_id, int* width,
                               int* height, std::string* error) {
  OutputSelector selector;
  selector.by_id = true;
  selector.id = source_id;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  if (!FindOutput(selector, &adapter, &output, error)) return false;
  if (!CheckOutputSupported(output.Get(), nullptr, error)) return false;
  DXGI_OUTPUT_DESC desc;
  HRESULT hr = output->GetDesc(&desc);
  if (FAILED(hr)) {
    SetError(error, Failure("IDXGIOutput::GetDesc", hr));
    return false;
  }
  const LONG w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
  const LONG h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
  if (w <= 0 || h <= 0) {
    SetError(error, "the display reports an empty size");
    return false;
  }
  if (width) *width = static_cast<int>(w);
  if (height) *height = static_cast<int>(h);
  return true;
}

void* D3D11Capture::device() const { return impl_->device_.Get(); }
const std::vector<void*>& D3D11Capture::textures() const {
  return impl_->textures_;
}
void* D3D11Capture::CaptureNext(std::string* error, size_t* texture_index) {
  return impl_->CaptureNext(error, texture_index);
}
bool D3D11Capture::failed() const { return impl_->failed_; }
const std::string& D3D11Capture::adapter_name() const {
  return impl_->adapter_name_;
}
int D3D11Capture::width() const { return impl_->source_width_; }
int D3D11Capture::height() const { return impl_->source_height_; }
int D3D11Capture::target_width() const { return impl_->target_width_; }
int D3D11Capture::target_height() const { return impl_->target_height_; }
int D3D11Capture::frame_rate() const { return impl_->frame_rate_; }

}  // namespace shared_encoder
}  // namespace discourse

#endif  // _WIN32
