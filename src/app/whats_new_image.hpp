#pragma once
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <string>

namespace taxi_camera::standalone {
// A What's new picture (PNG or JPEG bytes) decoded with WIC and scaled to fit
// max_width x max_height pixels, never enlarged, as a top-down 32-bit DIB for
// GDI. nullptr when the bytes are not an image WIC can read.
inline HBITMAP decode_whats_new_image(const std::string& bytes, int max_width, int max_height, int& width, int& height) {
  width = height = 0;
  if (bytes.empty() || bytes.size() > 0x7fffffff || max_width <= 0 || max_height <= 0)
    return nullptr;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* factory{};
  IWICStream* stream{};
  IWICBitmapDecoder* decoder{};
  IWICBitmapFrameDecode* frame{};
  IWICBitmapScaler* scaler{};
  IWICFormatConverter* converter{};
  HBITMAP bitmap{};
  UINT source_width{}, source_height{};
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
      SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
      SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&source_width, &source_height)) && source_width &&
      source_height) {
    const double fit = std::min({1.0, double(max_width) / source_width, double(max_height) / source_height});
    width = std::max(1, static_cast<int>(source_width * fit + 0.5));
    height = std::max(1, static_cast<int>(source_height * fit + 0.5));
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* pixels{};
    if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, static_cast<UINT>(width), static_cast<UINT>(height), WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
        (bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0)) != nullptr &&
        FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(width) * 4, static_cast<UINT>(width) * 4 * static_cast<UINT>(height),
                                     static_cast<BYTE*>(pixels)))) {
      DeleteObject(bitmap);
      bitmap = nullptr;
    }
  }
  for (IUnknown* object : std::initializer_list<IUnknown*>{converter, scaler, frame, decoder, stream, factory})
    if (object)
      object->Release();
  if (SUCCEEDED(com))
    CoUninitialize();
  if (!bitmap)
    width = height = 0;
  return bitmap;
}
}  // namespace taxi_camera::standalone
