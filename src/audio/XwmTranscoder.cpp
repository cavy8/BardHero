#include "audio/XwmTranscoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <opus/opusenc.h>

#include <array>
#include <functional>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace SH {
    namespace {
        using Microsoft::WRL::ComPtr;
        namespace fs = std::filesystem;

        std::string HrText(std::string_view operation, HRESULT hr) {
            std::ostringstream out;
            out << operation << " failed (0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(hr) << ')';
            return out.str();
        }

        std::string OpeText(std::string_view operation, int code) {
            return std::string(operation) + " failed (" +
                   (ope_strerror(code) ? ope_strerror(code) : "unknown") +
                   ')';
        }

        struct TempWave {
            fs::path path;
            ~TempWave() {
                if (path.empty()) return;
                std::error_code ec;
                fs::remove(path, ec);
            }
        };

        bool MakeWaveView(const fs::path& source, TempWave& temp,
                          std::string& error) {
            std::error_code ec;
            const auto size = fs::file_size(source, ec);
            if (ec || size < 12) {
                error = "source XWM is missing or truncated";
                return false;
            }
            std::array<char, 12> header{};
            {
                std::ifstream in(source, std::ios::binary);
                in.read(header.data(), static_cast<std::streamsize>(
                                           header.size()));
            }
            if (std::string_view(header.data(), 4) != "RIFF" ||
                std::string_view(header.data() + 8, 4) != "XWMA") {
                error = "source is not an xWMA RIFF file";
                return false;
            }

            wchar_t tempDir[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, tempDir)) {
                error = HrText("GetTempPathW", HRESULT_FROM_WIN32(GetLastError()));
                return false;
            }
            temp.path = fs::path(tempDir) /
                        ("bardhero-ba-" + std::to_string(GetCurrentProcessId()) +
                         "-" +
                         std::to_string(
                             std::hash<std::wstring>{}(source.wstring())) +
                         ".wav");
            {
                std::ifstream in(source, std::ios::binary);
                std::ofstream out(temp.path,
                                  std::ios::binary | std::ios::trunc);
                if (!in || !out) {
                    error = "could not create the temporary WMA view";
                    return false;
                }
                out << in.rdbuf();
                if (!out) {
                    error = "could not copy the temporary WMA view";
                    return false;
                }
            }
            {
                std::fstream wave(temp.path, std::ios::binary | std::ios::in |
                                                 std::ios::out);
                wave.seekp(8);
                wave.write("WAVE", 4);
                if (!wave) {
                    error = "could not normalize the temporary WMA header";
                    return false;
                }
            }
            return true;
        }

        bool UInt32(IMFAttributes* attrs, REFGUID key, UINT32& value) {
            return SUCCEEDED(attrs->GetUINT32(key, &value));
        }

        ComPtr<IMFMediaType> FindEncodedType(IMFMediaType* native,
                                             std::string& error) {
            UINT32 channels = 0, rate = 0, average = 0, align = 0;
            if (!UInt32(native, MF_MT_AUDIO_NUM_CHANNELS, channels) ||
                !UInt32(native, MF_MT_AUDIO_SAMPLES_PER_SECOND, rate) ||
                !UInt32(native, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, average) ||
                !UInt32(native, MF_MT_AUDIO_BLOCK_ALIGNMENT, align)) {
                error = "xWMA stream is missing required format metadata";
                return {};
            }

            ComPtr<IMFTransform> encoder;
            HRESULT hr = CoCreateInstance(
                CLSID_CWMAEncMediaObject, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&encoder));
            if (FAILED(hr)) {
                error = HrText("creating the Windows WMA format catalogue", hr);
                return {};
            }
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> candidate;
                hr = encoder->GetOutputAvailableType(0, i, &candidate);
                if (hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(hr)) {
                    error = HrText("enumerating WMA formats", hr);
                    return {};
                }
                GUID   subtype{};
                UINT32 c = 0, r = 0, a = 0, b = 0;
                candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
                UInt32(candidate.Get(), MF_MT_AUDIO_NUM_CHANNELS, c);
                UInt32(candidate.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, r);
                UInt32(candidate.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a);
                UInt32(candidate.Get(), MF_MT_AUDIO_BLOCK_ALIGNMENT, b);
                if (subtype.Data1 == WAVE_FORMAT_WMAUDIO2 && c == channels &&
                    r == rate && a == average && b == align) {
                    return candidate;
                }
            }
            error = "Windows has no WMA2 decoder profile matching this XWM";
            return {};
        }

        ComPtr<IMFMediaType> FindPcmType(IMFTransform* decoder,
                                         UINT32 channels, UINT32 rate,
                                         std::string& error) {
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> candidate;
                const HRESULT hr =
                    decoder->GetOutputAvailableType(0, i, &candidate);
                if (hr == MF_E_NO_MORE_TYPES) break;
                if (FAILED(hr)) {
                    error = HrText("enumerating decoded PCM formats", hr);
                    return {};
                }
                GUID   subtype{};
                UINT32 c = 0, r = 0, bits = 0;
                candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
                UInt32(candidate.Get(), MF_MT_AUDIO_NUM_CHANNELS, c);
                UInt32(candidate.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, r);
                UInt32(candidate.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
                if (subtype == MFAudioFormat_PCM && c == channels &&
                    r == rate && bits == 16) {
                    return candidate;
                }
            }
            error = "Windows WMA decoder offered no matching 16-bit PCM format";
            return {};
        }

        struct OpusOutput {
            std::ofstream file;
            bool          failed = false;

            static int Write(void* user, const unsigned char* data,
                             opus_int32 length) {
                auto& self = *static_cast<OpusOutput*>(user);
                self.file.write(reinterpret_cast<const char*>(data), length);
                self.failed = self.failed || !self.file;
                return self.failed ? 1 : 0;
            }
            static int Close(void* user) {
                auto& self = *static_cast<OpusOutput*>(user);
                self.file.close();
                self.failed = self.failed || self.file.fail();
                return self.failed ? 1 : 0;
            }
        };

        bool EncodeSample(IMFSample* sample, OggOpusEnc* encoder,
                          UINT32 channels, std::string& error) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(hr)) {
                error = HrText("coalescing decoded PCM", hr);
                return false;
            }
            BYTE* data = nullptr;
            DWORD length = 0;
            hr = buffer->Lock(&data, nullptr, &length);
            if (FAILED(hr)) {
                error = HrText("locking decoded PCM", hr);
                return false;
            }
            const auto bytesPerFrame = sizeof(opus_int16) * channels;
            const int  frames =
                static_cast<int>(length / static_cast<DWORD>(bytesPerFrame));
            const int code =
                frames > 0 ? ope_encoder_write(
                                 encoder,
                                 reinterpret_cast<const opus_int16*>(data),
                                 frames) :
                             OPE_OK;
            buffer->Unlock();
            if (length % bytesPerFrame != 0) {
                error = "decoded PCM ended on a partial frame";
                return false;
            }
            if (code != OPE_OK) {
                error = OpeText("encoding Opus audio", code);
                return false;
            }
            return true;
        }

        HRESULT PullDecoder(IMFTransform* decoder, OggOpusEnc* encoder,
                            UINT32 channels, std::string& error) {
            while (true) {
                MFT_OUTPUT_STREAM_INFO info{};
                HRESULT hr = decoder->GetOutputStreamInfo(0, &info);
                if (FAILED(hr)) return hr;
                ComPtr<IMFSample> sample;
                if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                    hr = MFCreateSample(&sample);
                    ComPtr<IMFMediaBuffer> buffer;
                    if (SUCCEEDED(hr)) {
                        hr = MFCreateMemoryBuffer(
                            info.cbSize ? info.cbSize : 65536, &buffer);
                    }
                    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
                    if (FAILED(hr)) return hr;
                }
                MFT_OUTPUT_DATA_BUFFER output{};
                output.dwStreamID = 0;
                output.pSample = sample.Get();
                DWORD status = 0;
                hr = decoder->ProcessOutput(0, 1, &output, &status);
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
                if (FAILED(hr)) return hr;
                if (!sample && output.pSample) sample.Attach(output.pSample);
                if (output.pEvents) output.pEvents->Release();
                if (!sample || !EncodeSample(sample.Get(), encoder, channels,
                                             error)) {
                    return error.empty() ? E_UNEXPECTED : E_FAIL;
                }
            }
        }
    }

    bool TranscodeXwmToOpus(const fs::path& source, const fs::path& output,
                            std::string& error) {
        error.clear();
        TempWave temp;
        if (!MakeWaveView(source, temp, error)) return false;

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool    ownsCom = SUCCEEDED(comHr);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            error = HrText("initializing COM", comHr);
            return false;
        }
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            if (ownsCom) CoUninitialize();
            error = HrText("starting Windows Media Foundation", hr);
            return false;
        }

        bool ok = false;
        do {
            ComPtr<IMFSourceReader> reader;
            hr = MFCreateSourceReaderFromURL(temp.path.c_str(), nullptr,
                                             &reader);
            if (FAILED(hr)) {
                error = HrText("opening the normalized XWM stream", hr);
                break;
            }
            ComPtr<IMFMediaType> native;
            hr = reader->GetNativeMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                &native);
            if (FAILED(hr)) {
                error = HrText("reading the XWM format", hr);
                break;
            }
            UINT32 channels = 0, rate = 0;
            if (!UInt32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, channels) ||
                !UInt32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, rate) ||
                channels < 1 || channels > 2) {
                error = "XWM must be mono or stereo and declare a sample rate";
                break;
            }
            auto encoded = FindEncodedType(native.Get(), error);
            if (!encoded) break;

            ComPtr<IMFTransform> decoder;
            hr = CoCreateInstance(CLSID_CWMADecMediaObject, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&decoder));
            if (FAILED(hr)) {
                error = HrText("creating the Windows WMA decoder", hr);
                break;
            }
            hr = decoder->SetInputType(0, encoded.Get(), 0);
            if (FAILED(hr)) {
                error = HrText("configuring the WMA decoder input", hr);
                break;
            }
            auto pcm = FindPcmType(decoder.Get(), channels, rate, error);
            if (!pcm) break;
            hr = decoder->SetOutputType(0, pcm.Get(), 0);
            if (FAILED(hr)) {
                error = HrText("configuring the WMA decoder output", hr);
                break;
            }

            OpusOutput opusOut;
            opusOut.file.open(output, std::ios::binary | std::ios::trunc);
            if (!opusOut.file) {
                error = "could not create the temporary Opus cache";
                break;
            }
            OggOpusComments* comments = ope_comments_create();
            if (!comments) {
                error = "could not allocate Opus comments";
                break;
            }
            ope_comments_add(comments, "ENCODER", "BardHero BA compatibility");
            const OpusEncCallbacks callbacks{ &OpusOutput::Write,
                                               &OpusOutput::Close };
            int opusError = OPE_OK;
            OggOpusEnc* encoder = ope_encoder_create_callbacks(
                &callbacks, &opusOut, comments, static_cast<opus_int32>(rate),
                static_cast<int>(channels), 0, &opusError);
            ope_comments_destroy(comments);
            if (!encoder) {
                error = OpeText("creating the Opus encoder", opusError);
                break;
            }
            const int bitrate = channels == 1 ? 64000 : 112000;
            opusError = ope_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
            if (opusError != OPE_OK) {
                error = OpeText("setting the Opus bitrate", opusError);
                ope_encoder_destroy(encoder);
                break;
            }

            decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            bool streamOk = true;
            while (streamOk) {
                DWORD stream = 0, flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                hr = reader->ReadSample(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                    &stream, &flags, &timestamp, &sample);
                if (FAILED(hr)) {
                    error = HrText("reading compressed XWM packets", hr);
                    streamOk = false;
                    break;
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                if (!sample) continue;
                hr = decoder->ProcessInput(0, sample.Get(), 0);
                if (FAILED(hr)) {
                    error = HrText("feeding the WMA decoder", hr);
                    streamOk = false;
                    break;
                }
                hr = PullDecoder(decoder.Get(), encoder, channels, error);
                if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    if (error.empty()) {
                        error = HrText("draining decoded WMA packets", hr);
                    }
                    streamOk = false;
                }
            }
            if (streamOk) {
                decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                hr = PullDecoder(decoder.Get(), encoder, channels, error);
                streamOk = hr == MF_E_TRANSFORM_NEED_MORE_INPUT;
                if (!streamOk && error.empty()) {
                    error = HrText("finishing the WMA decoder", hr);
                }
            }
            if (streamOk) {
                opusError = ope_encoder_drain(encoder);
                if (opusError != OPE_OK) {
                    error = OpeText("finishing the Opus cache", opusError);
                    streamOk = false;
                }
            }
            ope_encoder_destroy(encoder);
            if (opusOut.file.is_open()) opusOut.file.close();
            if (opusOut.failed && streamOk) {
                error = "writing the Opus cache failed";
                streamOk = false;
            }
            ok = streamOk;
        } while (false);

        MFShutdown();
        if (ownsCom) CoUninitialize();
        if (!ok) {
            std::error_code ec;
            fs::remove(output, ec);
        }
        return ok;
    }
}
