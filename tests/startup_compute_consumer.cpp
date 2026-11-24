// Source-built compute fixture for the startup runtime (#1412).
//
//   startup_compute_consumer <warp|hardware> key <container>
//   startup_compute_consumer <warp|hardware> prepare <container> <output>
//   startup_compute_consumer <warp|hardware> execute <variant> <container> <output>
//
// Variants: normal, partial, repeat, missing-root, bad-uav, same-binding, wide-root, root-index-31,
// root-index-32, offset-constants, incomplete, bad-offset, bad-count, large-count, reset-constants,
// root-invalidate, upload-limit, graphics-dispatch, compute-draw, reset-stale. The fixture writes
// <output>.before and <output> as 64 little-endian DWORDs.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);
extern "C" __declspec(dllimport) std::uint64_t x2w_d3d12_compute_cache_key(const void*, std::size_t);
extern "C" __declspec(dllimport) long x2w_d3d12_compute_dispatches();
extern "C" __declspec(dllimport) long x2w_d3d12_compute_skipped_dispatches();
extern "C" __declspec(dllimport) long x2w_d3d12_pipeline_real(void* pipeline);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0,
            Word e = 0, Word f = 0, Word g = 0, Word h = 0)
{
    using Slot = Word(WINAPI*)(void*, Word, Word, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f, g, h);
}

bool read_file(const char* path, std::vector<char>& out)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out.assign(std::istreambuf_iterator<char>(input), {});
    return true;
}

bool write_words(const char* path, const std::array<std::uint32_t, 64>& words)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return output.good();
}

bool create_adapter(const char* backend, ComPtr<IDXGIAdapter>& adapter)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (!std::strcmp(backend, "warp"))
        return SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    for (UINT n = 0;; ++n) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                               IID_PPV_ARGS(&candidate));
        if (hr == DXGI_ERROR_NOT_FOUND) return false;
        if (FAILED(hr)) return false;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc))) return false;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
            adapter = candidate;
            return true;
        }
    }
}

void* aligned_memory(std::size_t bytes)
{
    for (Word base = 0x2000000000ull; base < 0x3000000000ull; base += 0x10000) {
        void* memory = VirtualAlloc(reinterpret_cast<void*>(base), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory && (reinterpret_cast<Word>(memory) & 0xffff) == 0) return memory;
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    }
    return nullptr;
}

bool create_placed_buffer(void* guest, void* memory, UINT flags, void** resource)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 133, reinterpret_cast<Word>(memory), arg(&desc),
        D3D12_RESOURCE_STATE_COMMON, 0, arg(&__uuidof(ID3D12Resource)), arg(resource))));
}

bool normal_root_blob(ComPtr<ID3DBlob>& blob)
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
    ComPtr<ID3DBlob> error;
    return SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
}

bool create_root(void* guest, void** root)
{
    ComPtr<ID3DBlob> blob;
    if (!normal_root_blob(blob)) return false;
    *root = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 16, 0, arg(blob->GetBufferPointer()),
        blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(root)))) && *root;
}

// A root signature with a descriptor table, used only to check that the compute slice refuses it.
bool create_table_root(void* guest, void** root)
{
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) return false;
    *root = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 16, 0, arg(blob->GetBufferPointer()),
        blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(root)))) && *root;
}

// A 34-parameter root signature, used to check the 32-parameter limit and the bounds of the
// per-parameter constant storage. The shader itself only uses t0, u0 and b0. Root descriptors cost
// two DWORDs each, so the extra parameters are one-DWORD root constants to stay inside the
// signature's 64-DWORD budget.
bool create_wide_root(void* guest, void** root)
{
    std::array<D3D12_ROOT_PARAMETER, 34> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor = {0, 0};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor = {0, 0};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 2; i < 33; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[i].Constants = {i - 2, 0, 1};
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[33].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[33].Constants = {31, 0, 16};
    parameters[33].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(parameters.size()), parameters.data(), 0, nullptr,
                                   D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) return false;
    *root = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 16, 0, arg(blob->GetBufferPointer()),
        blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(root)))) && *root;
}

// Exactly 32 parameters, each a one-DWORD root constant in b0..b31: the largest list this slice
// stores. Index 31 is valid and index 32 is not.
bool create_capacity_root(void* guest, void** root)
{
    std::array<D3D12_ROOT_PARAMETER, 32> parameters{};
    for (UINT i = 0; i < parameters.size(); ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[i].Constants = {i, 0, 1};
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(parameters.size()), parameters.data(), 0, nullptr,
                                   D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) return false;
    *root = nullptr;
    return SUCCEEDED(static_cast<HRESULT>(invoke(guest, 16, 0, arg(blob->GetBufferPointer()),
        blob->GetBufferSize(), arg(&__uuidof(ID3D12RootSignature)), arg(root)))) && *root;
}

void fill_constants(std::array<std::uint32_t, 16>& constants)
{
    constants.fill(0);
    constants[0] = 64;   // thread count, not read by this shader
    constants[1] = 3;    // per-thread multiplier
    constants[12] = 1;   // config[3].x: addend per dispatch
    constants[13] = 7;   // config[3].y
    constants[14] = 11;  // config[3].z
    constants[15] = 13;  // config[3].w
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    const char* backend = argv[1];
    const char* mode = argv[2];
    if (!std::strcmp(mode, "key")) {
        std::vector<char> container;
        if (!read_file(argv[3], container)) return 3;
        std::printf("key=%016llx\n", static_cast<unsigned long long>(
            x2w_d3d12_compute_cache_key(container.data(), container.size())));
        return 0;
    }
    const bool prepare = !std::strcmp(mode, "prepare");
    const bool prepare_table = !std::strcmp(mode, "prepare-table");
    const bool prepare_unwrapped = !std::strcmp(mode, "prepare-unwrapped");
    const bool execute = !std::strcmp(mode, "execute");
    if (!prepare && !prepare_table && !prepare_unwrapped && !execute) return 2;
    const char* variant = execute ? argv[3] : "normal";
    const char* container_path = (prepare || prepare_table || prepare_unwrapped) ? argv[3] : argv[4];
    const char* output_path = (prepare || prepare_table || prepare_unwrapped) ? argv[4] : argv[5];
    if ((prepare || prepare_table || prepare_unwrapped) && argc != 5) return 2;
    if (execute && argc != 6) return 2;
    std::vector<char> container;
    if (!read_file(container_path, container) || container.empty()) return 3;

    ComPtr<IDXGIAdapter> adapter;
    if (!create_adapter(backend, adapter)) return 77;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(adapter->GetDesc(&adapter_desc))) return 4;
    std::printf("adapter=%ls\n", adapter_desc.Description);

    std::array<unsigned char, 64> parameters{};
    void* guest = nullptr;
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &guest)) || !guest)
        return 5;
    // prepare-unwrapped passes a desktop root object the shim did not create. The slice must refuse
    // it rather than fall back to a root from the container.
    ComPtr<ID3D12RootSignature> desktop_root;
    void* root = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    if (prepare_unwrapped) {
        ComPtr<ID3D12Device> desktop;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 77;
        ComPtr<ID3DBlob> blob;
        if (!normal_root_blob(blob)) return 6;
        if (FAILED(desktop->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                IID_PPV_ARGS(&desktop_root)))) return 6;
        pso_desc.pRootSignature = desktop_root.Get();
    } else {
        if (prepare_table ? !create_table_root(guest, &root) : !create_root(guest, &root)) return 6;
        pso_desc.pRootSignature = static_cast<ID3D12RootSignature*>(root);
    }
    pso_desc.CS = {container.data(), static_cast<UINT>(container.size())};
    void* pipeline = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 62, arg(&pso_desc), 0, 0,
        arg(&__uuidof(ID3D12PipelineState)), arg(&pipeline)))) || !pipeline) return 7;
    std::printf("pipeline=%s\n", x2w_d3d12_pipeline_real(pipeline) ? "real" : "placeholder");
    if (prepare || prepare_table || prepare_unwrapped) return 0;

    ComPtr<ID3D12Device> desktop;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&desktop)))) return 77;
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    if (FAILED(desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) || !heaps.Supported)
        return 77;

    void* source_memory = aligned_memory(0x10000);
    void* output_memory = aligned_memory(0x10000);
    if (!source_memory || !output_memory) return 8;
    auto* source_words = static_cast<std::uint32_t*>(source_memory);
    auto* output_words = static_cast<std::uint32_t*>(output_memory);
    for (std::uint32_t i = 0; i < 64; ++i) source_words[i] = i * 13u + 7u;
    for (std::uint32_t i = 0; i < 64; ++i) output_words[i] = 0xdeadbeefu;
    std::array<std::uint32_t, 64> before{};
    std::memcpy(before.data(), output_words, before.size() * 4);
    if (!write_words((std::string(output_path) + ".before").c_str(), before)) return 9;

    void* source = nullptr;
    void* output = nullptr;
    if (!create_placed_buffer(guest, source_memory, D3D12_RESOURCE_FLAG_NONE, &source) ||
        !create_placed_buffer(guest, output_memory, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &output)) return 10;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    const unsigned list_desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
    void* queue = nullptr;
    void* allocator = nullptr;
    void* list = nullptr;
    void* fence = nullptr;
    if (FAILED(static_cast<HRESULT>(invoke(guest, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&queue)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 9, D3D12_COMMAND_LIST_TYPE_DIRECT,
            arg(&__uuidof(ID3D12CommandAllocator)), arg(&allocator)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 73, arg(list_desc), arg(allocator), 0,
            arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&list)))) ||
        FAILED(static_cast<HRESULT>(invoke(guest, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&fence))))) return 11;

    std::array<std::uint32_t, 16> constants{};
    fill_constants(constants);
    // Each submission gets a new fence value so a later wait cannot pass on an older signal.
    LONG fence_value = 0;
    const auto submit_and_wait = [&]() -> bool {
        invoke(list, 9);
        invoke(queue, 10, 1, arg(&list));
        ++fence_value;
        if (FAILED(static_cast<HRESULT>(invoke(queue, 14, arg(fence), fence_value)))) return false;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (invoke(fence, 8) < static_cast<Word>(fence_value) && GetTickCount64() < deadline) Sleep(1);
        return invoke(fence, 8) >= static_cast<Word>(fence_value);
    };
    const auto bind = [&](bool with_root, void* pipeline_to_bind) {
        invoke(list, 139, arg(allocator));
        if (with_root) invoke(list, 33, arg(root));
        invoke(list, 25, arg(pipeline_to_bind));
        if (!std::strcmp(variant, "partial")) {
            invoke(list, 39, 2, 4, arg(constants.data()), 0);
            invoke(list, 39, 2, 12, arg(constants.data() + 4), 4);
        } else {
            invoke(list, 39, 2, 16, arg(constants.data()), 0);
        }
        // same-binding points the root SRV at the UAV-capable output buffer so both roots resolve
        // to one resource and the runtime has to refuse the contradictory state.
        const bool same_binding = std::strcmp(variant, "same-binding") == 0;
        const Word source_address = reinterpret_cast<Word>(same_binding ? output_memory : source_memory);
        const Word output_address = reinterpret_cast<Word>(output_memory) +
            (std::strcmp(variant, "bad-uav") == 0 ? 1 : 0);
        invoke(list, 43, 0, source_address);
        invoke(list, 45, 1, output_address);
    };

    const auto bind_base = [&](void* root_to_bind, void* pipeline_to_bind) {
        invoke(list, 139, arg(allocator));
        invoke(list, 33, arg(root_to_bind));
        invoke(list, 25, arg(pipeline_to_bind));
    };
    const auto set_objects = [&](Word source_address, Word output_address) {
        invoke(list, 43, 0, source_address);
        invoke(list, 45, 1, output_address);
    };

    if (!std::strcmp(variant, "graphics-dispatch")) {
        alignas(8) static unsigned char graphics_desc[0x300]{};
        void* graphics = nullptr;
        if (FAILED(static_cast<HRESULT>(invoke(guest, 67, arg(graphics_desc), arg(&__uuidof(ID3D12PipelineState)),
            arg(&graphics)))) || !graphics) return 12;
        invoke(list, 139, arg(allocator));
        invoke(list, 33, arg(root));
        invoke(list, 25, arg(graphics));
        invoke(list, 43, 0, reinterpret_cast<Word>(source_memory));
        invoke(list, 45, 1, reinterpret_cast<Word>(output_memory));
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "wide-root")) {
        // Bind a 34-parameter root, poke parameter 33 (past the 32-entry storage) and Dispatch:
        // the runtime must refuse instead of writing out of bounds. A graphics/compute pipeline
        // round trip must not reactivate it, and a normal dispatch afterwards must still work.
        void* wide = nullptr;
        if (!create_wide_root(guest, &wide)) return 18;
        alignas(8) static unsigned char graphics_desc[0x300]{};
        void* graphics = nullptr;
        if (FAILED(static_cast<HRESULT>(invoke(guest, 67, arg(graphics_desc), arg(&__uuidof(ID3D12PipelineState)),
            arg(&graphics)))) || !graphics) return 12;
        invoke(list, 139, arg(allocator));
        invoke(list, 25, arg(pipeline));
        invoke(list, 33, arg(wide));
        invoke(list, 37, 33, 9, 0);
        std::uint32_t value = 9;
        invoke(list, 39, 33, 1, arg(&value), 0);
        invoke(list, 14, 1, 1, 1);
        invoke(list, 25, arg(graphics));
        invoke(list, 25, arg(pipeline));
        invoke(list, 14, 1, 1, 1);
        invoke(list, 9);  // Close before Reset; both refused Dispatches recorded nothing.
        bind(true, pipeline);
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "root-index-31") || !std::strcmp(variant, "root-index-32")) {
        const bool invalid = !std::strcmp(variant, "root-index-32");
        void* capacity = nullptr;
        if (!create_capacity_root(guest, &capacity)) return 19;
        invoke(list, 139, arg(allocator));
        invoke(list, 25, arg(pipeline));
        invoke(list, 33, arg(capacity));
        std::uint32_t value = 5;
        if (invalid) {
            invoke(list, 37, 32, 5, 0);
            invoke(list, 39, 32, 1, arg(&value), 0);
        } else {
            invoke(list, 37, 31, 5, 0);
            invoke(list, 39, 31, 1, arg(&value), 0);
        }
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "offset-constants")) {
        // The same 16 values written back to front, four at a time.
        bind_base(root, pipeline);
        invoke(list, 39, 2, 4, arg(constants.data() + 12), 12);
        invoke(list, 39, 2, 4, arg(constants.data() + 8), 8);
        invoke(list, 39, 2, 4, arg(constants.data() + 4), 4);
        invoke(list, 39, 2, 4, arg(constants.data()), 0);
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "incomplete")) {
        bind_base(root, pipeline);
        invoke(list, 39, 2, 12, arg(constants.data()), 0);  // 12 of 16 DWORDs
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "bad-offset") || !std::strcmp(variant, "bad-count")) {
        bind_base(root, pipeline);
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        if (!std::strcmp(variant, "bad-offset"))
            invoke(list, 37, 2, 5, 16);
        else
            invoke(list, 39, 2, 8, arg(constants.data()), 12);
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "large-count")) {
        // A count far past the declared range must fail before any source-memory scan.
        bind_base(root, pipeline);
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        invoke(list, 39, 2, 0xFFFFFFFFu, arg(constants.data()), 0);
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "reset-constants")) {
        bind(true, pipeline);
        invoke(list, 14, 1, 1, 1);
        if (!submit_and_wait()) return 21;
        bind_base(root, pipeline);  // Reset cleared the initialized-DWORD mask.
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "root-invalidate")) {
        void* other = nullptr;
        if (!create_capacity_root(guest, &other)) return 20;
        bind(true, pipeline);
        invoke(list, 14, 1, 1, 1);
        // A structurally different root signature, then the original again: the mask must be
        // dropped and only the eight values written afterwards count.
        invoke(list, 33, arg(other));
        invoke(list, 33, arg(root));
        set_objects(reinterpret_cast<Word>(source_memory), reinterpret_cast<Word>(output_memory));
        invoke(list, 39, 2, 8, arg(constants.data()), 0);
        invoke(list, 14, 1, 1, 1);
    } else if (!std::strcmp(variant, "upload-limit")) {
        // Three host buffers force the retained-upload path; the limit is lowered to two.
        alignas(4096) static std::array<std::uint8_t, 0x10000> host[3];
        for (int b = 0; b < 2; ++b) {
            auto* words = reinterpret_cast<std::uint32_t*>(host[b].data());
            for (std::uint32_t i = 0; i < 64; ++i) words[i] = i * 13u + 7u;
        }
        bind_base(root, pipeline);
        invoke(list, 39, 2, 16, arg(constants.data()), 0);
        invoke(list, 45, 1, reinterpret_cast<Word>(output_memory));
        for (int b = 0; b < 3; ++b) {
            invoke(list, 43, 0, reinterpret_cast<Word>(host[b].data()));
            invoke(list, 14, 1, 1, 1);
        }
    } else {
        bind(std::strcmp(variant, "missing-root") != 0, pipeline);
        invoke(list, 14, 1, 1, 1);
        if (!std::strcmp(variant, "repeat")) invoke(list, 14, 1, 1, 1);
        if (!std::strcmp(variant, "compute-draw")) invoke(list, 12, 3, 1);
    }

    if (!std::strcmp(variant, "reset-stale")) {
        if (!submit_and_wait()) return 13;
        invoke(list, 139, arg(allocator));
        invoke(list, 14, 1, 1, 1);
    }

    if (!submit_and_wait()) return 15;

    std::array<std::uint32_t, 64> after{};
    std::memcpy(after.data(), output_words, after.size() * 4);
    if (!write_words(output_path, after)) return 17;
    std::printf("dispatch_count=%ld skipped=%ld\n", x2w_d3d12_compute_dispatches(),
                x2w_d3d12_compute_skipped_dispatches());
    std::printf("before=");
    for (std::uint32_t i = 0; i < 8; ++i) std::printf("%08x", before[i]);
    std::printf(" after=");
    for (std::uint32_t i = 0; i < 8; ++i) std::printf("%08x", after[i]);
    std::printf("\n");
    return 0;
}
