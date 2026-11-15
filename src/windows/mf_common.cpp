#include "mf.h"
#include <cstdlib>
#include <mutex>

namespace absent {

const MediaFoundation* mediaFoundation() {
    static MediaFoundation found;
    static bool ready = [] {
        if (std::getenv("FFAMP_NO_MEDIA_FOUNDATION")) return false;
        HMODULE library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return false;
        auto take = [&](auto& slot, const char* name) {
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(reinterpret_cast<void*>(GetProcAddress(library, name)));
            return slot != nullptr;
        };
        bool all = take(found.startup, "MFStartup") && take(found.enumerate, "MFTEnumEx") && take(found.mediaType, "MFCreateMediaType") &&
                   take(found.sample, "MFCreateSample") && take(found.memoryBuffer, "MFCreateMemoryBuffer") && take(found.alignedBuffer, "MFCreateAlignedMemoryBuffer");
        take(found.deviceManager, "MFCreateDXGIDeviceManager");
        return all && SUCCEEDED(found.startup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }();
    return ready ? &found : nullptr;
}

bool comReady() {
    struct Scope {
        HRESULT result;
        Scope() : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~Scope() {
            if (SUCCEEDED(result)) CoUninitialize();
        }
    };
    thread_local Scope scope;
    return SUCCEEDED(scope.result) || scope.result == RPC_E_CHANGED_MODE;
}

bool d3dEnabled() { return !std::getenv("FFAMP_NO_GPU_DECODE"); }

ComPtr<IMFTransform> findTransform(const GUID& category, const GUID& major, const GUID& input, const GUID& output, bool hardware, std::string* name) {
    const MediaFoundation* mf = mediaFoundation();
    if (!mf || !comReady()) return nullptr;
    MFT_REGISTER_TYPE_INFO in{major, input}, out{major, output};
    const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(mf->enumerate(category, flags, &in, output == GUID_NULL ? nullptr : &out, &list, &count))) return nullptr;
    ComPtr<IMFTransform> chosen;
    for (UINT32 i = 0; i < count; ++i) {
        if (!chosen && SUCCEEDED(list[i]->ActivateObject(IID_PPV_ARGS(&chosen)))) {
            if (name) {
                WCHAR* text = nullptr;
                UINT32 length = 0;
                if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &text, &length))) {
                    name->clear();
                    for (UINT32 k = 0; k < length; ++k) name->push_back(text[k] < 128 ? char(text[k]) : '?');
                    CoTaskMemFree(text);
                }
            }
        }
        list[i]->Release();
    }
    CoTaskMemFree(list);
    return chosen;
}

bool sendSample(IMFTransform* transform, const uint8_t* data, size_t size, int64_t time, int64_t duration, bool key, HRESULT* result, bool fresh) {
    const MediaFoundation* mf = mediaFoundation();
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* bytes = nullptr;
    if (FAILED(mf->memoryBuffer(DWORD(size), &buffer)) || FAILED(buffer->Lock(&bytes, nullptr, nullptr))) return false;
    memcpy(bytes, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(size));
    if (FAILED(mf->sample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return false;
    sample->SetSampleTime(time);
    if (duration > 0) sample->SetSampleDuration(duration);
    if (key || fresh) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    if (fresh) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    const HRESULT status = transform->ProcessInput(0, sample.Get(), 0);
    if (result) *result = status;
    return SUCCEEDED(status);
}

bool annexB(const uint8_t* data, size_t size, int lengthSize, std::vector<uint8_t>& out) {
    for (size_t at = 0; at + size_t(lengthSize) <= size;) {
        size_t length = 0;
        for (int k = 0; k < lengthSize; ++k) length = (length << 8) | data[at + size_t(k)];
        at += size_t(lengthSize);
        if (length > size - at) return false;
        static const uint8_t code[4] = {0, 0, 0, 1};
        out.insert(out.end(), code, code + 4);
        out.insert(out.end(), data + at, data + at + length);
        at += length;
    }
    return true;
}

std::vector<uint8_t> parameterSets(uint32_t configType, const std::vector<uint8_t>& c) {
    std::vector<uint8_t> out;
    static const uint8_t code[4] = {0, 0, 0, 1};
    auto list = [&](size_t& at, size_t count) {
        for (size_t i = 0; i < count && at + 2 <= c.size(); ++i) {
            const size_t length = (size_t(c[at]) << 8) | c[at + 1];
            at += 2;
            if (length > c.size() - at) return;
            out.insert(out.end(), code, code + 4);
            out.insert(out.end(), c.begin() + long(at), c.begin() + long(at + length));
            at += length;
        }
    };
    if (configType == 0x68766343) {
        if (c.size() < 23) return out;
        size_t at = 23;
        for (int n = 0; n < c[22] && at + 3 <= c.size(); ++n) {
            const size_t count = (size_t(c[at + 1]) << 8) | c[at + 2];
            at += 3;
            list(at, count);
        }
    } else {
        if (c.size() < 7) return out;
        size_t at = 6;
        list(at, c[5] & 31);
        if (at < c.size()) {
            const size_t count = c[at++];
            list(at, count);
        }
    }
    return out;
}

}
