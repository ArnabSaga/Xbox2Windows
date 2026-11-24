// Native compute Dispatch and GPU readback through the startup wrapper (#1412, Stage 4D / N3).
//
//   startup_compute_native_dispatch <warp|hardware> <cache-dir> [debug]
//
// Two approved revision-2 fixtures are executed through the wrapper's real compute PSO, root
// binding, list Dispatch, queue, fence and a committed READBACK buffer. Every output DWORD is
// compared against an independently generated CPU reference. Exit 0 pass, 1 failure, 77 skip.
#include "startup_stubs/d3d12_compute_native.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using x2w_native::Acceptance;
using x2w_native::Record;
using x2w_native::State;

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*,
                                                                     REFIID, void**);
extern "C" __declspec(dllimport) long x2w_d3d12_compute_dispatches();
extern "C" __declspec(dllimport) long x2w_d3d12_compute_skipped_dispatches();
extern "C" __declspec(dllimport) long x2w_d3d12_pipeline_real(void* pipeline);
extern "C" __declspec(dllimport) HRESULT x2w_native_fixture_create(void* device,
                                                                  const wchar_t* cache_dir,
                                                                  const char* key_text,
                                                                  void** root, void** pso,
                                                                  int* state, char* detail,
                                                                  std::size_t detail_size);
extern "C" __declspec(dllimport) void x2w_native_fixture_release(void* pipeline);
extern "C" __declspec(dllimport) void x2w_native_fixture_release_root(void* root);
extern "C" __declspec(dllimport) int x2w_native_fixture_info(void* pipeline, int* path, int* cls,
                                                             unsigned long long* key);
extern "C" __declspec(dllimport) int x2w_native_fixture_root(
    void* root, unsigned int* types, unsigned int* registers, unsigned int* spaces,
    unsigned int* counts, unsigned int capacity, unsigned int* used);
extern "C" __declspec(dllimport) int x2w_native_fixture_extent_ok(unsigned long long offset,
                                                                 unsigned long long bytes,
                                                                 unsigned long long width);
extern "C" __declspec(dllimport) int x2w_native_fixture_render_mode();
extern "C" __declspec(dllimport) int x2w_native_fixture_list_reason(void* list, char* text,
                                                                   std::size_t size);
extern "C" __declspec(dllimport) HRESULT x2w_native_fixture_readback(
    void* list, void* source, unsigned long long source_offset, void* readback,
    unsigned long long readback_offset, unsigned long long bytes, unsigned int before_state);

namespace {

using Word = std::uint64_t;
constexpr std::uint32_t sentinel_base = 0x7F000000u;
constexpr std::size_t output_words = 320;
constexpr std::size_t source_words = 256;

Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0,
            Word e = 0, Word f = 0, Word g = 0, Word h = 0)
{
    using Slot = Word(WINAPI*)(void*, Word, Word, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f,
                                                                          g, h);
}

template <class T>
Word arg(T* value)
{
    return reinterpret_cast<Word>(value);
}

// ---------------------------------------------------------------- reporting

struct Report {
    std::string mode;
    std::vector<std::pair<std::string, bool>> checks;
    std::vector<std::string> notes;
    int failures = 0;
    void check(const std::string& name, bool ok, const std::string& detail = "")
    {
        checks.emplace_back(name, ok);
        if (!ok) ++failures;
        std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                    detail.empty() ? "" : ": ", detail.c_str());
        std::fflush(stdout);
    }
    void note(const std::string& text)
    {
        notes.push_back(text);
        std::printf("NOTE %s\n", text.c_str());
        std::fflush(stdout);
    }
};

// ---------------------------------------------------------------- files

std::wstring wide(const char* text)
{
    if (!text) return std::wstring();
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring out(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    return out;
}

std::string narrow(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(static_cast<char>(c));
    return out;
}

std::wstring evidence_dir()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"X2W_NATIVE_EVIDENCE", buffer, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(buffer) : std::wstring();
}

// One run writes only into its own mode directory, so WARP/hardware and normal/debug raw readbacks
// cannot overwrite each other.
std::wstring run_directory;

bool write_bytes(const std::wstring& path, const void* data, std::size_t size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = size == 0 ||
                    (WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
                     written == size);
    CloseHandle(file);
    return ok;
}

bool write_words(const std::string& base, const char* suffix,
                 const std::vector<std::uint32_t>& words)
{
    const std::wstring directory = run_directory.empty() ? evidence_dir() : run_directory;
    if (directory.empty()) return true;
    CreateDirectoryW(directory.c_str(), nullptr);
    CreateDirectoryW((directory + L"\\readback").c_str(), nullptr);
    const std::wstring name(base.begin(), base.end());
    const std::wstring file(suffix, suffix + std::strlen(suffix));
    return write_bytes(directory + L"\\readback\\" + name + L"." + file, words.data(),
                       words.size() * 4);
}

bool read_text_bounded(const std::wstring& path, std::string& out, std::size_t limit)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    const unsigned long long file_bytes = static_cast<unsigned long long>(size.QuadPart);
    const std::size_t bytes = static_cast<std::size_t>(
        file_bytes < static_cast<unsigned long long>(limit) ? file_bytes : limit);
    out.assign(bytes, '\0');
    DWORD read = 0;
    const bool ok = bytes == 0 || (ReadFile(file, out.data(), static_cast<DWORD>(bytes), &read,
                                            nullptr) &&
                                   read == bytes);
    CloseHandle(file);
    return ok;
}

// ---------------------------------------------------------------- reference

std::uint32_t source_word(std::size_t i)
{
    return ((0x1234u + 0x101u * static_cast<std::uint32_t>(i)) & 0xFFFFu) |
           ((1u + static_cast<std::uint32_t>(i % 7)) << 16);
}

std::vector<std::uint32_t> make_source()
{
    std::vector<std::uint32_t> source(source_words);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = source_word(i);
    return source;
}

std::vector<std::uint32_t> make_sentinel(std::size_t words = output_words)
{
    std::vector<std::uint32_t> output(words);
    for (std::size_t i = 0; i < words; ++i) output[i] = sentinel_base + static_cast<std::uint32_t>(i);
    return output;
}

std::uint32_t compute_word(const std::vector<std::uint32_t>& source, std::uint32_t index,
                           std::uint32_t cap)
{
    const std::uint64_t low = source[index] & 0xFFFFu;
    const std::uint64_t high = (source[index] >> 16) & 0xFFFFu;
    const std::uint32_t value =
        static_cast<std::uint32_t>((low * high + index) & 0xFFFFFFFFull);
    return value < cap ? value : cap;
}

// Independent of the translated HLSL: the formula and the profile geometry come from the N0 recipe
// and the accepted record, not from generated source.
std::vector<std::uint32_t> reference(const Record& record, const std::vector<std::uint32_t>& source,
                                     std::uint32_t bound, std::uint32_t cap)
{
    const std::uint32_t columns = record.dispatch_x * record.group_x;
    const std::uint32_t rows = record.dispatch_y * record.group_y;
    std::vector<std::uint32_t> output = make_sentinel();
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < columns; ++x) {
            if (x >= bound) continue;
            const std::size_t index = record.profile == x2w_native::profile_2d
                                          ? static_cast<std::size_t>(y) * columns + x
                                          : x;
            if (index < output.size()) output[index] = compute_word(source, x, cap);
        }
    }
    return output;
}

struct Mismatch {
    bool ok = false;
    std::size_t index = 0;
    std::uint32_t expected = 0;
    std::uint32_t actual = 0;
    bool compare(const std::vector<std::uint32_t>& expected_words,
                 const std::vector<std::uint32_t>& actual_words)
    {
        const std::size_t count = expected_words.size();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t value = i < actual_words.size() ? actual_words[i] : 0;
            if (value != expected_words[i]) {
                ok = false;
                index = i;
                expected = expected_words[i];
                actual = value;
                return false;
            }
        }
        ok = actual_words.size() >= count;
        return ok;
    }
};

std::string mismatch_text(const Mismatch& mismatch)
{
    if (mismatch.ok) return "all words match";
    char text[160];
    std::snprintf(text, sizeof(text), "first mismatch index=%zu expected=0x%08x actual=0x%08x",
                  mismatch.index, mismatch.expected, mismatch.actual);
    return text;
}

// ---------------------------------------------------------------- device

void* aligned_memory(std::size_t bytes)
{
    for (Word base = 0x2000000000ull; base < 0x3000000000ull; base += 0x10000) {
        void* memory = VirtualAlloc(reinterpret_cast<void*>(base), bytes, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
        if (memory && (reinterpret_cast<Word>(memory) & 0xffff) == 0) return memory;
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    }
    return nullptr;
}

bool create_adapter(const char* backend, ComPtr<IDXGIAdapter>& adapter, DXGI_ADAPTER_DESC& desc)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (!std::strcmp(backend, "warp")) {
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        return SUCCEEDED(adapter->GetDesc(&desc));
    }
    ComPtr<IDXGIAdapter1> fallback;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&candidate)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 candidate_desc{};
        if (!candidate || FAILED(candidate->GetDesc1(&candidate_desc))) continue;
        if (candidate_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                     __uuidof(ID3D12Device), nullptr)))
            continue;
        if (candidate_desc.VendorId == 0x8086) {
            adapter = candidate;
            return SUCCEEDED(adapter->GetDesc(&desc));
        }
        if (!fallback) fallback = candidate;
    }
    if (fallback) {
        adapter = fallback;
        return SUCCEEDED(adapter->GetDesc(&desc));
    }
    return false;
}

unsigned query_shader_model(IDXGIAdapter* adapter)
{
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return 0;
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        return static_cast<unsigned>(model.HighestShaderModel);
    model.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        return static_cast<unsigned>(model.HighestShaderModel);
    return 0;
}

std::wstring temp_log_path()
{
    wchar_t directory[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    wchar_t name[96];
    std::swprintf(name, 96, L"x2w-dispatch-%lu.log", static_cast<unsigned long>(GetCurrentProcessId()));
    return std::wstring(length && length < MAX_PATH ? directory : L".\\") + name;
}

struct Backend {
    void* guest = nullptr;
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    long fence_value = 0;

    bool open()
    {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 8, arg(&queue_desc),
                                                    arg(&__uuidof(ID3D12CommandQueue)),
                                                    arg(&queue)))) &&
               SUCCEEDED(static_cast<HRESULT>(invoke(guest, 36, 0, 0,
                                                    arg(&__uuidof(ID3D12Fence)), arg(&fence))));
    }

    // A fresh allocator and list per case: the wrapper's pooled allocator cannot be reset again
    // once a submission is outstanding, and the fixture must not depend on that reuse.
    bool recreate()
    {
        const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
        void* fresh_allocator = nullptr;
        void* fresh_list = nullptr;
        if (FAILED(static_cast<HRESULT>(invoke(guest, 9, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               arg(&__uuidof(ID3D12CommandAllocator)),
                                               arg(&fresh_allocator)))))
            return false;
        if (FAILED(static_cast<HRESULT>(invoke(guest, 73, arg(list_desc), arg(fresh_allocator), 0,
                                               arg(&__uuidof(ID3D12GraphicsCommandList)),
                                               arg(&fresh_list)))))
            return false;
        allocator = fresh_allocator;
        list = fresh_list;
        return SUCCEEDED(static_cast<HRESULT>(invoke(list, 139, arg(allocator))));
    }

    bool submit()
    {
        invoke(list, 9);
        invoke(queue, 10, 1, arg(&list));
        ++fence_value;
        if (FAILED(static_cast<HRESULT>(invoke(queue, 14, arg(fence), fence_value)))) return false;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (static_cast<long long>(invoke(fence, 8)) < fence_value &&
               GetTickCount64() < deadline)
            Sleep(1);
        return static_cast<long long>(invoke(fence, 8)) >= fence_value;
    }
};

bool create_guest_adapter(IUnknown* adapter, Backend& backend)
{
    std::array<unsigned char, 64> parameters{};
    return SUCCEEDED(D3D12XboxCreateDevice(adapter, parameters.data(), __uuidof(ID3D12Device),
                                           &backend.guest)) &&
           backend.guest && backend.open();
}

bool make_placed(void* guest, void* memory, std::uint64_t width, std::uint32_t flags, void** out)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    *out = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 133, reinterpret_cast<Word>(memory),
                                                arg(&desc), D3D12_RESOURCE_STATE_COMMON, 0,
                                                arg(&__uuidof(ID3D12Resource)), arg(out)))) &&
           *out;
}

bool make_readback(void* guest, std::uint64_t width, void** out)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    *out = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(
               guest, 27, arg(&props), D3D12_HEAP_FLAG_NONE, arg(&desc),
               D3D12_RESOURCE_STATE_COPY_DEST, 0, arg(&__uuidof(ID3D12Resource)), arg(out)))) &&
           *out;
}

bool make_root(void* guest, void** root)
{
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor = {0, 0};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor = {0, 0};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants = {0, 0, 16};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        return false;
    *root = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 16, 0, arg(blob->GetBufferPointer()),
                                                blob->GetBufferSize(),
                                                arg(&__uuidof(ID3D12RootSignature)), arg(root)))) &&
           *root;
}

// ---------------------------------------------------------------- case harness

struct Target {
    void* source = nullptr;
    void* output = nullptr;
    void* readback = nullptr;
    std::uint32_t* source_mem = nullptr;
    std::uint32_t* output_mem = nullptr;
    std::uint64_t source_width = 0;
    std::uint64_t output_width = 0;
    bool create(Backend& backend, std::uint64_t source_width, std::uint64_t output_width,
                std::uint32_t source_flags = D3D12_RESOURCE_FLAG_NONE,
                std::uint32_t output_flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        void* source_memory = aligned_memory(0x10000);
        void* output_memory = aligned_memory(0x10000);
        if (!source_memory || !output_memory) return false;
        source_mem = static_cast<std::uint32_t*>(source_memory);
        output_mem = static_cast<std::uint32_t*>(output_memory);
        this->source_width = source_width;
        this->output_width = output_width;
        return make_placed(backend.guest, source_memory, source_width, source_flags, &source) &&
               make_placed(backend.guest, output_memory, output_width, output_flags, &output) &&
               make_readback(backend.guest, output_width, &readback);
    }
    void fill(const std::vector<std::uint32_t>& source_words,
              const std::vector<std::uint32_t>& sentinel)
    {
        std::memcpy(source_mem, source_words.data(), source_words.size() * 4);
        std::memcpy(output_mem, sentinel.data(), sentinel.size() * 4);
    }
};

struct Outcome {
    long recorded = 0;
    long skipped = 0;
    bool submitted = false;
    std::string reason;
    std::vector<std::uint32_t> words;
};

void bind_fixture(Backend& backend, void* root, void* pso,
                  const std::array<std::uint32_t, 16>& constants,
                  const std::vector<std::pair<UINT, UINT>>& constant_ranges, Word srv, Word uav,
                  bool bind_srv = true, bool bind_uav = true)
{
    invoke(backend.list, 33, arg(root));
    invoke(backend.list, 25, arg(pso));
    for (const auto& range : constant_ranges)
        invoke(backend.list, 39, 2, range.second, arg(constants.data() + range.first), range.first);
    if (bind_srv) invoke(backend.list, 43, 0, srv);
    if (bind_uav) invoke(backend.list, 45, 1, uav);
}

Outcome execute(Backend& backend, Target& target, const std::function<void()>& bind, UINT x, UINT y,
                UINT z)
{
    Outcome outcome;
    const long recorded = x2w_d3d12_compute_dispatches();
    const long skipped = x2w_d3d12_compute_skipped_dispatches();
    if (!backend.recreate()) return outcome;
    bind();
    invoke(backend.list, 14, x, y, z);
    outcome.recorded = x2w_d3d12_compute_dispatches() - recorded;
    outcome.skipped = x2w_d3d12_compute_skipped_dispatches() - skipped;
    char reason[192] = "";
    if (x2w_native_fixture_list_reason(backend.list, reason, sizeof(reason)))
        outcome.reason = reason;
    // The fixture flag gates Dispatch. Re-enable the helper so a refused case can still be copied
    // for the unchanged-output comparison; the explicit disabled-readback control calls the export
    // directly with the flag off.
    SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
    const UINT before = outcome.recorded > 0 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                             : D3D12_RESOURCE_STATE_COMMON;
    const HRESULT copy = x2w_native_fixture_readback(backend.list, target.output, 0,
                                                     target.readback, 0, target.output_width,
                                                     before);
    outcome.submitted = SUCCEEDED(copy) && backend.submit();
    if (!outcome.submitted) return outcome;
    void* mapped = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(target.readback, 8, 0, 0, arg(&mapped)))) || !mapped)
        return outcome;
    const auto* words = static_cast<const std::uint32_t*>(mapped);
    outcome.words.assign(words, words + target.output_width / 4);
    invoke(target.readback, 9, 0, 0);
    return outcome;
}

bool expect_unchanged(Report& report, const std::string& name, const Outcome& outcome,
                      const std::vector<std::uint32_t>& sentinel, std::size_t expected_words)
{
    const std::size_t count =
        sentinel.size() < expected_words ? sentinel.size() : expected_words;
    const std::vector<std::uint32_t> expected(sentinel.begin(), sentinel.begin() + count);
    Mismatch mismatch;
    const bool complete = outcome.words.size() == expected_words;
    const bool unchanged = complete && mismatch.compare(expected, outcome.words);
    report.check(name + " leaves the output unchanged",
                 outcome.recorded == 0 && outcome.skipped >= 1 && outcome.submitted && unchanged,
                 "recorded=" + std::to_string(outcome.recorded) + " skipped=" +
                     std::to_string(outcome.skipped) + " words=" +
                     std::to_string(outcome.words.size()) + "/" +
                     std::to_string(expected_words) + " " + mismatch_text(mismatch) +
                     (outcome.reason.empty() ? "" : " reason=" + outcome.reason));
    return unchanged;
}

bool expect_reference(Report& report, const std::string& name, const Outcome& outcome,
                      const std::vector<std::uint32_t>& expected, long dispatches)
{
    Mismatch mismatch;
    const bool matched = mismatch.compare(expected, outcome.words);
    report.check(name,
                 outcome.recorded == dispatches && outcome.skipped == 0 && outcome.submitted &&
                     matched,
                 "recorded=" + std::to_string(outcome.recorded) +
                     " skipped=" + std::to_string(outcome.skipped) + " " + mismatch_text(mismatch) +
                     (outcome.reason.empty() ? "" : " reason=" + outcome.reason));
    return matched;
}

std::array<std::uint32_t, 16> constants_for(const Record& record, std::uint32_t bound,
                                            std::uint32_t cap)
{
    std::array<std::uint32_t, 16> constants{};
    for (std::uint32_t i = 0; i < constants.size(); ++i) constants[i] = 0x1000u + i;
    constants[record.bound_constant] = bound;
    constants[record.cap_constant] = cap;
    return constants;
}

// ---------------------------------------------------------------- profiles

struct Profile {
    const char* name = "";
    std::uint64_t key = 0;
    Acceptance acceptance;
    Record record;
    std::vector<std::uint32_t> full_readback;
    std::vector<std::uint32_t> partial_readback;
};

bool load_profile(const std::wstring& cache, Profile& profile, Report& report)
{
    profile.acceptance = x2w_native::inspect(cache, profile.key, {"warp", "hardware"});
    if (!profile.acceptance.accepted()) {
        report.check(std::string("profile ") + profile.name + " is an accepted cache hit", false,
                     profile.acceptance.detail);
        return false;
    }
    profile.record = profile.acceptance.record;
    return true;
}

bool profile_matches(const Profile& profile, Report& report)
{
    const Record& record = profile.record;
    const bool one_d = !std::strcmp(profile.name, "1d");
    const std::uint32_t columns = record.dispatch_x * record.group_x;
    const std::uint32_t rows = record.dispatch_y * record.group_y;
    bool ok = record.profile == (one_d ? 1u : 2u) && columns == (one_d ? 192u : 24u) &&
              rows == (one_d ? 1u : 8u) && record.rsrc2_interpretation == 0 &&
              record.recorded_rsrc2 == 0 && record.guest_wave == 32;
    if (one_d) {
        ok = ok && record.group_x == 32 && record.group_y == 1 && record.group_z == 1 &&
             record.dispatch_x == 6 && record.dispatch_y == 1 && record.dispatch_z == 1 &&
             record.u0_stride == 4 && record.bound_constant == 0 && record.cap_constant == 1;
    } else {
        ok = ok && record.group_x == 8 && record.group_y == 4 && record.group_z == 1 &&
             record.dispatch_x == 3 && record.dispatch_y == 2 && record.dispatch_z == 1 &&
             record.u0_stride == 96 && record.bound_constant == 0 && record.cap_constant == 1;
    }
    report.check(std::string("profile ") + profile.name + " matches the approved geometry", ok,
                 "profile=" + std::to_string(record.profile) + " group=" +
                     std::to_string(record.group_x) + "x" + std::to_string(record.group_y) + "x" +
                     std::to_string(record.group_z) + " dispatch=" +
                     std::to_string(record.dispatch_x) + "x" +
                     std::to_string(record.dispatch_y) + "x" +
                     std::to_string(record.dispatch_z));
    return ok;
}

// ---------------------------------------------------------------- positive cases

void positive_cases(Backend& backend, Profile& profile, void* root, void* pso, Report& report,
                    const std::vector<std::uint32_t>& source)
{
    const Record& record = profile.record;
    const UINT dx = record.dispatch_x, dy = record.dispatch_y, dz = record.dispatch_z;
    const std::uint32_t columns = dx * record.group_x;
    const std::vector<std::uint32_t> sentinel = make_sentinel();
    struct GuardCase {
        const char* name;
        std::uint32_t bound;
        std::uint32_t cap;
        bool split;
    };
    const GuardCase cases[] = {
        {"full", columns, 100000, false},
        {"partial", columns == 192 ? 173u : 13u, 100000, false},
        {"zero", 0, 100000, false},
        {"cap4000", columns, 4000, false},
        {"split", columns, 100000, true},
    };
    for (const GuardCase& guard : cases) {
        Target target;
        if (!target.create(backend, 1024, 1280)) {
            report.check(std::string("case ") + profile.name + "/" + guard.name, false,
                         "resource creation failed");
            continue;
        }
        target.fill(source, sentinel);
        const std::array<std::uint32_t, 16> constants =
            constants_for(record, guard.bound, guard.cap);
        std::vector<std::pair<UINT, UINT>> ranges;
        if (guard.split)
            ranges = {{0, 4}, {4, 4}, {8, 4}, {12, 4}};
        else
            ranges = {{0, 16}};
        const Outcome outcome = execute(
            backend, target,
            [&]() {
                bind_fixture(backend, root, pso, constants, ranges,
                             reinterpret_cast<Word>(target.source_mem),
                             reinterpret_cast<Word>(target.output_mem));
            },
            dx, dy, dz);
        const std::vector<std::uint32_t> expected =
            reference(record, source, guard.bound, guard.cap);
        const std::string name = std::string("GPU ") + profile.name + "/" + guard.name;
        const bool matched = expect_reference(report, name, outcome, expected, 1);
        if (matched && !std::strcmp(guard.name, "full")) profile.full_readback = outcome.words;
        if (matched && !std::strcmp(guard.name, "partial"))
            profile.partial_readback = outcome.words;
        write_words(std::string(profile.name) + "-" + guard.name, "source.bin", source);
        write_words(std::string(profile.name) + "-" + guard.name, "sentinel.bin", sentinel);
        write_words(std::string(profile.name) + "-" + guard.name, "expected.bin", expected);
        write_words(std::string(profile.name) + "-" + guard.name, "readback.bin", outcome.words);
        if (!std::strcmp(guard.name, "full") || !std::strcmp(guard.name, "partial"))
            report.note(std::string("case ") + profile.name + "/" + guard.name +
                        " readback words=" + std::to_string(outcome.words.size()));
    }

    // Repeated Dispatch: two distinct destinations with different valid caps, one submission.
    {
        Target first, second;
        if (!first.create(backend, 1024, 1280) || !second.create(backend, 1024, 1280)) {
            report.check("repeated Dispatch resources", false, "resource creation failed");
            return;
        }
        first.fill(source, sentinel);
        second.fill(source, sentinel);
        const long recorded = x2w_d3d12_compute_dispatches();
        const auto constants_first = constants_for(record, columns, 100000);
        const auto constants_second = constants_for(record, columns, 4000);
        backend.recreate();
        bind_fixture(backend, root, pso, constants_first, {{0, 16}},
                     reinterpret_cast<Word>(first.source_mem), reinterpret_cast<Word>(first.output_mem));
        invoke(backend.list, 14, dx, dy, dz);
        bind_fixture(backend, root, pso, constants_second, {{0, 16}},
                     reinterpret_cast<Word>(second.source_mem),
                     reinterpret_cast<Word>(second.output_mem));
        invoke(backend.list, 14, dx, dy, dz);
        const long delta = x2w_d3d12_compute_dispatches() - recorded;
        // A readback buffer covering both destinations.
        void* readback = nullptr;
        const bool buffer = make_readback(backend.guest, 2560, &readback);
        const HRESULT first_copy = buffer
            ? x2w_native_fixture_readback(backend.list, first.output, 0, readback, 0, 1280,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            : E_FAIL;
        const HRESULT second_copy = buffer
            ? x2w_native_fixture_readback(backend.list, second.output, 0, readback, 1280, 1280,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            : E_FAIL;
        const bool submitted = buffer && SUCCEEDED(first_copy) && SUCCEEDED(second_copy) &&
                               backend.submit();
        std::vector<std::uint32_t> words;
        if (submitted) {
            void* mapped = nullptr;
            if (SUCCEEDED(static_cast<HRESULT>(invoke(readback, 8, 0, 0, arg(&mapped)))) && mapped) {
                const auto* all = static_cast<const std::uint32_t*>(mapped);
                words.assign(all, all + 640);
                invoke(readback, 9, 0, 0);
            }
        }
        const std::vector<std::uint32_t> expected_first = reference(record, source, columns, 100000);
        const std::vector<std::uint32_t> expected_second = reference(record, source, columns, 4000);
        Mismatch m1, m2;
        const std::size_t first_count = words.size() < 320 ? words.size() : 320;
        const std::vector<std::uint32_t> got_first(words.begin(), words.begin() + first_count);
        const std::vector<std::uint32_t> got_second(words.begin() + first_count, words.end());
        const bool ok = delta == 2 && submitted && m1.compare(expected_first, got_first) &&
                        m2.compare(expected_second, got_second);
        report.check(std::string("GPU ") + profile.name + "/repeat records two real Dispatches",
                     ok, "delta=" + std::to_string(delta) + " " + mismatch_text(m1.ok ? m2 : m1));
        write_words(std::string(profile.name) + "-repeat", "expected-first.bin", expected_first);
        write_words(std::string(profile.name) + "-repeat", "expected-second.bin", expected_second);
        write_words(std::string(profile.name) + "-repeat", "readback.bin", words);

        // Sensitivity: the second result must differ from the first reference, proving the
        // changed constants reached the GPU rather than a repeated idempotent write.
        if (ok) {
            Mismatch distinct;
            report.check(std::string("sensitivity ") + profile.name +
                             "/repeat distinguishes the two caps",
                         !distinct.compare(expected_first, got_second),
                         mismatch_text(distinct));
        }
    }
}

// ---------------------------------------------------------------- negative cases

void negative_cases(Backend& backend, IDXGIAdapter* adapter, const Profile& profile, void* root,
                    void* pso, Report& report, const std::vector<std::uint32_t>& source)
{
    const Record& record = profile.record;
    const UINT dx = record.dispatch_x, dy = record.dispatch_y, dz = record.dispatch_z;
    const std::uint32_t columns = dx * record.group_x;
    const std::vector<std::uint32_t> sentinel = make_sentinel();
    const auto constants = constants_for(record, columns, 100000);
    const auto make_target = [&](Target& target, std::uint64_t source_width,
                                 std::uint64_t output_width,
                                 std::uint32_t output_flags =
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        if (!target.create(backend, source_width, output_width,
                           D3D12_RESOURCE_FLAG_NONE, output_flags)) {
            report.check(std::string("negative resources ") + profile.name, false,
                         "resource creation failed");
            return false;
        }
        target.fill(source, sentinel);
        return true;
    };
    const auto bind_all = [&](Target& target) {
        return [&, constants]() {
            bind_fixture(backend, root, pso, constants, {{0, 16}},
                         reinterpret_cast<Word>(target.source_mem),
                         reinterpret_cast<Word>(target.output_mem));
        };
    };

    // Fixture activation disabled at Dispatch time (the pipeline still exists).
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "0");
            const Outcome outcome = execute(backend, target, bind_all(target), dx, dy, dz);
            SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
            expect_unchanged(report, "refusal: fixture disabled", outcome, sentinel, target.output_width / 4);
        }
    }

    // Missing / incomplete bindings use safe, fully created resources.
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    invoke(backend.list, 25, arg(pso));  // pipeline only, no root
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: missing compute root signature", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem), false, true);
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: missing root SRV", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem), true, false);
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: missing root UAV", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 12}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: incomplete root constants", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                    std::uint32_t value = 1;
                    invoke(backend.list, 37, 2, value, 16);  // offset 16 of 16 is invalid
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: invalid constant offset", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                    std::uint32_t value = 1;
                    invoke(backend.list, 39, 2, 8, arg(&value), 12);  // 8 from offset 12 exceeds 16
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: invalid constant range", outcome, sentinel, target.output_width / 4);
        }
    }

    // Invalid guard and dimensions: the fixture boundary, not the generic path, refuses.
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const auto bad = constants_for(record, columns + 1, 100000);
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, bad, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: guard outside the approved profile", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const UINT wrong = dx > 1 ? dx - 1 : dx + 1;
            const Outcome outcome = execute(backend, target, bind_all(target), wrong, dy, dz);
            expect_unchanged(report, "refusal: incorrect Dispatch dimensions", outcome, sentinel, target.output_width / 4);
        }
    }

    // Extents and offsets: oversized allocations must not hide invalid bindings.
    if (true) {
        Target target;
        if (make_target(target, 64, 1280)) {
            const Outcome outcome = execute(backend, target, bind_all(target), dx, dy, dz);
            expect_unchanged(report, "refusal: source extent too small", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 64)) {
            const Outcome outcome = execute(backend, target, bind_all(target), dx, dy, dz);
            expect_unchanged(report, "refusal: destination extent too small", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 4096, 1280)) {
            const Word inside = reinterpret_cast<Word>(target.source_mem) + 4096 - 16;
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}}, inside,
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: source binding offset outside the accessible range",
                             outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.output_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: same resource as SRV and UAV", outcome, sentinel, target.output_width / 4);
        }
    }

    // Wrong root signature and a graphics/placeholder pipeline.
    if (true) {
        void* other = nullptr;
        Target target;
        if (make_root(backend.guest, &other) && make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, other, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: incorrect root signature", outcome, sentinel, target.output_width / 4);
        }
    }
    if (true) {
        alignas(8) static unsigned char graphics_desc[0x300]{};
        void* graphics = nullptr;
        Target target;
        if (SUCCEEDED(static_cast<HRESULT>(invoke(
                backend.guest, 67, arg(graphics_desc), arg(&__uuidof(ID3D12PipelineState)),
                arg(&graphics)))) &&
            graphics && make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    invoke(backend.list, 33, arg(root));
                    invoke(backend.list, 25, arg(graphics));
                    invoke(backend.list, 39, 2, 16, arg(constants.data()), 0);
                    invoke(backend.list, 43, 0, reinterpret_cast<Word>(target.source_mem));
                    invoke(backend.list, 45, 1, reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: placeholder pipeline during Dispatch", outcome,
                             sentinel, target.output_width / 4);
        }
    }

    // Stale arguments after Reset: bind, Reset, Dispatch without rebinding.
    if (true) {
        Target target;
        if (make_target(target, 1024, 1280)) {
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                    backend.recreate();
                },
                dx, dy, dz);
            expect_unchanged(report, "refusal: stale arguments after Reset", outcome, sentinel, target.output_width / 4);
        }
    }

    // Device mismatch: a second wrapped device cannot dispatch the main device's fixture pipeline.
    if (adapter) {
        Backend second;
        Target target;
        if (create_guest_adapter(adapter, second) && second.recreate() &&
            make_target(target, 1024, 1280)) {
            const long recorded = x2w_d3d12_compute_dispatches();
            const long skipped = x2w_d3d12_compute_skipped_dispatches();
            bind_fixture(second, root, pso, constants, {{0, 16}},
                         reinterpret_cast<Word>(target.source_mem),
                         reinterpret_cast<Word>(target.output_mem));
            invoke(second.list, 14, dx, dy, dz);
            const long delta_recorded = x2w_d3d12_compute_dispatches() - recorded;
            const long delta_skipped = x2w_d3d12_compute_skipped_dispatches() - skipped;
            char reason[192] = "";
            x2w_native_fixture_list_reason(second.list, reason, sizeof(reason));
            Mismatch mismatch;
            const std::vector<std::uint32_t> cpu(target.output_mem, target.output_mem + 320);
            const bool cpu_unchanged = mismatch.compare(sentinel, cpu);
            report.check("refusal: device mismatch is refused",
                         delta_recorded == 0 && delta_skipped >= 1 && cpu_unchanged,
                         "recorded=" + std::to_string(delta_recorded) + " skipped=" +
                             std::to_string(delta_skipped) + " " + mismatch_text(mismatch) +
                             (reason[0] ? std::string(" reason=") + reason : ""));
            invoke(second.list, 9);  // close the unexecuted list
        }
    }

    // Disabled readback: valid wrapped list and resources, flag off. The export must return
    // E_ACCESSDENIED before any transition or copy. A prior enabled copy proves the path works and
    // the disabled call must leave that readback untouched rather than overwriting it with the
    // second source.
    if (true) {
        Target first, second;
        if (make_target(first, 1024, 1280) && make_target(second, 1024, 1280)) {
            std::vector<std::uint32_t> other = sentinel;
            for (std::uint32_t& word : other) word ^= 0x01010101u;
            second.fill(source, other);
            backend.recreate();
            const HRESULT enabled = x2w_native_fixture_readback(
                backend.list, first.output, 0, first.readback, 0, 1280,
                D3D12_RESOURCE_STATE_COMMON);
            SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "0");
            const HRESULT disabled = x2w_native_fixture_readback(
                backend.list, second.output, 0, first.readback, 0, 1280,
                D3D12_RESOURCE_STATE_COMMON);
            SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
            const bool submitted = SUCCEEDED(enabled) && backend.submit();
            std::vector<std::uint32_t> words;
            if (submitted) {
                void* mapped = nullptr;
                if (SUCCEEDED(static_cast<HRESULT>(invoke(first.readback, 8, 0, 0, arg(&mapped)))) &&
                    mapped) {
                    const auto* all = static_cast<const std::uint32_t*>(mapped);
                    words.assign(all, all + 320);
                    invoke(first.readback, 9, 0, 0);
                }
            }
            Mismatch kept, overwritten;
            const bool kept_first = kept.compare(sentinel, words);
            const bool wrote_second = overwritten.compare(other, words);
            report.check("refusal: disabled readback returns E_ACCESSDENIED without copying",
                         enabled == S_OK && disabled == E_ACCESSDENIED && submitted &&
                             kept_first && !wrote_second,
                         "enabled=0x" + std::to_string(static_cast<unsigned long>(enabled)) +
                             " disabled=0x" + std::to_string(static_cast<unsigned long>(disabled)) +
                             " first_copy=" + (kept_first ? "kept" : "missing") +
                             " second_copy=" + (wrote_second ? "present" : "absent"));
        }
    }

    // Checked extent arithmetic (pure): an overflowing offset+bytes pair is refused.
    report.check("boundary: extent arithmetic refuses overflow",
                 x2w_native_fixture_extent_ok(~0ull - 8, 16, ~0ull) == 0 &&
                     x2w_native_fixture_extent_ok(0, 768, 768) == 1 &&
                     x2w_native_fixture_extent_ok(1, 768, 768) == 0);
}

// ---------------------------------------------------------------- sensitivity

void sensitivity_cases(Backend& backend, const Profile& profile, void* root, void* pso,
                       Report& report, const std::vector<std::uint32_t>& source)
{
    const Record& record = profile.record;
    const UINT dx = record.dispatch_x, dy = record.dispatch_y, dz = record.dispatch_z;
    const std::uint32_t columns = dx * record.group_x;
    const std::vector<std::uint32_t> sentinel = make_sentinel();
    const std::array<std::uint32_t, 16> constants = constants_for(record, columns, 100000);
    const std::vector<std::uint32_t> expected = reference(record, source, columns, 100000);

    // Omitted Dispatch is detected: the sentinel-only buffer cannot match the reference.
    {
        Target target;
        if (target.create(backend, 1024, 1280)) {
            target.fill(source, sentinel);
            const long before = x2w_d3d12_compute_dispatches();
            backend.recreate();
            bind_fixture(backend, root, pso, constants, {{0, 16}},
                         reinterpret_cast<Word>(target.source_mem),
                         reinterpret_cast<Word>(target.output_mem));
            const bool copied = SUCCEEDED(x2w_native_fixture_readback(
                backend.list, target.output, 0, target.readback, 0, 1280,
                D3D12_RESOURCE_STATE_COMMON));
            const bool submitted = copied && backend.submit();
            std::vector<std::uint32_t> words;
            if (submitted) {
                void* mapped = nullptr;
                if (SUCCEEDED(static_cast<HRESULT>(invoke(target.readback, 8, 0, 0, arg(&mapped)))) &&
                    mapped) {
                    const auto* all = static_cast<const std::uint32_t*>(mapped);
                    words.assign(all, all + 320);
                    invoke(target.readback, 9, 0, 0);
                }
            }
            Mismatch mismatch;
            report.check("sensitivity: omitted Dispatch is detected",
                         x2w_d3d12_compute_dispatches() == before && submitted &&
                             !mismatch.compare(expected, words),
                         mismatch_text(mismatch));
        }
    }

    // A real execution with a different source must not match the original reference, while it
    // does match its own: the readback reflects the submitted input.
    {
        std::vector<std::uint32_t> alternate = source;
        for (std::size_t i = 0; i < alternate.size(); ++i) alternate[i] ^= 0x00010001u;
        Target target;
        if (target.create(backend, 1024, 1280)) {
            target.fill(alternate, sentinel);
            const Outcome outcome = execute(
                backend, target,
                [&]() {
                    bind_fixture(backend, root, pso, constants, {{0, 16}},
                                 reinterpret_cast<Word>(target.source_mem),
                                 reinterpret_cast<Word>(target.output_mem));
                },
                dx, dy, dz);
            const std::vector<std::uint32_t> own = reference(record, alternate, columns, 100000);
            Mismatch mismatched, matched;
            report.check("sensitivity: a different source does not match the original reference",
                         !mismatched.compare(expected, outcome.words), mismatch_text(mismatched));
            report.check("sensitivity: the different source matches its own reference",
                         outcome.recorded == 1 && matched.compare(own, outcome.words),
                         mismatch_text(matched));
        }
    }

    // A deliberately wrong expected word is detected against the real full-guard readback.
    {
        std::vector<std::uint32_t> wrong = expected;
        wrong[5] ^= 0xFFFFFFFFu;
        Mismatch mismatch;
        report.check("sensitivity: an incorrect expected word is detected",
                     !profile.full_readback.empty() &&
                         !mismatch.compare(wrong, profile.full_readback) && mismatch.index == 5,
                     mismatch_text(mismatch));
    }

    // Modified sentinel in a location the partial guard must leave untouched, checked against the
    // real partial-guard readback.
    {
        const std::uint32_t partial_bound = columns == 192 ? 173u : 13u;
        const std::vector<std::uint32_t> partial_expected =
            reference(record, source, partial_bound, 100000);
        std::vector<std::uint32_t> modified = profile.partial_readback;
        const std::size_t untouched = columns == 192 ? 200u : 200u;
        if (modified.size() > untouched) modified[untouched] = 0x12345678u;
        Mismatch mismatch;
        report.check("sensitivity: a modified sentinel is detected",
                     !profile.partial_readback.empty() &&
                         !mismatch.compare(partial_expected, modified) &&
                         mismatch.index == untouched,
                     mismatch_text(mismatch));
    }

    // Incorrect reference row stride for the two-dimensional profile, checked against the real
    // full-guard readback.
    if (record.profile == x2w_native::profile_2d) {
        std::vector<std::uint32_t> wrong_stride = make_sentinel();
        for (std::uint32_t y = 0; y < 8; ++y)
            for (std::uint32_t x = 0; x < 24; ++x)
                wrong_stride[static_cast<std::size_t>(y) * 4 + x] =
                    compute_word(source, x, 100000);
        Mismatch mismatch;
        report.check("sensitivity: an incorrect reference row stride is detected",
                     !profile.full_readback.empty() &&
                         !mismatch.compare(wrong_stride, profile.full_readback),
                     mismatch_text(mismatch));
    }
}

// ---------------------------------------------------------------- create refusals

bool clone_request(const std::wstring& cache, std::uint64_t key, const char* tag,
                   std::wstring& directory)
{
    wchar_t temp[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) return false;
    wchar_t name[128];
    std::swprintf(name, 128, L"x2w-dispatch-%lu-%hs",
                  static_cast<unsigned long>(GetCurrentProcessId()), tag);
    directory = std::wstring(temp) + name;
    CreateDirectoryW(directory.c_str(), nullptr);
    wchar_t stem[17];
    std::swprintf(stem, 17, L"%016llx", static_cast<unsigned long long>(key));
    struct File {
        const wchar_t* prefix;
        const wchar_t* suffix;
    };
    const File files[] = {
        {L"req_", L".csn"}, {L"", L".csn.hlsl"}, {L"", L".csn.cso"},
        {L"", L".csn.manifest"}, {L"", L".csn.src"}};
    for (const File& file : files) {
        const std::wstring from = cache + L"\\" + file.prefix + stem + file.suffix;
        const std::wstring to = directory + L"\\" + file.prefix + stem + file.suffix;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) return false;
    }
    return true;
}

void remove_tree(const std::wstring& directory)
{
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!std::wcscmp(data.cFileName, L".") || !std::wcscmp(data.cFileName, L"..")) continue;
            const std::wstring entry = directory + L"\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(entry);
            else DeleteFileW(entry.c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(directory.c_str());
}

void create_refusals(const std::wstring& cache, void* device, Report& report)
{
    const std::uint64_t key = x2w_native::golden_key_2d;
    const std::string key_text = x2w_native::key_text(key);
    const auto attempt = [&](const std::wstring& directory, const char* label, HRESULT expected_hr,
                             int expected_state) {
        void* root = reinterpret_cast<void*>(1);
        void* pso = reinterpret_cast<void*>(1);
        int state = -1;
        char detail[256] = "";
        const HRESULT hr = x2w_native_fixture_create(device, directory.c_str(), key_text.c_str(),
                                                     &root, &pso, &state, detail, sizeof(detail));
        report.check(label,
                     hr == expected_hr && !root && !pso &&
                         (expected_state < 0 || state == expected_state),
                     std::string("hr=0x") + std::to_string(static_cast<unsigned long>(hr)) +
                         " state=" + std::to_string(state) + " " + detail);
    };

    {
        void* root = reinterpret_cast<void*>(1);
        void* pso = reinterpret_cast<void*>(1);
        int state = -1;
        char detail[256] = "";
        report.check("create refusal: an unapproved key is rejected",
                     x2w_native_fixture_create(device, cache.c_str(), "0000000000000000", &root,
                                               &pso, &state, detail, sizeof(detail)) ==
                             E_INVALIDARG &&
                         !root && !pso);
    }

    std::wstring missing;
    if (clone_request(cache, key, "missing", missing)) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((missing + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!std::wcscmp(data.cFileName, L".") || !std::wcscmp(data.cFileName, L".."))
                    continue;
                DeleteFileW((missing + L"\\" + data.cFileName).c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        attempt(missing, "create refusal: a missing cache entry creates no pipeline", S_FALSE,
                x2w_native::state_code(State::missing));
        remove_tree(missing);
    }

    std::wstring corrupt;
    if (clone_request(cache, key, "corrupt", corrupt)) {
        wchar_t stem[17];
        std::swprintf(stem, 17, L"%016llx", static_cast<unsigned long long>(key));
        const std::wstring cso = corrupt + L"\\" + stem + L".csn.cso";
        HANDLE file = CreateFileW(cso.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            unsigned char byte = 0;
            DWORD moved = 0;
            SetFilePointer(file, 32, nullptr, FILE_BEGIN);
            ReadFile(file, &byte, 1, &moved, nullptr);
            byte ^= 0xFF;
            SetFilePointer(file, 32, nullptr, FILE_BEGIN);
            WriteFile(file, &byte, 1, &moved, nullptr);
            CloseHandle(file);
        }
        attempt(corrupt, "create refusal: a corrupt artifact creates no pipeline", S_FALSE,
                x2w_native::state_code(State::corrupt));
        remove_tree(corrupt);
    }

    std::wstring refused;
    if (clone_request(cache, key, "refused", refused)) {
        wchar_t stem[17];
        std::swprintf(stem, 17, L"%016llx", static_cast<unsigned long long>(key));
        std::vector<unsigned char> bytes;
        Record record;
        std::string detail;
        const std::wstring request = refused + L"\\req_" + stem + L".csn";
        if (x2w_native::read_bounded(request, x2w_native::max_record, bytes) ==
                x2w_native::ReadStatus::ok &&
            x2w_native::parse_record(bytes.data(), bytes.size(), record, detail) &&
            x2w_native::validate_record(record, detail)) {
            const std::string code = x2w_native::digest_hex(
                x2w_native::sha256(record.code.data(), record.code.size()));
            const std::string text =
                std::string(x2w_native::manifest_header) + "\n" + "key " + key_text + "\n" +
                "revision " + x2w_native::revision + "\n" + "status refused\n" +
                "record_version " + std::to_string(record.version) + "\n" + "record_sha256 " +
                x2w_native::digest_hex(x2w_native::record_digest(record)) + "\n" + "code_bytes " +
                std::to_string(record.code.size()) + "\n" + "code_sha256 " + code + "\n" +
                "reason Unsupported program: unverified private LDS read\n";
            write_bytes(refused + L"\\" + stem + L".csn.manifest", text.data(), text.size());
            attempt(refused, "create refusal: a refused entry creates no pipeline", S_FALSE,
                    x2w_native::state_code(State::refused));
        } else {
            report.check("create refusal: a refused entry creates no pipeline", false,
                         "could not build the refusal clone");
        }
        remove_tree(refused);
    }
}

// ---------------------------------------------------------------- debug log

void debug_report(const std::wstring& log_path, bool debug, Report& report)
{
    std::string text;
    if (!read_text_bounded(log_path, text, 8u << 20)) {
        if (debug) report.check("debug: the shim log is available", false, "no log file");
        return;
    }
    std::size_t enabled = 0, errors = 0, corruption = 0, removed = 0, warnings = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        if (line.find("debug layer:") != std::string::npos &&
            line.find("enabled") != std::string::npos)
            ++enabled;
        if (line.find("DEVICE REMOVED") != std::string::npos) ++removed;
        if (line.find("x2w-d3d12-debug") != std::string::npos) {
            if (line.find("sev=0") != std::string::npos) ++corruption;
            if (line.find("sev=1") != std::string::npos) ++errors;
            if (line.find("sev=2") != std::string::npos) ++warnings;
        }
    }
    if (debug) {
        report.check("debug: the debug layer was enabled", enabled > 0);
        report.check("debug: no error-severity messages", errors == 0,
                     std::to_string(errors) + " errors, " + std::to_string(warnings) +
                         " warnings, " + std::to_string(corruption) + " corruption");
        report.check("debug: no corruption messages", corruption == 0);
        report.check("debug: no device removal", removed == 0);
        report.note("debug: warnings=" + std::to_string(warnings) + " errors=" +
                    std::to_string(errors) + " corruption=" + std::to_string(corruption));
    }
}

// ---------------------------------------------------------------- evidence

void write_evidence(const Report& report, const std::string& mode, const std::string& adapter,
                    const std::string& extra, const std::string& shim_log)
{
    const std::wstring directory = run_directory.empty() ? evidence_dir() : run_directory;
    if (directory.empty()) return;
    CreateDirectoryW(directory.c_str(), nullptr);
    CreateDirectoryW((directory + L"\\readback").c_str(), nullptr);
    const std::wstring base = directory + L"\\dispatch";
    std::string log;
    for (const auto& check : report.checks)
        log += std::string(check.second ? "PASS " : "FAIL ") + check.first + "\n";
    for (const auto& note : report.notes) log += "NOTE " + note + "\n";
    log += "adapter " + adapter + "\n";
    if (!extra.empty()) log += extra + "\n";
    log += "shim_log " + shim_log + "\n";
    log += std::string("result ") + (report.failures ? "FAIL" : "PASS") + "\n";
    write_bytes(base + L".log", log.data(), log.size());
    const auto escape = [](const std::string& value) {
        std::string out;
        for (char c : value) {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        return out;
    };
    std::string json = "{\n  \"mode\": \"" + escape(mode) + "\",\n  \"adapter\": \"" +
                       escape(adapter) + "\",\n  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        json += "    {\"name\": \"" + escape(report.checks[i].first) + "\", \"ok\": " +
                (report.checks[i].second ? "true" : "false") + "}";
        if (i + 1 != report.checks.size()) json += ",";
        json += "\n";
    }
    json += "  ],\n  \"failures\": " + std::to_string(report.failures) + ",\n  \"notes\": [";
    for (std::size_t i = 0; i < report.notes.size(); ++i) {
        json += "\"" + escape(report.notes[i]) + "\"";
        if (i + 1 != report.notes.size()) json += ", ";
    }
    json += "],\n  \"result\": \"" + std::string(report.failures ? "FAIL" : "PASS") + "\"\n}\n";
    write_bytes(base + L".json", json.data(), json.size());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: startup_compute_native_dispatch <warp|hardware> <cache-dir> [debug]\n");
        return 2;
    }
    const std::string backend_name = argv[1];
    const std::wstring cache = wide(argv[2]);
    const bool debug = argc > 3 && !std::strcmp(argv[3], "debug");
    if (GetFileAttributesW(cache.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "the accepted native compute cache is absent: %s\n", argv[2]);
        return 77;
    }
    CreateDirectoryW(evidence_dir().c_str(), nullptr);

    Report report;
    report.mode = backend_name + (debug ? "-debug" : "");
    run_directory = evidence_dir();
    if (!run_directory.empty()) {
        run_directory += L"\\" + wide(report.mode.c_str());
        CreateDirectoryW(run_directory.c_str(), nullptr);
    }
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (!create_adapter(backend_name.c_str(), adapter, adapter_desc)) return 77;

    SetEnvironmentVariableA("X2W_D3D12_PERMISSIVE", "1");  // required for the recording wrapper
    SetEnvironmentVariableA("X2W_D3D12_RENDER", "1");      // installs the typed list handlers
    SetEnvironmentVariableA("X2W_D3D12_LIST_NOOP", "1");   // installs the list-slot dispatcher
    SetEnvironmentVariableA("X2W_D3D12_DEBUG", debug ? "1" : "0");
    SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
    const std::wstring shim_log = temp_log_path();
    DeleteFileW(shim_log.c_str());
    SetEnvironmentVariableW(L"X2W_TRAP_LOG", shim_log.c_str());

    Backend backend;
    if (!create_guest_adapter(adapter.Get(), backend)) {
        report.check("a wrapped desktop device, queue, allocator, list and fence are available",
                     false);
        write_evidence(report, report.mode, "", "", narrow(shim_log));
        return 1;
    }
    char adapter_text[512];
    std::snprintf(adapter_text, sizeof(adapter_text), "%ls vendor=%04x device=%04x",
                  adapter_desc.Description, adapter_desc.VendorId, adapter_desc.DeviceId);
    report.check("the wrapper is in render mode (typed list handlers installed)",
                 x2w_native_fixture_render_mode() == 1);

    Profile profiles[2];
    profiles[0].name = "1d";
    profiles[0].key = x2w_native::golden_key_1d;
    profiles[1].name = "2d";
    profiles[1].key = x2w_native::golden_key_2d;
    std::vector<std::uint32_t> source = make_source();

    for (Profile& profile : profiles) {
        if (!load_profile(cache, profile, report)) continue;
        if (!profile_matches(profile, report)) continue;

        void* root = nullptr;
        void* pso = nullptr;
        int state = -1;
        char detail[512] = "";
        const std::string key = x2w_native::key_text(profile.key);
        const HRESULT created = x2w_native_fixture_create(
            backend.guest, cache.c_str(), key.c_str(), &root, &pso, &state, detail, sizeof(detail));
        report.check(std::string("profile ") + profile.name + " creates a real fixture pipeline",
                     created == S_OK && root && pso &&
                         state == x2w_native::state_code(State::hit),
                     std::string("hr=0x") + std::to_string(static_cast<unsigned long>(created)) +
                         " " + detail);
        if (created != S_OK || !root || !pso) continue;

        int path = -1, cls = -1;
        unsigned long long info_key = 0;
        unsigned int types[8]{}, registers[8]{}, spaces[8]{}, counts[8]{}, used = 0;
        report.check(std::string("profile ") + profile.name + " classification and metadata",
                     x2w_native_fixture_info(pso, &path, &cls, &info_key) && path == 162 &&
                         cls == 2 && info_key == profile.key &&
                         x2w_native_fixture_root(root, types, registers, spaces, counts, 8, &used) &&
                         used == 3 && types[0] == 3 && types[1] == 4 && types[2] == 1 &&
                         counts[2] == 16 &&
                         x2w_d3d12_pipeline_real(pso) == 1,
                     "path=" + std::to_string(path) + " cls=" + std::to_string(cls));

        positive_cases(backend, profile, root, pso, report, source);
        negative_cases(backend, adapter.Get(), profile, root, pso, report, source);
        sensitivity_cases(backend, profile, root, pso, report, source);

        x2w_native_fixture_release(pso);
        x2w_native_fixture_release_root(root);
    }

    create_refusals(cache, backend.guest, report);
    debug_report(shim_log, debug, report);
    report.note("compute dispatches recorded=" +
                std::to_string(x2w_d3d12_compute_dispatches()) + " skipped=" +
                std::to_string(x2w_d3d12_compute_skipped_dispatches()));
    const unsigned model = query_shader_model(adapter.Get());
    // Preserve the raw shim/debug log next to the run's evidence.
    if (!run_directory.empty()) {
        std::string shim;
        if (read_text_bounded(shim_log, shim, 8u << 20))
            write_bytes(run_directory + L"\\shim.log", shim.data(), shim.size());
    }
    write_evidence(report, report.mode, adapter_text,
                   "shader_model=0x" + std::to_string(model), narrow(shim_log));
    std::printf("%s %s: %zu checks, %d failed, dispatches=%ld skipped=%ld\n",
                report.failures ? "FAIL" : "PASS", report.mode.c_str(), report.checks.size(),
                report.failures, x2w_d3d12_compute_dispatches(),
                x2w_d3d12_compute_skipped_dispatches());
    return report.failures ? 1 : 0;
}
