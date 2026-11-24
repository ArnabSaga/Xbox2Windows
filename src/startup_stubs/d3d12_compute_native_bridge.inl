// Fixture-only bridge between the accepted revision-2 native cache consumer and the startup
// device wrapper (#1412, Stage 4C / N2C). Included inside d3d12_xs_device.cpp after the anonymous
// namespace closes, and only when X2W_NATIVE_COMPUTE_FIXTURE_BRIDGE is defined for the shim.
//
// Activation requires X2W_NATIVE_COMPUTE_FIXTURE=1 and one of the two approved golden keys. The
// bridge never parses production slot-67 descriptor memory, never dispatches and never translates
// or compiles. The slot-67 placeholder and its table row are untouched.
#if defined(X2W_NATIVE_COMPUTE_FIXTURE_BRIDGE)
#include "startup_stubs/d3d12_compute_native.h"

namespace {

// The fixture slot. 57/67/55/86/62 are taken by existing device paths; 162 is a diagnostic-only
// identity for this fixture and is not installed in any production table.
constexpr int native_fixture_path = 162;
constexpr std::uint64_t native_fixture_keys[] = {x2w_native::golden_key_1d,
                                                 x2w_native::golden_key_2d};

bool native_fixture_enabled()
{
    char value[4]{};
    return GetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", value, sizeof(value)) &&
           value[0] == '1';
}

bool native_fixture_approved(std::uint64_t key)
{
    for (std::uint64_t approved : native_fixture_keys)
        if (key == approved) return true;
    return false;
}

void native_copy_detail(const std::string& text, char* out, std::size_t size)
{
    if (!out || !size) return;
    const std::size_t count = text.size() < size - 1 ? text.size() : size - 1;
    std::memcpy(out, text.data(), count);
    out[count] = 0;
}

bool native_root_metadata_is_fixture(const std::vector<RootParameter>& parameters)
{
    if (parameters.size() != 3) return false;
    const D3D12_ROOT_PARAMETER_TYPE types[3] = {D3D12_ROOT_PARAMETER_TYPE_SRV,
                                                D3D12_ROOT_PARAMETER_TYPE_UAV,
                                                D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
    const UINT registers[3] = {0, 0, 0};
    const UINT counts[3] = {0, 0, 16};
    for (std::size_t i = 0; i < 3; ++i) {
        if (parameters[i].type != types[i] || parameters[i].descriptor_table ||
            parameters[i].shader_register != registers[i] || parameters[i].space != 0)
            return false;
        if (types[i] == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (parameters[i].constant_count != counts[i]) return false;
        } else if (parameters[i].constant_count != 0) {
            return false;
        }
    }
    return true;
}

// The exact execution contract of an accepted fixture pipeline. Derived from the accepted record at
// creation time; the Dispatch boundary reads it back so a caller cannot alter the expected
// dimensions, guard range or extents by rebinding.
struct NativeFixtureContract {
    Wrapper* device = nullptr;
    std::uint64_t key = 0;
    std::uint32_t profile = 0;
    std::uint32_t dispatch_x = 0, dispatch_y = 0, dispatch_z = 0;
    std::uint32_t bound_constant = 0, cap_constant = 0;
    std::uint32_t max_guard = 0;
    std::uint64_t source_bytes = 0;
    std::uint64_t output_bytes = 0;
};
std::unordered_map<const Wrapper*, NativeFixtureContract>& native_fixture_contracts()
{
    static std::unordered_map<const Wrapper*, NativeFixtureContract> contracts;
    return contracts;
}
SRWLOCK native_fixture_contract_lock = SRWLOCK_INIT;

// Checked extent test: offset + bytes must fit in width without wrapping. An oversized backing
// allocation therefore cannot hide an invalid binding offset.
bool native_fixture_extent_ok(std::uint64_t offset, std::uint64_t bytes, std::uint64_t width)
{
    if (offset > width) return false;
    return bytes <= width - offset;
}

// The fixture Dispatch boundary (#1412, Stage 4D / N3). Called from list_dispatch after the runtime
// resolved every root argument and before any state transition or desktop Dispatch. Returns true
// for every pipeline that is not the native fixture, so slot 62 and graphics are unchanged.
bool native_fixture_dispatch_check(Wrapper* list, const Pass& pass,
                                   const NativeFixtureBinding* srvs, UINT srv_count,
                                   const NativeFixtureBinding* uavs, UINT uav_count, UINT x,
                                   UINT y, UINT z, const char** reason)
{
    const auto fail = [reason](const char* text) {
        static LONG reported;
        if (first_calls(reported, 32)) log("x2w-native fixture Dispatch refused: %s", text);
        if (reason) *reason = text;
        return false;
    };
    const PipelineInfo* info = pass.pipeline ? pipeline_info(pass.pipeline) : nullptr;
    if (!info || info->path != native_fixture_path) return true;

    if (!native_fixture_enabled()) return fail("fixture activation is disabled");
    NativeFixtureContract contract;
    bool known = false;
    {
        AcquireSRWLockShared(&native_fixture_contract_lock);
        const auto found = native_fixture_contracts().find(pass.pipeline);
        if (found != native_fixture_contracts().end()) {
            contract = found->second;
            known = true;
        }
        ReleaseSRWLockShared(&native_fixture_contract_lock);
    }
    if (!known) return fail("the fixture pipeline has no registered execution contract");
    if (!native_fixture_approved(contract.key) || info->compute_key != contract.key)
        return fail("the pipeline identity is not the approved fixture identity");
    if (pass.pipeline_class != PipelineClass::compute || !pass.pipeline_real || !pass.pipeline ||
        !pass.pipeline->own().real)
        return fail("the fixture pipeline is not a real compute pipeline");
    if (!list || list->own().list_device != contract.device)
        return fail("the fixture pipeline and the command list belong to different devices");
    if (!pass.compute_root_real || info->compute_root != pass.compute_root_real)
        return fail("the bound root signature does not match the fixture PSO");
    if (pass.compute_root_params.size() != 3)
        return fail("the fixture root signature has the wrong parameter count");
    const RootParameter& srv_parameter = pass.compute_root_params[0];
    const RootParameter& uav_parameter = pass.compute_root_params[1];
    const RootParameter& constants_parameter = pass.compute_root_params[2];
    if (srv_parameter.type != D3D12_ROOT_PARAMETER_TYPE_SRV || srv_parameter.descriptor_table ||
        srv_parameter.shader_register != 0 || srv_parameter.space != 0 ||
        uav_parameter.type != D3D12_ROOT_PARAMETER_TYPE_UAV || uav_parameter.descriptor_table ||
        uav_parameter.shader_register != 0 || uav_parameter.space != 0 ||
        constants_parameter.type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
        constants_parameter.descriptor_table || constants_parameter.shader_register != 0 ||
        constants_parameter.space != 0 || constants_parameter.constant_count != 16)
        return fail("the bound root signature is not the fixture contract");
    if ((pass.compute_constants_set[2] & 0xFFFFull) != 0xFFFFull)
        return fail("the 16 fixture root constants are not fully initialized");
    if (x != contract.dispatch_x || y != contract.dispatch_y || z != contract.dispatch_z)
        return fail("the Dispatch dimensions do not match the approved record");
    const std::uint32_t bound = pass.compute_constants[2][contract.bound_constant];
    if (bound > contract.max_guard) return fail("the guard value is outside the approved profile");

    const NativeFixtureBinding* srv = nullptr;
    for (UINT i = 0; i < srv_count; ++i)
        if (srvs[i].parameter == 0) srv = &srvs[i];
    const NativeFixtureBinding* uav = nullptr;
    for (UINT i = 0; i < uav_count; ++i)
        if (uavs[i].parameter == 1) uav = &uavs[i];
    if (!srv || !uav) return fail("the fixture SRV or UAV is not bound");
    if (srv->resource == uav->resource)
        return fail("the same resource is bound as the fixture SRV and UAV");
    if (!srv->resource || !uav->resource) return fail("a fixture binding has no desktop resource");
    const D3D12_RESOURCE_DESC source_desc = srv->resource->GetDesc();
    const D3D12_RESOURCE_DESC output_desc = uav->resource->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        output_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return fail("the fixture bindings are not buffers");
    if (!(output_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return fail("the fixture UAV resource lacks unordered-access capability");
    const std::uint64_t source_base = srv->resource->GetGPUVirtualAddress();
    const std::uint64_t output_base = uav->resource->GetGPUVirtualAddress();
    if (srv->desktop < source_base || uav->desktop < output_base)
        return fail("a fixture binding is before its desktop resource");
    const std::uint64_t source_offset = srv->desktop - source_base;
    const std::uint64_t output_offset = uav->desktop - output_base;
    if ((source_offset & 3) || (output_offset & 3))
        return fail("a fixture binding is not DWORD-aligned");
    if (!native_fixture_extent_ok(source_offset, contract.source_bytes, source_desc.Width))
        return fail("the source binding does not cover the approved read extent");
    if (!native_fixture_extent_ok(output_offset, contract.output_bytes, output_desc.Width))
        return fail("the destination binding does not cover the approved write extent");
    return true;
}

}  // namespace

// Creates a real desktop root and compute PSO from an accepted cache entry, through the wrapper
// so the recorded root metadata and pipeline classification are the runtime's own. Output pointers
// are null on every failure.
extern "C" __declspec(dllexport) HRESULT x2w_native_fixture_create(
    void* device_guest, const wchar_t* cache_dir, const char* key_text, void** root_out,
    void** pso_out, int* state_out, char* detail, std::size_t detail_size)
{
    if (root_out) *root_out = nullptr;
    if (pso_out) *pso_out = nullptr;
    if (state_out) *state_out = -1;
    if (detail && detail_size) detail[0] = 0;
    if (!root_out || !pso_out) return E_POINTER;
    if (!device_guest) {
        native_copy_detail("no device", detail, detail_size);
        return E_POINTER;
    }
    if (!native_fixture_enabled()) {
        native_copy_detail("fixture activation is disabled", detail, detail_size);
        return E_ACCESSDENIED;
    }
    std::uint64_t key = 0;
    if (!key_text || !x2w_native::parse_key_text(key_text, key) || !native_fixture_approved(key)) {
        native_copy_detail("the key is not an approved fixture identity", detail, detail_size);
        return E_INVALIDARG;
    }
    Wrapper* device = as_wrapper(device_guest);
    if (!device || device->own().kind != device_kind || !device->own().real) {
        native_copy_detail("the device is not a wrapped desktop device", detail, detail_size);
        return E_INVALIDARG;
    }
    if (!cache_dir) {
        native_copy_detail("no cache directory", detail, detail_size);
        return E_INVALIDARG;
    }

    const x2w_native::Acceptance acceptance =
        x2w_native::inspect(cache_dir, key, {"warp", "hardware"});
    if (state_out) *state_out = x2w_native::state_code(acceptance.state);
    if (!acceptance.accepted()) {
        native_copy_detail(std::string("cache state ") + x2w_native::state_name(acceptance.state) +
                               ": " + acceptance.detail,
                           detail, detail_size);
        return S_FALSE;
    }

    std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
    std::string why;
    if (!x2w_native::dxbc_parts(acceptance.output.data(), acceptance.output.size(), parts, why) ||
        !parts.count("RTS0")) {
        native_copy_detail("the verified output has no usable root signature", detail, detail_size);
        return E_FAIL;
    }

    void* root_guest = nullptr;
    // D3D12 accepts the signed container and extracts the embedded signature; the wrapper records
    // the metadata through the same serialized input.
    const HRESULT root_hr =
        device_create_root_signature(device, 0, acceptance.output.data(), acceptance.output.size(),
                                     &__uuidof(ID3D12RootSignature), &root_guest);
    if (FAILED(root_hr) || !root_guest) {
        native_copy_detail("the wrapper rejected the embedded root signature", detail, detail_size);
        return FAILED(root_hr) ? root_hr : E_FAIL;
    }
    Wrapper* root = as_wrapper(root_guest);
    if (!root || root->own().kind != root_signature_kind || !root->own().real) {
        native_copy_detail("the wrapper root has no desktop object", detail, detail_size);
        return E_FAIL;
    }
    std::vector<RootParameter> parameters;
    if (!compute_parameters_for(root, parameters) || !native_root_metadata_is_fixture(parameters) ||
        compute_root_unsupported(parameters)) {
        native_copy_detail("the recorded root metadata is not the fixture contract", detail,
                           detail_size);
        release(root);
        return E_FAIL;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = static_cast<ID3D12RootSignature*>(root->own().real);
    desc.CS = {acceptance.output.data(), static_cast<UINT>(acceptance.output.size())};
    ID3D12PipelineState* pipeline = nullptr;
    const HRESULT pso_hr =
        device_of(device)->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(pso_hr) || !pipeline) {
        native_copy_detail("CreateComputePipelineState failed", detail, detail_size);
        if (pipeline) pipeline->Release();
        release(root);
        return FAILED(pso_hr) ? pso_hr : E_FAIL;
    }
    Wrapper* wrapped = wrap(pipeline, pipeline_kind);
    note_pipeline(wrapped, native_fixture_path, nullptr, nullptr, 0, nullptr, nullptr,
                  PipelineClass::compute, key, desc.pRootSignature);

    // The execution contract is derived once, from the accepted record. The Dispatch boundary reads
    // it back; rebinding cannot widen the allowed dimensions, guard or extents.
    const x2w_native::Record& record = acceptance.record;
    const std::uint64_t columns = std::uint64_t{record.dispatch_x} * record.group_x;
    const std::uint64_t rows = std::uint64_t{record.dispatch_y} * record.group_y;
    NativeFixtureContract contract;
    contract.device = device;
    contract.key = key;
    contract.profile = record.profile;
    contract.dispatch_x = record.dispatch_x;
    contract.dispatch_y = record.dispatch_y;
    contract.dispatch_z = record.dispatch_z;
    contract.bound_constant = record.bound_constant;
    contract.cap_constant = record.cap_constant;
    contract.max_guard = static_cast<std::uint32_t>(columns);
    contract.source_bytes = columns * record.element_bytes;
    contract.output_bytes = record.profile == x2w_native::profile_1d
                                ? columns * record.element_bytes
                                : rows * record.u0_stride;
    {
        AcquireSRWLockExclusive(&native_fixture_contract_lock);
        native_fixture_contracts()[wrapped] = contract;
        ReleaseSRWLockExclusive(&native_fixture_contract_lock);
    }

    native_copy_detail("fixture pipeline created", detail, detail_size);
    *root_out = root_guest;
    *pso_out = wrapped;
    return S_OK;
}

extern "C" __declspec(dllexport) void x2w_native_fixture_release(void* pipeline)
{
    Wrapper* wrapped = as_wrapper(pipeline);
    if (wrapped && wrapped->own().kind == pipeline_kind) {
        AcquireSRWLockExclusive(&native_fixture_contract_lock);
        native_fixture_contracts().erase(wrapped);
        ReleaseSRWLockExclusive(&native_fixture_contract_lock);
        release(wrapped);
    }
}

extern "C" __declspec(dllexport) void x2w_native_fixture_release_root(void* root)
{
    Wrapper* wrapped = as_wrapper(root);
    if (wrapped && wrapped->own().kind == root_signature_kind) release(wrapped);
}

// Reads the recorded classification for a fixture (or any) wrapped pipeline.
extern "C" __declspec(dllexport) int x2w_native_fixture_info(void* pipeline, int* path, int* cls,
                                                             unsigned long long* key)
{
    if (path) *path = -1;
    if (cls) *cls = -1;
    if (key) *key = 0;
    Wrapper* wrapped = as_wrapper(pipeline);
    if (!wrapped || wrapped->own().kind != pipeline_kind) return 0;
    const PipelineInfo* info = pipeline_info(wrapped);
    if (!info) return 0;
    if (path) *path = info->path;
    if (cls) *cls = static_cast<int>(info->cls);
    if (key) *key = info->compute_key;
    return 1;
}

// Reads the recorded root metadata for a wrapped root. Returns 0 when no parameter list was
// recorded (the wrapper keeps that distinguishable from a parameterless root).
extern "C" __declspec(dllexport) int x2w_native_fixture_root(
    void* root, unsigned int* types, unsigned int* registers, unsigned int* spaces,
    unsigned int* counts, unsigned int capacity, unsigned int* used)
{
    if (used) *used = 0;
    Wrapper* wrapped = as_wrapper(root);
    if (!wrapped || wrapped->own().kind != root_signature_kind) return 0;
    const std::vector<RootParameter>* parameters = root_info(wrapped);
    if (!parameters) return 0;
    const std::size_t count = parameters->size() < capacity ? parameters->size() : capacity;
    for (std::size_t i = 0; i < count; ++i) {
        if (types) types[i] = static_cast<unsigned int>((*parameters)[i].type);
        if (registers) registers[i] = (*parameters)[i].shader_register;
        if (spaces) spaces[i] = (*parameters)[i].space;
        if (counts) counts[i] = (*parameters)[i].constant_count;
    }
    if (used) *used = static_cast<unsigned int>(count);
    return 1;
}
extern "C" __declspec(dllexport) int x2w_native_fixture_render_mode()
{
    return render_mode() ? 1 : 0;
}

// The last reason the wrapper recorded for a refused Dispatch on this list. Test-only diagnostic;
// the production list path does not call it.
extern "C" __declspec(dllexport) int x2w_native_fixture_list_reason(void* list_guest, char* text,
                                                                  std::size_t size)
{
    if (text && size) text[0] = 0;
    Wrapper* list = as_wrapper(list_guest);
    if (!list || list->own().kind != list_kind) return 0;
    const Pass& pass = pass_of(list);
    if (!pass.compute_bind_error) return 0;
    native_copy_detail(pass.compute_bind_reason, text, size);
    return 1;
}

// Checked extent arithmetic used by the Dispatch boundary; exported so the test can exercise the
// overflow case with safe values instead of fabricating a near-2^64 resource.
extern "C" __declspec(dllexport) int x2w_native_fixture_extent_ok(unsigned long long offset,
                                                                unsigned long long bytes,
                                                                unsigned long long width)
{
    return native_fixture_extent_ok(offset, bytes, width) ? 1 : 0;
}

// Fixture-only readback recording: transition the dispatch destination to COPY_SOURCE and copy an
// exact byte range into a committed READBACK buffer. It records on the wrapper list's desktop
// object, is reachable only through this test export, and performs no Dispatch.
extern "C" __declspec(dllexport) HRESULT x2w_native_fixture_readback(
    void* list_guest, void* source_guest, unsigned long long source_offset, void* readback_guest,
    unsigned long long readback_offset, unsigned long long bytes, unsigned int before_state)
{
    // Fixture-only: refuse before touching the list, resources or wrapper state when activation is
    // off. The export performs no Dispatch, but it must still be inert outside fixture runs.
    if (!native_fixture_enabled()) return E_ACCESSDENIED;
    Wrapper* list = as_wrapper(list_guest);
    Wrapper* source = as_wrapper(source_guest);
    Wrapper* readback = as_wrapper(readback_guest);
    if (!list || list->own().kind != list_kind || !list->own().real) return E_INVALIDARG;
    if (!source || source->own().kind != resource_kind || !source->own().real) return E_INVALIDARG;
    if (!readback || readback->own().kind != resource_kind || !readback->own().real)
        return E_INVALIDARG;
    ID3D12Resource* source_real = static_cast<ID3D12Resource*>(source->own().real);
    ID3D12Resource* readback_real = static_cast<ID3D12Resource*>(readback->own().real);
    const D3D12_RESOURCE_DESC source_desc = source_real->GetDesc();
    const D3D12_RESOURCE_DESC readback_desc = readback_real->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        readback_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return E_INVALIDARG;
    if (!bytes || (bytes & 3) || (source_offset & 3) || (readback_offset & 3)) return E_INVALIDARG;
    if (!native_fixture_extent_ok(source_offset, bytes, source_desc.Width) ||
        !native_fixture_extent_ok(readback_offset, bytes, readback_desc.Width))
        return E_INVALIDARG;
    const D3D12_RESOURCE_STATES before = static_cast<D3D12_RESOURCE_STATES>(before_state);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE &&
        before != D3D12_RESOURCE_STATE_COMMON &&
        before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return E_INVALIDARG;
    auto* desktop = static_cast<ID3D12GraphicsCommandList*>(list->own().real);
    if (before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source_real;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        desktop->ResourceBarrier(1, &barrier);
        // Keep the wrapper's per-list tracking in step with the desktop state we just recorded, so
        // Close's return-to-COMMON uses the correct StateBefore.
        Pass& pass = pass_of(list);
        bool tracked = false;
        for (auto& known : pass.states) {
            if (known.first != source_real) continue;
            known.second = D3D12_RESOURCE_STATE_COPY_SOURCE;
            tracked = true;
        }
        if (!tracked) pass.states.emplace_back(source_real, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    desktop->CopyBufferRegion(readback_real, readback_offset, source_real, source_offset, bytes);
    return S_OK;
}

#endif  // X2W_NATIVE_COMPUTE_FIXTURE_BRIDGE
