// Experimental compute path for the startup runtime. Included inside d3d12_xs_device.cpp's
// anonymous namespace, after the graphics draw path. It implements only the supported subset:
// a signed, retargeted desktop compute container from X2W_SHADER_CACHE, root SRV/UAV arguments,
// root 32-bit constants and Dispatch. Descriptor tables and root CBVs are refused with a reason.

constexpr char compute_revision[] = "x2w-startup-compute-1";
LONG compute_pipelines_real, compute_pipelines_refused, compute_pipelines_requested, compute_dispatches,
    compute_dispatches_skipped;

#if defined(X2W_NATIVE_COMPUTE_FIXTURE_BRIDGE)
// Fixture-only Dispatch boundary (#1412, Stage 4D / N3). The definition lives in
// d3d12_compute_native_bridge.inl and returns true immediately for every pipeline whose
// PipelineInfo path is not the fixture identity, so the slot-62 path is unchanged.
struct NativeFixtureBinding {
    UINT parameter;
    ID3D12Resource* resource;
    std::uint64_t desktop;
};
bool native_fixture_dispatch_check(Wrapper* list, const Pass& pass,
                                   const NativeFixtureBinding* srvs, UINT srv_count,
                                   const NativeFixtureBinding* uavs, UINT uav_count, UINT x, UINT y,
                                   UINT z, const char** reason);
#endif

u64 compute_cache_key(const void* data, std::size_t bytes)
{
    return fnv_bytes(data, bytes, fnv_bytes(compute_revision, sizeof(compute_revision) - 1));
}

void digest_hex(const std::array<unsigned char, 32>& digest, char out[65])
{
    for (int i = 0; i < 32; ++i) std::snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

bool read_file_bytes(const wchar_t* path, std::vector<unsigned char>& out, std::size_t limit)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit) {
        CloseHandle(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    std::size_t offset = 0;
    while (offset < out.size() && ReadFile(file, out.data() + offset, static_cast<DWORD>(out.size() - offset), &got, nullptr) && got)
        offset += got;
    CloseHandle(file);
    return offset == out.size();
}

bool write_atomic_path(const wchar_t* path, const void* data, std::size_t bytes)
{
    static volatile LONG counter;
    wchar_t temporary[MAX_PATH];
    const int name = swprintf(temporary, MAX_PATH, L"%s.tmp.%lu.%ld", path, GetCurrentProcessId(),
                              InterlockedIncrement(&counter));
    if (name < 0 || name >= MAX_PATH) return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = bytes == 0 ||
        (WriteFile(file, data, static_cast<DWORD>(bytes), &written, nullptr) && written == bytes);
    if (complete) FlushFileBuffers(file);
    CloseHandle(file);
    if (complete && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temporary);
    return false;
}

std::string trim_line(std::string line)
{
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

struct ComputeManifest {
    bool present = false;
    bool valid = false;
    std::string status;
    std::string reason;
    std::string key;
    std::string revision;
    std::string source_sha;
    std::string output_sha;
    std::string pso_probe;
    unsigned long long source_bytes = 0;
    unsigned long long output_bytes = 0;
};

bool parse_manifest(const std::string& text, bool request, ComputeManifest& out)
{
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line)) return false;
    line = trim_line(line);
    if (line != (request ? "x2w-compute-request 1" : "x2w-compute-cache 1")) return false;
    bool have_key = false, have_revision = false, have_source = false, have_bytes = false;
    while (std::getline(input, line)) {
        line = trim_line(line);
        if (line.empty()) continue;
        const std::size_t space = line.find(' ');
        const std::string field = line.substr(0, space);
        const std::string value = space == std::string::npos ? std::string{} : line.substr(space + 1);
        if (field == "key") { out.key = value; have_key = true; }
        else if (field == "revision") { out.revision = value; have_revision = true; }
        else if (field == "source_sha256") { out.source_sha = value; have_source = true; }
        else if (field == "source_bytes") { out.source_bytes = std::strtoull(value.c_str(), nullptr, 10); have_bytes = true; }
        else if (!request && field == "status") out.status = value;
        else if (!request && field == "reason") out.reason = value;
        else if (!request && field == "output_sha256") out.output_sha = value;
        else if (!request && field == "output_bytes") out.output_bytes = std::strtoull(value.c_str(), nullptr, 10);
        else if (!request && field == "pso_probe") out.pso_probe = value;
    }
    out.present = true;
    out.valid = have_key && have_revision && have_source && have_bytes;
    if (!request) out.valid = out.valid && (out.status == "signed" || out.status == "refused");
    return out.valid;
}

bool request_manifest_matches(const std::wstring& manifest_path, u64 key, const std::vector<unsigned char>& source)
{
    std::vector<unsigned char> text;
    if (!read_file_bytes(manifest_path.c_str(), text, 1u << 16)) return false;
    ComputeManifest manifest;
    if (!parse_manifest(std::string(text.begin(), text.end()), true, manifest)) return false;
    char expected_key[17], source_sha[65];
    std::snprintf(expected_key, sizeof(expected_key), "%016llx", static_cast<unsigned long long>(key));
    digest_hex(x2w_startup::sha256(source.data(), source.size()), source_sha);
    return manifest.key == expected_key && manifest.revision == compute_revision &&
           manifest.source_bytes == source.size() && manifest.source_sha == source_sha;
}

bool publish_compute_request(u64 key, const std::vector<unsigned char>& source)
{
    wchar_t raw[MAX_PATH], manifest_path[MAX_PATH];
    if (!cache_path(raw, L"req_", key, L".cs") || !cache_path(manifest_path, L"", key, L".cs.src")) return false;
    if (GetFileAttributesW(raw) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(manifest_path) != INVALID_FILE_ATTRIBUTES &&
        request_manifest_matches(manifest_path, key, source)) return true;
    if (!write_atomic_path(raw, source.data(), source.size())) return false;
    char source_sha[65];
    digest_hex(x2w_startup::sha256(source.data(), source.size()), source_sha);
    char text[512];
    const int used = std::snprintf(text, sizeof(text),
        "x2w-compute-request 1\nkey %016llx\nrevision %s\nsource_bytes %llu\nsource_sha256 %s\n",
        static_cast<unsigned long long>(key), compute_revision,
        static_cast<unsigned long long>(source.size()), source_sha);
    return used > 0 && used < static_cast<int>(sizeof(text)) &&
           write_atomic_path(manifest_path, text, static_cast<std::size_t>(used));
}

// Root parameters of a wrapped root signature, from the recorded list or the serialized blob.
// Both maps are shared with creation threads; copy under the same lock as the writer and parse the
// blob copy outside the critical section.
bool compute_parameters_for(Wrapper* wrapped, std::vector<RootParameter>& parameters)
{
    if (!wrapped) return false;
    {
        AcquireSRWLockShared(&root_lock);
        const auto found = root_infos().find(wrapped);
        if (found != root_infos().end()) {
            parameters = found->second;
            ReleaseSRWLockShared(&root_lock);
            return true;
        }
        ReleaseSRWLockShared(&root_lock);
    }
    std::vector<unsigned char> blob;
    {
        AcquireSRWLockShared(&note_lock);
        const auto found = root_blobs().find(wrapped);
        if (found != root_blobs().end()) blob = found->second;
        ReleaseSRWLockShared(&note_lock);
    }
    if (blob.empty()) return false;
    return read_root_parameters(blob.data(), blob.size(), parameters);
}

// An empty parameter list is a valid root signature; callers refuse only when the list could not
// be inspected at all.
const char* compute_root_unsupported(const std::vector<RootParameter>& parameters)
{
    if (parameters.size() > x2w_compute::max_root_parameters) return "more than 32 root parameters";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const RootParameter& parameter = parameters[i];
        if (parameter.descriptor_table) return "descriptor table parameters are not supported in this slice";
        if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_CBV) return "root CBV parameters are not supported in this slice";
        if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (parameter.constant_count == 0 || parameter.constant_count > x2w_compute::max_root_constants)
                return "a root constants parameter declares an unsupported number of values";
        } else if (parameter.type != D3D12_ROOT_PARAMETER_TYPE_SRV &&
                   parameter.type != D3D12_ROOT_PARAMETER_TYPE_UAV) {
            return "an unsupported root parameter type";
        }
    }
    return nullptr;
}

bool has_container_digest(const std::vector<unsigned char>& container)
{
    if (container.size() < 20) return false;
    for (std::size_t i = 4; i < 20; ++i)
        if (container[i]) return true;
    return false;
}

// Creates a real desktop compute PSO from a signed cache entry. Returns S_OK with *out set, or
// S_FALSE when the pipeline must stay a placeholder. The caller owns the placeholder decision.
HRESULT create_compute_pipeline_cached(Wrapper* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, void** out)
{
    *out = nullptr;
    if (!render_mode()) return S_FALSE;
    if (!desc || !desc->CS.pShaderBytecode || desc->CS.BytecodeLength < 32 ||
        desc->CS.BytecodeLength > (64u << 20) ||
        !x2w_local::readable(desc->CS.pShaderBytecode, desc->CS.BytecodeLength) ||
        std::memcmp(desc->CS.pShaderBytecode, "DXBC", 4) != 0)
        return S_FALSE;
    std::vector<unsigned char> source(static_cast<const unsigned char*>(desc->CS.pShaderBytecode),
                                      static_cast<const unsigned char*>(desc->CS.pShaderBytecode) + desc->CS.BytecodeLength);
    const u64 key = compute_cache_key(source.data(), source.size());
    char key_text[17];
    std::snprintf(key_text, sizeof(key_text), "%016llx", static_cast<unsigned long long>(key));
    char source_sha[65];
    digest_hex(x2w_startup::sha256(source.data(), source.size()), source_sha);

    // A null root signature cannot be matched to the root the title later binds, so this slice
    // cannot dispatch it. Refuse instead of caching a fallback under a shared null key.
    if (!desc->pRootSignature) {
        InterlockedIncrement(&compute_pipelines_refused);
        log("x2w-compute %s: placeholder: the pipeline has no root signature", key_text);
        return S_FALSE;
    }

    wchar_t raw_request[MAX_PATH], request_manifest[MAX_PATH], cache_manifest[MAX_PATH], cache_output[MAX_PATH], error_path[MAX_PATH];
    if (!cache_path(cache_manifest, L"", key, L".cs.manifest") || !cache_path(cache_output, L"", key, L".cs.cso") ||
        !cache_path(error_path, L"", key, L".cs.error")) {
        static LONG no_cache;
        if (first_calls(no_cache, 2))
            log("x2w-compute %s: X2W_SHADER_CACHE is not set; pipeline stays placeholder", key_text);
        return S_FALSE;
    }

    ComputeManifest manifest;
    std::vector<unsigned char> manifest_text;
    if (read_file_bytes(cache_manifest, manifest_text, 1u << 16)) {
        if (parse_manifest(std::string(manifest_text.begin(), manifest_text.end()), false, manifest) &&
            manifest.key == key_text && manifest.revision == compute_revision &&
            manifest.source_bytes == source.size() && manifest.source_sha == source_sha) {
            if (manifest.status == "refused") {
                InterlockedIncrement(&compute_pipelines_refused);
                log("x2w-compute %s: placeholder: refused: %s", key_text, manifest.reason.c_str());
                return S_FALSE;
            }
            if (manifest.pso_probe.rfind("FAILED", 0) == 0) {
                InterlockedIncrement(&compute_pipelines_refused);
                log("x2w-compute %s: placeholder: cache entry reports a failed PSO probe", key_text);
                return S_FALSE;
            }
            std::vector<unsigned char> cso;
            if (read_file_bytes(cache_output, cso, 64u << 20) && !cso.empty()) {
                char output_sha[65];
                digest_hex(x2w_startup::sha256(cso.data(), cso.size()), output_sha);
                const bool hash_ok = cso.size() >= 32 && manifest.output_bytes == cso.size() &&
                                     manifest.output_sha == output_sha &&
                                     std::memcmp(cso.data(), "DXBC", 4) == 0;
                if (hash_ok && !has_container_digest(cso)) {
                    InterlockedIncrement(&compute_pipelines_refused);
                    log("x2w-compute %s: placeholder: signed output has a zero DXBC digest", key_text);
                    return S_FALSE;
                }
                if (hash_ok && has_container_digest(cso)) {
                    // The signed container is authoritative. This slice uses only the root
                    // signature the title supplied: a fallback derived from the container would
                    // need lifetime-safe identity that this slice does not provide.
                    Wrapper* wrapped_root = as_wrapper(desc->pRootSignature);
                    if (!wrapped_root || wrapped_root->own().kind != root_signature_kind || !wrapped_root->own().real) {
                        InterlockedIncrement(&compute_pipelines_refused);
                        log("x2w-compute %s: placeholder: the supplied root signature is not a wrapped desktop root",
                            key_text);
                        return S_FALSE;
                    }
                    ID3D12RootSignature* root = static_cast<ID3D12RootSignature*>(wrapped_root->own().real);
                    std::vector<RootParameter> parameters;
                    if (!compute_parameters_for(wrapped_root, parameters)) {
                        InterlockedIncrement(&compute_pipelines_refused);
                        log("x2w-compute %s: placeholder: root signature parameters could not be inspected", key_text);
                        return S_FALSE;
                    }
                    const char* unsupported = compute_root_unsupported(parameters);
                    if (unsupported) {
                        InterlockedIncrement(&compute_pipelines_refused);
                        log("x2w-compute %s: placeholder: %s", key_text, unsupported);
                        return S_FALSE;
                    }
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desktop = *desc;
                    desktop.pRootSignature = root;
                    desktop.CS = {cso.data(), static_cast<UINT>(cso.size())};
                    desktop.CachedPSO = {};
                    ID3D12PipelineState* pipeline = nullptr;
                    const HRESULT hr = device_of(self)->CreateComputePipelineState(&desktop, IID_PPV_ARGS(&pipeline));
                    if (SUCCEEDED(hr) && pipeline) {
                        Wrapper* w = wrap(pipeline, pipeline_kind);
                        note_pipeline(w, 62, nullptr, desc, sizeof(*desc), nullptr, nullptr,
                                      PipelineClass::compute, key, root);
                        InterlockedIncrement(&compute_pipelines_real);
                        log("x2w-compute %s: real desktop compute PSO, %zu signed bytes, root=%p", key_text, cso.size(),
                            static_cast<void*>(root));
                        *out = w;
                        return S_OK;
                    }
                    check_device("Device CreateComputePipelineState", 62);
                    log("x2w-compute %s: placeholder: CreateComputePipelineState failed 0x%08lx", key_text,
                        static_cast<unsigned long>(hr));
                    return S_FALSE;
                }
                InterlockedIncrement(&compute_pipelines_refused);
                log("x2w-compute %s: placeholder: signed output hash or size mismatch", key_text);
                return S_FALSE;
            }
            InterlockedIncrement(&compute_pipelines_refused);
            log("x2w-compute %s: placeholder: signed output missing", key_text);
            return S_FALSE;
        }
        // A manifest for another key, revision or source cannot be trusted. Ignore it and publish a
        // fresh request so the offline tool replaces it; this is the stale-cache invalidation path.
        log("x2w-compute %s: stale or mismatched cache manifest ignored; publishing a new request", key_text);
    }

    // A tool-side infrastructure failure is not a shader refusal: keep the request and retry later.
    std::vector<unsigned char> error_text;
    if (read_file_bytes(error_path, error_text, 1u << 16)) {
        log("x2w-compute %s: placeholder: retryable: %.*s", key_text,
            static_cast<int>(error_text.size()), reinterpret_cast<const char*>(error_text.data()));
        return S_FALSE;
    }

    if (!cache_path(raw_request, L"req_", key, L".cs") || !cache_path(request_manifest, L"", key, L".cs.src"))
        return S_FALSE;
    if (!publish_compute_request(key, source)) {
        log("x2w-compute %s: placeholder: could not publish the cache request", key_text);
        return S_FALSE;
    }
    InterlockedIncrement(&compute_pipelines_requested);
    log("x2w-compute %s: placeholder: request written to %ls", key_text, raw_request);
    return S_FALSE;
}

void compute_bind_failure(Pass& pass, const char* reason)
{
    pass.compute_bind_error = true;
    if (!pass.compute_bind_reason[0]) std::snprintf(pass.compute_bind_reason, sizeof(pass.compute_bind_reason), "%s", reason);
}

// List slot 33 (desktop 29 + 4), SetComputeRootSignature.
void __stdcall list_compute_root_signature(Wrapper* self, void* root)
{
    Pass& pass = pass_of(self);
    if (!root) {
        clear_compute_bindings(pass);
        compute_bind_failure(pass, "no real compute root signature is bound");
        return;
    }
    Wrapper* w = as_wrapper(root);
    if (!w || w->own().kind != root_signature_kind || !w->own().real) {
        clear_compute_bindings(pass);
        compute_bind_failure(pass, "compute root signature has no desktop object");
        return;
    }
    ID3D12RootSignature* real = static_cast<ID3D12RootSignature*>(w->own().real);
    std::vector<RootParameter> parameters;
    const bool parameters_known = compute_parameters_for(w, parameters);
    const char* unsupported = compute_root_unsupported(parameters);
    // Clear before recording the failure: a root change normally clears the pass state, and the
    // failure must survive it. The sticky flag keeps Dispatch refused even after a pipeline-class
    // change clears the per-call arguments.
    if (pass.compute_root_real != real) clear_compute_bindings(pass);
    if (!parameters_known) compute_bind_failure(pass, "compute root signature parameters could not be inspected");
    else if (unsupported) compute_bind_failure(pass, unsupported);
    pass.compute_root_unsupported = !parameters_known || unsupported != nullptr;
    pass.compute_root_real = real;
    // Only a supported list becomes active state; an oversized or structurally unsupported root
    // keeps no parameter list at all, so no fixed array can be indexed through it.
    if (pass.compute_root_unsupported)
        pass.compute_root_params.clear();
    else
        pass.compute_root_params = std::move(parameters);
    if (auto* list = recording(self)) list->SetComputeRootSignature(real);
}

// List slot 37 (desktop 33 + 4), SetComputeRoot32BitConstant(parameter, value, offset).
void __stdcall list_compute_root_constant(Wrapper* self, UINT parameter, UINT value, UINT offset)
{
    Pass& pass = pass_of(self);
    if (!recording(self) || parameter >= x2w_compute::max_root_parameters ||
        parameter >= pass.compute_root_params.size() || offset >= x2w_compute::max_root_constants) {
        compute_bind_failure(pass, "invalid SetComputeRoot32BitConstant arguments");
        return;
    }
    const RootParameter& info = pass.compute_root_params[parameter];
    if (info.type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS || offset >= info.constant_count) {
        compute_bind_failure(pass, "root constant offset exceeds the declared count");
        return;
    }
    pass.compute_constants[parameter][offset] = value;
    pass.compute_constants_set[parameter] |= std::uint64_t{1} << offset;
}

// List slot 39 (desktop 35 + 4), SetComputeRoot32BitConstants(parameter, count, data, offset).
void __stdcall list_compute_root_constants(Wrapper* self, UINT parameter, UINT count, const void* data, UINT offset)
{
    Pass& pass = pass_of(self);
    if (!recording(self) || !count || parameter >= x2w_compute::max_root_parameters ||
        parameter >= pass.compute_root_params.size() || offset >= x2w_compute::max_root_constants) {
        compute_bind_failure(pass, "invalid SetComputeRoot32BitConstants arguments");
        return;
    }
    const RootParameter& info = pass.compute_root_params[parameter];
    if (info.type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS || offset > info.constant_count ||
        count > info.constant_count - offset) {
        compute_bind_failure(pass, "root constant range exceeds the declared count");
        return;
    }
    // Bounds are established before this probe: count is now at most the declared constant count.
    if (!data || !x2w_local::readable(data, std::size_t{count} * sizeof(std::uint32_t))) {
        compute_bind_failure(pass, "invalid SetComputeRoot32BitConstants arguments");
        return;
    }
    std::memcpy(pass.compute_constants[parameter] + offset, data, std::size_t{count} * sizeof(std::uint32_t));
    for (UINT i = 0; i < count; ++i) pass.compute_constants_set[parameter] |= std::uint64_t{1} << (offset + i);
}

// List slot 43 (desktop 39 + 4), SetComputeRootShaderResourceView(parameter, address).
void __stdcall list_compute_root_srv(Wrapper* self, UINT parameter, u64 address)
{
    Pass& pass = pass_of(self);
    if (!recording(self) || parameter >= pass.compute_root_params.size() ||
        parameter >= x2w_compute::max_root_parameters ||
        pass.compute_root_params[parameter].type != D3D12_ROOT_PARAMETER_TYPE_SRV || !address) {
        compute_bind_failure(pass, "invalid SetComputeRootShaderResourceView arguments");
        return;
    }
    pass.compute_srv_set[parameter] = true;
    pass.compute_srv_address[parameter] = address;
}

// List slot 45 (desktop 41 + 4), SetComputeRootUnorderedAccessView(parameter, address).
void __stdcall list_compute_root_uav(Wrapper* self, UINT parameter, u64 address)
{
    Pass& pass = pass_of(self);
    if (!recording(self) || parameter >= pass.compute_root_params.size() ||
        parameter >= x2w_compute::max_root_parameters ||
        pass.compute_root_params[parameter].type != D3D12_ROOT_PARAMETER_TYPE_UAV || !address) {
        compute_bind_failure(pass, "invalid SetComputeRootUnorderedAccessView arguments");
        return;
    }
    pass.compute_uav_set[parameter] = true;
    pass.compute_uav_address[parameter] = address;
}

// List slot 41 (desktop 37 + 4), SetComputeRootConstantBufferView. Deferred in this slice.
void __stdcall list_compute_root_cbv(Wrapper* self, UINT, u64)
{
    compute_bind_failure(pass_of(self), "root CBV parameters are not supported in this slice");
}

// List slot 35 (desktop 31 + 4), SetComputeRootDescriptorTable. Descriptor tables are deferred.
void __stdcall list_compute_root_table(Wrapper* self, UINT, u64)
{
    compute_bind_failure(pass_of(self), "compute descriptor-table binding is not supported in this slice");
}

// Like require_state, but the caller supplies the state the resource is in when it is first seen.
// A placed buffer may have been created directly in UNORDERED_ACCESS.
void require_state_from(Wrapper* self, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    Pass& pass = pass_of(self);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->own().real);
    for (auto& known : pass.states) {
        if (known.first != resource) continue;
        if (known.second != to) {
            transition(list, resource, known.second, to);
            known.second = to;
        }
        return;
    }
    transition(list, resource, from, to);
    pass.states.emplace_back(resource, to);
}

std::unordered_set<ID3D12Resource*>& retained_compute_aliases()
{
    static std::unordered_set<ID3D12Resource*> aliases;
    return aliases;
}
SRWLOCK compute_retained_lock = SRWLOCK_INIT;
unsigned compute_upload_reservations = 0;  // guarded by compute_retained_lock

// The slice keeps 1024 retained uploads. A test can lower the bound to exercise the boundary
// deterministically; it can never raise it above the production capacity.
unsigned compute_upload_limit()
{
    static const unsigned limit = [] {
        char value[16]{};
        const DWORD length = GetEnvironmentVariableA("X2W_TEST_COMPUTE_UPLOAD_LIMIT", value, sizeof(value));
        if (!length || length >= sizeof(value)) return x2w_compute::max_retained_uploads;
        const unsigned long parsed = std::strtoul(value, nullptr, 10);
        if (!parsed) return x2w_compute::max_retained_uploads;
        return parsed < x2w_compute::max_retained_uploads ? static_cast<unsigned>(parsed)
                                                          : x2w_compute::max_retained_uploads;
    }();
    return limit;
}

// Resolves a title GPU address to an aliased, device-owned placed buffer.
bool resolve_compute_alias(Wrapper* self, u64 address, bool unordered, ID3D12Resource** resource,
                           u64* desktop_address, D3D12_RESOURCE_STATES* initial_state, char* why,
                           std::size_t why_bytes)
{
    PlacedVertexView placed = placed_buffer_range(self->own().list_device, address, 4);
    if (placed.reject) {
        std::snprintf(why, why_bytes, "title address 0x%llx is ambiguous across placed buffers", address);
        return false;
    }
    if (!placed.owner || !placed.alias) {
        std::snprintf(why, why_bytes, "title address 0x%llx has no aliased placed buffer", address);
        return false;
    }
    const u64 base = reinterpret_cast<u64>(placed.owner->own().title_memory);
    if (address < base) {
        std::snprintf(why, why_bytes, "title address 0x%llx is before its placed buffer", address);
        return false;
    }
    const u64 offset = address - base;
    const D3D12_RESOURCE_DESC desc = placed.alias->GetDesc();
    if (unordered && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        std::snprintf(why, why_bytes, "placed buffer at 0x%llx lacks unordered-access capability", address);
        return false;
    }
    if (offset >= desc.Width || desc.Width - offset < 4 || (offset & 3)) {
        std::snprintf(why, why_bytes, "title address 0x%llx is outside its placed buffer or not DWORD-aligned", address);
        return false;
    }
    const u64 desktop = placed.alias->GetGPUVirtualAddress() + offset;
    if (!desktop || (desktop & 3)) {
        std::snprintf(why, why_bytes, "desktop address for title 0x%llx is not a DWORD-aligned GPU address", address);
        return false;
    }
    AcquireSRWLockExclusive(&compute_retained_lock);
    if (retained_compute_aliases().insert(placed.alias).second)
        placed.alias->AddRef();  // Held for the life of the process; the wrapper owns the original.
    ReleaseSRWLockExclusive(&compute_retained_lock);
    *resource = placed.alias;
    *desktop_address = desktop;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (const ResourceInfo* known = resource_info(placed.owner)) state = known->initial_state;
    *initial_state = state;
    return true;
}

std::vector<x2w_startup::ComPtr<ID3D12Resource>>& retained_compute_uploads()
{
    static std::vector<x2w_startup::ComPtr<ID3D12Resource>> uploads;
    return uploads;
}

bool upload_compute_srv(Wrapper* self, u64 address, ID3D12Resource** resource, u64* desktop_address, char* why,
                        std::size_t why_bytes)
{
    const std::size_t bytes = readable_window(address, 0x10000);
    if (!bytes) {
        std::snprintf(why, why_bytes, "title address 0x%llx is not readable", address);
        return false;
    }
    const unsigned limit = compute_upload_limit();
    // Check and reserve in one critical section, so concurrent recordings cannot both pass the
    // check and exceed the capacity. The guard releases a reservation on every failure path,
    // including an exception from vector insertion.
    struct Reservation {
        bool held = true;
        ~Reservation()
        {
            if (!held) return;
            AcquireSRWLockExclusive(&compute_retained_lock);
            --compute_upload_reservations;
            ReleaseSRWLockExclusive(&compute_retained_lock);
        }
    } reservation;
    AcquireSRWLockExclusive(&compute_retained_lock);
    const bool uploads_full = retained_compute_uploads().size() + compute_upload_reservations >= limit;
    if (uploads_full) reservation.held = false;
    else ++compute_upload_reservations;
    ReleaseSRWLockExclusive(&compute_retained_lock);
    if (uploads_full) {
        std::snprintf(why, why_bytes, "the retained compute upload limit (%u) is reached", limit);
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    x2w_startup::ComPtr<ID3D12Resource> upload;
    Wrapper* device = self->own().list_device ? self->own().list_device : self;
    if (FAILED(device_of(device)->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr, IID_PPV_ARGS(&upload)))) {
        std::snprintf(why, why_bytes, "could not allocate an upload buffer for title 0x%llx", address);
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) {
        std::snprintf(why, why_bytes, "could not map the upload buffer for title 0x%llx", address);
        return false;
    }
    std::memcpy(mapped, reinterpret_cast<const void*>(address), bytes);
    upload->Unmap(0, nullptr);
    ID3D12Resource* retained = upload.Get();
    const u64 gpu_address = upload->GetGPUVirtualAddress();
    AcquireSRWLockExclusive(&compute_retained_lock);
    try {
        retained_compute_uploads().push_back(std::move(upload));
    } catch (...) {
        ReleaseSRWLockExclusive(&compute_retained_lock);
        throw;
    }
    --compute_upload_reservations;
    reservation.held = false;
    ReleaseSRWLockExclusive(&compute_retained_lock);
    // Publish the outputs only after retained ownership exists.
    *resource = retained;
    *desktop_address = gpu_address;
    return true;
}

// Records and reports a Dispatch the runtime refuses. The reason is logged once per failure kind.
void dispatch_refused(Pass& pass, const char* reason, UINT x, UINT y, UINT z)
{
    compute_bind_failure(pass, reason);
    static LONG reported;
    if (first_calls(reported, 8)) log("x2w-compute Dispatch(%u, %u, %u) skipped: %s", x, y, z, reason);
    InterlockedIncrement(&compute_dispatches_skipped);
}

// List slot 14, Dispatch(x, y, z). Runs only when a real compute PSO, a matching compute root
// signature and every declared root argument are present.
void __stdcall list_dispatch(Wrapper* self, UINT x, UINT y, UINT z)
{
    Pass& pass = pass_of(self);
    auto* list = recording(self);
    if (!list || pass.pipeline_class != PipelineClass::compute || !pass.pipeline_real)
        return dispatch_refused(pass, pass.pipeline_class == PipelineClass::compute
                                          ? "no real compute pipeline is bound"
                                          : "Dispatch requires a compute pipeline", x, y, z);
    if (!pass.compute_root_real) return dispatch_refused(pass, "no real compute root signature is bound", x, y, z);
    if (pass.compute_bind_error) return dispatch_refused(pass, pass.compute_bind_reason, x, y, z);
    if (pass.compute_root_unsupported)
        return dispatch_refused(pass, "the bound compute root signature is not supported", x, y, z);
    if (pass.compute_required_root && pass.compute_required_root != pass.compute_root_real)
        return dispatch_refused(pass, "the bound compute root signature does not match the PSO", x, y, z);
    const PipelineInfo* pipeline = pipeline_info(pass.pipeline);
    if (pipeline && pipeline->compute_root && pipeline->compute_root != pass.compute_root_real)
        return dispatch_refused(pass, "the bound compute root signature does not match the PSO", x, y, z);
    if (!x || !y || !z) return dispatch_refused(pass, "a Dispatch dimension is zero", x, y, z);
    // Defence in depth: even if binding state were somehow larger, the fixed arrays below are
    // bounded by this one constant.
    if (pass.compute_root_params.size() > x2w_compute::max_root_parameters)
        return dispatch_refused(pass, "the bound compute root signature has too many parameters", x, y, z);

    struct ResolvedUav { UINT parameter; ID3D12Resource* resource; u64 desktop; D3D12_RESOURCE_STATES initial; };
    struct ResolvedSrv { UINT parameter; ID3D12Resource* resource; u64 desktop; bool aliased; D3D12_RESOURCE_STATES initial; };
    ResolvedUav uavs[x2w_compute::max_root_parameters]{};
    ResolvedSrv srvs[x2w_compute::max_root_parameters]{};
    UINT uav_count = 0, srv_count = 0;
    char why[192];
    for (UINT i = 0; i < pass.compute_root_params.size(); ++i) {
        if (i >= x2w_compute::max_root_parameters)
            return dispatch_refused(pass, "the bound compute root signature has too many parameters", x, y, z);
        const RootParameter& parameter = pass.compute_root_params[i];
        if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            const std::uint64_t required = parameter.constant_count >= x2w_compute::max_root_constants
                ? ~std::uint64_t{0}
                : ((std::uint64_t{1} << parameter.constant_count) - 1);
            if ((pass.compute_constants_set[i] & required) != required) {
                std::snprintf(why, sizeof(why), "root constants parameter %u is not fully initialized", i);
                return dispatch_refused(pass, why, x, y, z);
            }
        } else if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
            if (!pass.compute_srv_set[i]) {
                std::snprintf(why, sizeof(why), "root SRV parameter %u was never set", i);
                return dispatch_refused(pass, why, x, y, z);
            }
            ID3D12Resource* resource = nullptr;
            u64 desktop = 0;
            D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
            const bool aliased = resolve_compute_alias(self, pass.compute_srv_address[i], false, &resource, &desktop,
                                                       &initial, why, sizeof(why));
            if (!aliased && !upload_compute_srv(self, pass.compute_srv_address[i], &resource, &desktop, why, sizeof(why)))
                return dispatch_refused(pass, why, x, y, z);
            srvs[srv_count++] = {i, resource, desktop, aliased, initial};
        } else if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
            if (!pass.compute_uav_set[i]) {
                std::snprintf(why, sizeof(why), "root UAV parameter %u was never set", i);
                return dispatch_refused(pass, why, x, y, z);
            }
            ID3D12Resource* resource = nullptr;
            u64 desktop = 0;
            D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
            if (!resolve_compute_alias(self, pass.compute_uav_address[i], true, &resource, &desktop, &initial, why,
                                       sizeof(why)))
                return dispatch_refused(pass, why, x, y, z);
            uavs[uav_count++] = {i, resource, desktop, initial};
        } else {
            return dispatch_refused(pass, "an unsupported root parameter reached Dispatch", x, y, z);
        }
    }

    // One resource cannot be both a root SRV and a root UAV in the same dispatch: the two state
    // transitions would contradict each other and the read would see the wrong layout.
    for (UINT u = 0; u < uav_count; ++u)
        for (UINT s = 0; s < srv_count; ++s)
            if (uavs[u].resource == srvs[s].resource)
                return dispatch_refused(pass, "the same resource is bound as both root SRV and root UAV", x, y, z);

#if defined(X2W_NATIVE_COMPUTE_FIXTURE_BRIDGE)
    {
        NativeFixtureBinding srv_bindings[x2w_compute::max_root_parameters]{};
        NativeFixtureBinding uav_bindings[x2w_compute::max_root_parameters]{};
        for (UINT i = 0; i < srv_count; ++i)
            srv_bindings[i] = {srvs[i].parameter, srvs[i].resource, srvs[i].desktop};
        for (UINT i = 0; i < uav_count; ++i)
            uav_bindings[i] = {uavs[i].parameter, uavs[i].resource, uavs[i].desktop};
        const char* fixture_reason = nullptr;
        if (!native_fixture_dispatch_check(self, pass, srv_bindings, srv_count, uav_bindings,
                                           uav_count, x, y, z, &fixture_reason))
            return dispatch_refused(pass,
                                    fixture_reason ? fixture_reason : "fixture Dispatch refused",
                                    x, y, z);
    }
#endif

    for (UINT i = 0; i < srv_count; ++i) {
        if (srvs[i].aliased)
            require_state_from(self, srvs[i].resource, srvs[i].initial, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootShaderResourceView(srvs[i].parameter, srvs[i].desktop);
    }
    for (UINT i = 0; i < uav_count; ++i) {
        require_state_from(self, uavs[i].resource, uavs[i].initial, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootUnorderedAccessView(uavs[i].parameter, uavs[i].desktop);
    }
    for (UINT i = 0; i < pass.compute_root_params.size(); ++i) {
        const RootParameter& parameter = pass.compute_root_params[i];
        if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            list->SetComputeRoot32BitConstants(i, parameter.constant_count, pass.compute_constants[i], 0);
    }
    for (UINT i = 0; i < uav_count; ++i) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = uavs[i].resource;
        list->ResourceBarrier(1, &barrier);
    }
    list->Dispatch(x, y, z);
    const LONG total = InterlockedIncrement(&compute_dispatches);
    if (total <= 8 || (total & (total - 1)) == 0)
        log("x2w-compute Dispatch(%u, %u, %u) recorded; total=%ld", x, y, z, total);
}
