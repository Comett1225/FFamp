#include "../nal.h"
#include "../soft_video.h"
#include "mf.h"
#include <d3d11_4.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace absent {

namespace {

void setValue(IMFTransform* transform, const GUID& key, UINT32 value) {
    ComPtr<ICodecAPI> api;
    if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&api)))) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;

    bool create() {
        const MediaFoundation* mf = mediaFoundation();
        if (!mf || !mf->deviceManager) return false;
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, nullptr))) return false;
        ComPtr<ID3D11Multithread> guard;
        if (SUCCEEDED(device.As(&guard))) guard->SetMultithreadProtected(TRUE);
        UINT token = 0;
        return SUCCEEDED(mf->deviceManager(&token, &manager)) && SUCCEEDED(manager->ResetDevice(device.Get(), token));
    }

    bool open() {
        static std::mutex lock;
        static Gpu& shared = *new Gpu;
        std::lock_guard<std::mutex> hold(lock);
        if (shared.device && shared.device->GetDeviceRemovedReason() != S_OK) shared = Gpu();
        if (!shared.device && !shared.create()) {
            shared = Gpu();
            return false;
        }
        *this = shared;
        return true;
    }
};

DWORD_PTR WINAPI keepAffinity(HANDLE, DWORD_PTR) {
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    return process ? process : 1;
}

void skipAffinityCalls(const wchar_t* dll) {
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(dll));
    if (!base) return;
    const void* targets[] = {reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadAffinityMask")),
                             reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetThreadAffinityMask"))};
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return;
    for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); entry->Name; ++entry) {
        for (auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + entry->FirstThunk); slot->u1.Function; ++slot) {
            const void* bound = reinterpret_cast<const void*>(slot->u1.Function);
            if (!bound || (bound != targets[0] && bound != targets[1])) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &old)) continue;
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(&keepAffinity);
            VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), old, &old);
        }
    }
}

struct MfVideoDecoder : SoftDecoder {
    ComPtr<IMFTransform> transform;
    Gpu gpu;
    bool hardware = false;
    bool hevc = false;
    int lengthSize = 4;
    std::vector<uint8_t> headers;
    std::vector<uint8_t> bytes;
    bool primed = false;
    int width = 0;
    int height = 0;
    UINT32 frameWidth = 0;
    UINT32 frameHeight = 0;
    LONG pitch = 0;
    bool deep = false;
    bool providesSamples = false;
    DWORD outputSize = 0;
    Color color;
    const char* failure = "";
    struct Pending {
        int slot = -1;
        LONGLONG time = 0;
    };
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> ring[3];
    D3D11_TEXTURE2D_DESC ringShape{};
    std::deque<Pending> pending;
    unsigned copies = 0;

    const char* name() const override { return hardware ? "mediafoundation-gpu" : "mediafoundation"; }
    bool drainKeepsState() const override { return false; }
    bool weavesFields() const override { return true; }

    bool pickOutput() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(0, i, &type))) return false;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_P010) {
                if (FAILED(transform->SetOutputType(0, type.Get(), 0))) continue;
                deep = subtype == MFVideoFormat_P010;
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &frameWidth, &frameHeight);
                UINT32 stride = 0;
                pitch = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(int32_t(stride)) : LONG(frameWidth) * (deep ? 2 : 1);
                MFT_OUTPUT_STREAM_INFO info{};
                transform->GetOutputStreamInfo(0, &info);
                providesSamples = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
                outputSize = info.cbSize;
                return true;
            }
        }
    }

    bool open(const GUID& subtype, bool useGpu) {
        static const GUID kH264 = {0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
        static const GUID kMpeg2 = {0x2D709E52, 0x123F, 0x49B5, {0x9C, 0xBC, 0x9A, 0xF5, 0xCD, 0xE2, 0x8F, 0xB9}};
        const bool microsoft = subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_MPEG2;
        if (microsoft && comReady()) CoCreateInstance(subtype == MFVideoFormat_H264 ? kH264 : kMpeg2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
        std::string label;
        if (!transform) transform = findTransform(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, subtype, GUID_NULL, false, &label);
        failure = "no system decoder for this codec";
        if (!transform) return false;
        if (microsoft) skipAffinityCalls(L"msmpeg2vdec.dll");
        if (useGpu && gpu.open()) {
            ComPtr<IMFAttributes> attributes;
            UINT32 aware = 0;
            if (SUCCEEDED(transform->GetAttributes(&attributes)) && SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware &&
                SUCCEEDED(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(gpu.manager.Get())))) {
                hardware = true;
            }
        }
        setValue(transform.Get(), CODECAPI_AVLowLatencyMode, 1);
        ComPtr<IMFMediaType> input;
        if (FAILED(mediaFoundation()->mediaType(&input))) return false;
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, subtype);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, UINT32(width), UINT32(height));
        failure = "the system decoder refused the stream description";
        if (FAILED(transform->SetInputType(0, input.Get(), 0))) return false;
        failure = "the system decoder gives no NV12 or P010 pictures";
        if (!pickOutput()) return false;
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void emit(const Pending& item, const PictureSink& sink) {
        Picture p;
        p.stamp = item.time;
        p.color = color;
        p.width = std::min<int>(width, int(frameWidth));
        p.height = std::min<int>(height, int(frameHeight));
        p.chromaWidth = (p.width + 1) / 2;
        p.chromaHeight = (p.height + 1) / 2;
        p.interleaved = true;
        p.bits = deep ? 10 : 8;
        p.shift = deep ? 6 : 0;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (item.slot < 0 || FAILED(context->Map(ring[item.slot].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            sink(p);
            return;
        }
        const BYTE* scan = static_cast<const BYTE*>(mapped.pData);
        p.plane[0] = scan;
        p.plane[1] = scan + size_t(mapped.RowPitch) * ringShape.Height;
        p.stride[0] = p.stride[1] = mapped.RowPitch;
        sink(p);
        context->Unmap(ring[item.slot].Get(), 0);
    }

    void flush(const PictureSink& sink, size_t keep = 0) {
        while (pending.size() > keep) {
            const Pending item = pending.front();
            pending.pop_front();
            emit(item, sink);
        }
    }

    bool copyOut(IMFMediaBuffer* buffer, LONGLONG time, const PictureSink& sink) {
        ComPtr<IMFDXGIBuffer> surface;
        ComPtr<ID3D11Texture2D> texture;
        UINT index = 0;
        if (FAILED(buffer->QueryInterface(IID_PPV_ARGS(&surface))) || FAILED(surface->GetResource(IID_PPV_ARGS(&texture))) || FAILED(surface->GetSubresourceIndex(&index))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!context || desc.Width != ringShape.Width || desc.Height != ringShape.Height || desc.Format != ringShape.Format) {
            flush(sink);
            if (!context) gpu.device->GetImmediateContext(&context);
            D3D11_TEXTURE2D_DESC staging = desc;
            staging.MipLevels = 1;
            staging.ArraySize = 1;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            ringShape = D3D11_TEXTURE2D_DESC{};
            for (auto& slot : ring) {
                slot.Reset();
                if (FAILED(gpu.device->CreateTexture2D(&staging, nullptr, &slot))) return false;
            }
            ringShape = desc;
        }
        flush(sink, std::size(ring) - 1);
        const int slot = int(copies++ % std::size(ring));
        context->CopySubresourceRegion(ring[slot].Get(), 0, 0, 0, 0, texture.Get(), index, nullptr);
        pending.push_back({slot, time});
        return true;
    }

    void hand(IMFSample* sample, const PictureSink& sink) {
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        if (wanted && !wanted(time)) {
            pending.push_back({-1, time});
            flush(sink, std::size(ring) - 1);
            return;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        if (hardware && copyOut(buffer.Get(), time, sink)) return;
        flush(sink);
        ComPtr<IMF2DBuffer> flat;
        BYTE* scan = nullptr;
        LONG stride = 0;
        BYTE* start = nullptr;
        DWORD length = 0;
        const bool twoD = SUCCEEDED(buffer.As(&flat)) && SUCCEEDED(flat->Lock2D(&scan, &stride));
        if (!twoD) {
            if (FAILED(buffer->Lock(&start, nullptr, &length))) return;
            scan = start;
            stride = pitch;
        }
        UINT32 rows = std::max<UINT32>(frameHeight, UINT32(height));
        if (scan && stride > 0) {
            Picture p;
            p.width = std::min<int>(width, int(frameWidth));
            p.height = std::min<int>(height, int(frameHeight));
            p.chromaWidth = (p.width + 1) / 2;
            p.chromaHeight = (p.height + 1) / 2;
            p.interleaved = true;
            p.bits = deep ? 10 : 8;
            p.shift = deep ? 6 : 0;
            p.plane[0] = scan;
            p.plane[1] = scan + size_t(stride) * rows;
            p.stride[0] = p.stride[1] = size_t(stride);
            p.stamp = time;
            p.color = color;
            if (twoD || size_t(stride) * rows * 3 / 2 <= length) sink(p);
        }
        if (twoD) flat->Unlock2D();
        else buffer->Unlock();
    }

    bool pull(const PictureSink& sink) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> own;
            if (!providesSamples) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(mediaFoundation()->alignedBuffer(outputSize, MF_64_BYTE_ALIGNMENT, &buffer)) || FAILED(mediaFoundation()->sample(&own))) return false;
                own->AddBuffer(buffer.Get());
                output.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (output.pEvents) output.pEvents->Release();
                flush(sink);
                if (!pickOutput()) return false;
                continue;
            }
            if (FAILED(result)) {
                if (output.pEvents) output.pEvents->Release();
                return false;
            }
            if (output.pSample) hand(output.pSample, sink);
            if (providesSamples && output.pSample) output.pSample->Release();
            if (output.pEvents) output.pEvents->Release();
        }
    }

    bool decode(const uint8_t* data, size_t size, int64_t stamp, const PictureSink& sink) override {
        bytes.clear();
        const bool fresh = !primed;
        if (fresh) bytes = headers;
        if (!lengthSize) bytes.insert(bytes.end(), data, data + size);
        else if (!annexB(data, size, lengthSize, bytes)) return false;
        if (bytes.empty()) return false;
        primed = true;
        for (int attempt = 0; attempt < 2; ++attempt) {
            HRESULT result = S_OK;
            if (sendSample(transform.Get(), bytes.data(), bytes.size(), stamp, 0, false, &result, fresh)) return pull(sink);
            if (result != MF_E_NOTACCEPTING || !pull(sink)) return false;
        }
        return false;
    }

    bool drain(const PictureSink& sink) override {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        const bool ok = pull(sink);
        flush(sink);
        return ok;
    }

    void reset() override {
        pending.clear();
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        primed = false;
    }
};

struct MfVideoEncoder : SoftEncoder {
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> events;
    EncodeSpec spec;
    bool hardware = false;
    bool providesSamples = false;
    DWORD outputSize = 0;
    std::vector<uint8_t> configBytes;
    std::string label;

    const char* name() const override { return hardware ? "mediafoundation-gpu" : "mediafoundation"; }
    uint32_t configType() const override { return spec.hevc ? fourcc("hvcC") : fourcc("avcC"); }
    std::vector<uint8_t> config() const override { return configBytes; }

    int64_t toUnits(int64_t ticks) const { return int64_t((long double)ticks * 10000000 / spec.timescale + (ticks >= 0 ? 0.5L : -0.5L)); }
    int64_t fromUnits(int64_t units) const { return int64_t((long double)units * spec.timescale / 10000000 + (units >= 0 ? 0.5L : -0.5L)); }

    void settings() {
        setValue(transform.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
        setValue(transform.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(spec.bitrate));
        setValue(transform.Get(), CODECAPI_AVEncMPVGOPSize, UINT32(std::max(1, spec.keyInterval)));
        setValue(transform.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setValue(transform.Get(), CODECAPI_AVLowLatencyMode, 0);
    }

    bool open(bool useHardware) {
        const GUID output = spec.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        transform = findTransform(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_NV12, output, useHardware, &label);
        if (!transform) return false;
        hardware = useHardware;
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) {
            UINT32 async = 0;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                if (FAILED(transform.As(&events))) return false;
            }
        }
        settings();
        const MediaFoundation* mf = mediaFoundation();
        const UINT32 rate = UINT32(std::lround((spec.fps > 0 ? spec.fps : 30) * 1000));
        ComPtr<IMFMediaType> out, in;
        if (FAILED(mf->mediaType(&out)) || FAILED(mf->mediaType(&in))) return false;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, output);
        out->SetUINT32(MF_MT_AVG_BITRATE, UINT32(spec.bitrate));
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, UINT32(spec.width), UINT32(spec.height));
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, rate, 1000);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, spec.hevc ? UINT32(eAVEncH265VProfile_Main_420_8) : UINT32(eAVEncH264VProfile_High));
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        if (FAILED(transform->SetOutputType(0, out.Get(), 0))) return false;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, UINT32(spec.width), UINT32(spec.height));
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, rate, 1000);
        MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(transform->SetInputType(0, in.Get(), 0))) return false;
        settings();
        MFT_OUTPUT_STREAM_INFO info{};
        transform->GetOutputStreamInfo(0, &info);
        providesSamples = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        outputSize = std::max<DWORD>(info.cbSize, DWORD(spec.width) * DWORD(spec.height) * 3 / 2 + 65536);
        return SUCCEEDED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) && SUCCEEDED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    void hand(IMFSample* sample, const PacketSink& sink) {
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) return;
        Packet p;
        NalList vps, sps, pps;
        annexNals(data, length, [&](const uint8_t* unit, size_t size) {
            if (!size) return;
            const int type = spec.hevc ? (unit[0] >> 1) & 63 : unit[0] & 31;
            if ((spec.hevc && type == 35) || (!spec.hevc && type == 9)) return;
            if (spec.hevc ? type == 32 : false) vps.emplace_back(unit, unit + size);
            if (spec.hevc ? type == 33 : type == 7) sps.emplace_back(unit, unit + size);
            if (spec.hevc ? type == 34 : type == 8) pps.emplace_back(unit, unit + size);
            const uint8_t prefix[4] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
            p.data.insert(p.data.end(), prefix, prefix + 4);
            p.data.insert(p.data.end(), unit, unit + size);
        });
        buffer->Unlock();
        if (configBytes.empty() && !sps.empty() && !pps.empty()) configBytes = spec.hevc ? buildHevcConfig(vps, sps, pps) : buildAvcConfig(sps, pps);
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        p.pts = p.dts = fromUnits(time);
        UINT64 decodeTime = 0;
        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decodeTime))) p.dts = std::min(p.pts, fromUnits(int64_t(decodeTime)));
        UINT32 clean = 0;
        p.sync = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
        if (!p.data.empty()) sink(std::move(p));
    }

    bool takeOne(const PacketSink& sink, bool& more) {
        MFT_OUTPUT_DATA_BUFFER output{};
        ComPtr<IMFSample> own;
        if (!providesSamples) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(mediaFoundation()->memoryBuffer(outputSize, &buffer)) || FAILED(mediaFoundation()->sample(&own))) return false;
            own->AddBuffer(buffer.Get());
            output.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
        more = false;
        if (output.pEvents) output.pEvents->Release();
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(transform->GetOutputAvailableType(0, 0, &type))) transform->SetOutputType(0, type.Get(), 0);
            more = true;
            return true;
        }
        if (FAILED(result)) return false;
        if (output.pSample) hand(output.pSample, sink);
        if (providesSamples && output.pSample) output.pSample->Release();
        more = true;
        return true;
    }

    bool pullAll(const PacketSink& sink) {
        for (bool more = true; more;) if (!takeOne(sink, more)) return false;
        return true;
    }

    bool waitEvent(MediaEventType& kind) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(events->GetEvent(0, &event))) return false;
        return SUCCEEDED(event->GetType(&kind));
    }

    bool frameSample(const Picture& p, int64_t pts, int64_t duration, ComPtr<IMFSample>& sample) {
        const MediaFoundation* mf = mediaFoundation();
        const size_t w = size_t(spec.width), h = size_t(spec.height), cw = (w + 1) / 2, ch = (h + 1) / 2;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* to = nullptr;
        if (FAILED(mf->alignedBuffer(DWORD(w * h + 2 * cw * ch), MF_64_BYTE_ALIGNMENT, &buffer)) || FAILED(buffer->Lock(&to, nullptr, nullptr))) return false;
        for (size_t y = 0; y < h; ++y) memcpy(to + y * w, p.plane[0] + y * p.stride[0], w);
        uint8_t* uv = to + w * h;
        for (size_t y = 0; y < ch; ++y) {
            const uint8_t* u = p.plane[1] + y * p.stride[1];
            const uint8_t* v = p.plane[2] + y * p.stride[2];
            uint8_t* line = uv + y * w;
            for (size_t x = 0; x < cw; ++x) {
                line[2 * x] = u[x];
                line[2 * x + 1] = v[x];
            }
        }
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(w * h + 2 * cw * ch));
        if (FAILED(mf->sample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return false;
        sample->SetSampleTime(toUnits(pts));
        sample->SetSampleDuration(std::max<int64_t>(1, toUnits(duration)));
        return true;
    }

    bool encode(const Picture& p, int64_t pts, int64_t duration, const PacketSink& sink) override {
        ComPtr<IMFSample> sample;
        if (!frameSample(p, pts, duration, sample)) return false;
        if (!events) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                const HRESULT result = transform->ProcessInput(0, sample.Get(), 0);
                if (SUCCEEDED(result)) return pullAll(sink);
                if (result != MF_E_NOTACCEPTING || !pullAll(sink)) return false;
            }
            return false;
        }
        for (;;) {
            MediaEventType kind = MEUnknown;
            if (!waitEvent(kind)) return false;
            if (kind == METransformNeedInput) return SUCCEEDED(transform->ProcessInput(0, sample.Get(), 0));
            if (kind == METransformHaveOutput) {
                bool more = false;
                if (!takeOne(sink, more)) return false;
            }
        }
    }

    bool finish(const PacketSink& sink) override {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (FAILED(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) return false;
        if (!events) return pullAll(sink);
        for (;;) {
            MediaEventType kind = MEUnknown;
            if (!waitEvent(kind)) return false;
            if (kind == METransformDrainComplete) return true;
            if (kind == METransformHaveOutput) {
                bool more = false;
                if (!takeOne(sink, more)) return false;
            }
        }
    }
};

}

std::unique_ptr<SoftDecoder> openPlatformVideo(const Track& track, const std::vector<uint8_t>& config, const StreamShape& shape, std::string* error) {
    const bool hevc = track.configType == fourcc("hvcC");
    const bool mpeg = track.codec == fourcc("mp1v") || track.codec == fourcc("mp2v");
    if (!hevc && !mpeg && track.configType != fourcc("avcC")) return nullptr;
    if (!mediaFoundation()) {
        if (error) *error = "Media Foundation is not available on this system";
        return nullptr;
    }
    const VideoInfo info = mpeg ? VideoInfo() : videoInfo(track.configType, config);
    std::string why;
    if (track.width < 64 || track.height < 64) return nullptr;
    if (mpeg && (shape.chromaX != 1 || shape.chromaY != 1)) {
        if (error) *error = "Media Foundation decodes only 4:2:0 MPEG-1 and MPEG-2 video";
        return nullptr;
    }
    if (hevc) {
        const bool main = (info.profile >= 1 && info.profile <= 3) || (!info.profile && (info.compatibilityFlags & 0x70000000u));
        const int64_t largest = 35651584, side = 16888;
        if (!info.valid || !main || info.chroma != 1 || (info.bitDepth != 8 && info.bitDepth != 10) || info.bitDepthChroma != info.bitDepth ||
            int64_t(track.width) * track.height > largest || track.width > side || track.height > side) {
            if (error) *error = "Media Foundation decodes only HEVC Main and Main 10 at 4:2:0 within level 6.2";
            return nullptr;
        }
    }
    for (bool useGpu : {d3dEnabled(), false}) {
        auto decoder = std::make_unique<MfVideoDecoder>();
        decoder->hevc = hevc;
        decoder->width = track.width;
        decoder->height = track.height;
        decoder->lengthSize = mpeg ? 0 : hevc ? (config.size() > 21 ? (config[21] & 3) + 1 : 4) : (config.size() > 4 ? (config[4] & 3) + 1 : 4);
        if (!mpeg) decoder->headers = parameterSets(track.configType, config);
        if (info.colorKnown || info.rangeKnown) {
            decoder->color.known = info.colorKnown;
            decoder->color.primaries = info.primaries;
            decoder->color.transfer = info.transfer;
            decoder->color.matrix = info.matrix;
            decoder->color.full = info.full;
        }
        if (decoder->open(mpeg ? MFVideoFormat_MPEG2 : hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, useGpu)) return decoder;
        why = decoder->failure;
        if (!useGpu) break;
    }
    if (error) *error = "Media Foundation: " + why;
    return nullptr;
}

std::unique_ptr<SoftEncoder> openPlatformEncoder(const EncodeSpec& spec, std::string* error) {
    if (!mediaFoundation() || std::getenv("FFAMP_NO_SYSTEM_ENCODER")) return nullptr;
    if (int64_t(spec.width) * spec.height < 320 * 240) return nullptr;
    for (bool hardware : {true, false}) {
        auto encoder = std::make_unique<MfVideoEncoder>();
        encoder->spec = spec;
        if (encoder->open(hardware)) return encoder;
    }
    if (error) *error = spec.hevc ? "the system has no HEVC encoder" : "the system H.264 encoder refused these settings";
    return nullptr;
}

}
