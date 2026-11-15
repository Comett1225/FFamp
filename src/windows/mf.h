#pragma once
#include <windows.h>
#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

namespace absent {

using Microsoft::WRL::ComPtr;

struct MediaFoundation {
    HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* enumerate)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*) = nullptr;
    HRESULT(WINAPI* mediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* sample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* memoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* alignedBuffer)(DWORD, DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* deviceManager)(UINT*, IMFDXGIDeviceManager**) = nullptr;
};

const MediaFoundation* mediaFoundation();
bool comReady();
ComPtr<IMFTransform> findTransform(const GUID& category, const GUID& major, const GUID& input, const GUID& output, bool hardware, std::string* name);
bool sendSample(IMFTransform* transform, const uint8_t* data, size_t size, int64_t time, int64_t duration, bool key, HRESULT* result = nullptr, bool fresh = false);
bool annexB(const uint8_t* data, size_t size, int lengthSize, std::vector<uint8_t>& out);
std::vector<uint8_t> parameterSets(uint32_t configType, const std::vector<uint8_t>& config);
bool d3dEnabled();

}
