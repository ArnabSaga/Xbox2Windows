// Bounded C++ consumer of the accepted revision-2 native compute cache (#1412, Stage 4C / N2).
// Mirrors tools/startup/compute_native.py: parse, validate, identity, manifest schema, the one
// acceptance decision, the embedded root contract, and desktop root/PSO creation. No translation,
// no compilation, no dispatch, no production descriptor parsing.
#include "startup_stubs/d3d12_compute_native.h"

#include "startup_stubs/sha256.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

namespace x2w_native {
namespace {

constexpr char magic[4] = {'X', '2', 'W', 'N'};
constexpr std::uint64_t fnv_basis = 1469598103934665603ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

std::uint32_t read_u32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

void write_u32(unsigned char* data, std::uint32_t value)
{
    data[0] = static_cast<unsigned char>(value);
    data[1] = static_cast<unsigned char>(value >> 8);
    data[2] = static_cast<unsigned char>(value >> 16);
    data[3] = static_cast<unsigned char>(value >> 24);
}

bool trim_ascii(const char* data, std::size_t size, std::string& out)
{
    std::size_t first = 0, last = size;
    while (first < last && (data[first] == ' ' || data[first] == '\t' || data[first] == '\r' ||
                            data[first] == '\n' || data[first] == '\f' || data[first] == '\v'))
        ++first;
    while (last > first && (data[last - 1] == ' ' || data[last - 1] == '\t' ||
                            data[last - 1] == '\r' || data[last - 1] == '\n' ||
                            data[last - 1] == '\f' || data[last - 1] == '\v'))
        --last;
    out.assign(data + first, data + last);
    return true;
}

std::wstring join_path(const std::wstring& directory, const std::wstring& name)
{
    if (directory.empty()) return name;
    const wchar_t last = directory.back();
    return (last == L'\\' || last == L'/') ? directory + name : directory + L"\\" + name;
}

std::wstring key_file(const std::wstring& directory, std::uint64_t key, const wchar_t* prefix,
                      const wchar_t* suffix)
{
    wchar_t stem[17];
    std::swprintf(stem, 17, L"%016llx", static_cast<unsigned long long>(key));
    return join_path(directory, std::wstring(prefix) + stem + suffix);
}

std::string narrow_ascii(const std::vector<unsigned char>& bytes, std::size_t limit)
{
    if (bytes.empty()) return std::string();
    std::string text(reinterpret_cast<const char*>(bytes.data()),
                     bytes.size() < limit ? bytes.size() : limit);
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                                   text[begin] == '\n'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n'))
        --end;
    return text.substr(begin, end - begin);
}

std::string manifest_text(const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string text(manifest_header);
    text += '\n';
    for (const auto& field : fields) {
        text += field.first;
        text += ' ';
        text += field.second;
        text += '\n';
    }
    return text;
}

bool record_round_trip(const Record& record, const void* data, std::size_t size)
{
    std::vector<unsigned char> bytes;
    std::string detail;
    if (!serialize_record(record, bytes, detail) || bytes.size() != size) return false;
    return std::memcmp(bytes.data(), data, size) == 0;
}

bool load_record(const std::wstring& directory, std::uint64_t key, Record& record,
                 std::string& detail)
{
    const std::wstring request = key_file(directory, key, L"req_", L".csn");
    std::vector<unsigned char> bytes;
    const ReadStatus status = read_bounded(request, max_record, bytes);
    if (status == ReadStatus::missing) {
        detail = "the request record is missing";
        return false;
    }
    if (status == ReadStatus::too_large) {
        detail = "the request record is larger than " + std::to_string(max_record) + " bytes";
        return false;
    }
    if (status != ReadStatus::ok) {
        detail = "the request record could not be read";
        return false;
    }
    if (!parse_record(bytes.data(), bytes.size(), record, detail)) return false;
    if (!validate_record(record, detail)) return false;
    if (record_key(record) != key) {
        detail = "the request bytes do not hash to the request key";
        return false;
    }
    return true;
}

bool output_parts(const std::vector<unsigned char>& output,
                  std::map<std::string, std::pair<std::size_t, std::size_t>>& parts,
                  std::string& detail)
{
    return dxbc_parts(output.data(), output.size(), parts, detail);
}

std::string rts0_contract_text(const RootContract& contract)
{
    std::string text = "version=" + std::to_string(contract.version) +
                       " samplers=" + std::to_string(contract.samplers) +
                       " flags=" + std::to_string(contract.flags);
    for (std::size_t i = 0; i < contract.parameters.size(); ++i) {
        const RootParameterInfo& parameter = contract.parameters[i];
        text += " #" + std::to_string(i) + " type=" + std::to_string(parameter.type) +
                " reg=" + std::to_string(parameter.reg) +
                " space=" + std::to_string(parameter.space);
        if (parameter.has_count) text += " count=" + std::to_string(parameter.count);
        else text += " flags=" + std::to_string(parameter.flags);
    }
    return text;
}

bool deserialize_matches(const unsigned char* container, std::size_t size, const Record& record,
                         RootContract& contract, std::string& detail)
{
    std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
    if (!dxbc_parts(container, size, parts, detail)) return false;
    const auto found = parts.find("RTS0");
    if (found == parts.end()) {
        detail = "the output carries no embedded root signature";
        return false;
    }
    if (!parse_rts0(container + found->second.first, found->second.second, contract, detail))
        return false;
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(container, size,
                                                              IID_PPV_ARGS(&reader))) ||
        !reader) {
        detail = "the embedded root signature could not be deserialized";
        return false;
    }
    // `desc` points into the deserializer; it must stay alive for the whole validation.
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)) ||
        !desc) {
        detail = "the embedded root signature has no 1.1 description";
        return false;
    }
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        detail = "the embedded root signature is not version 1.1";
        return false;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& d = desc->Desc_1_1;
    if (d.NumParameters != static_cast<UINT>(fixture_roots)) {
        detail = "the embedded root signature does not declare three parameters";
        return false;
    }
    if (d.NumStaticSamplers != 0) {
        detail = "the supported root signature has no static samplers";
        return false;
    }
    if (d.Flags != D3D12_ROOT_SIGNATURE_FLAG_NONE) {
        detail = "the supported root signature has root flags";
        return false;
    }
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1& parameter = d.pParameters[i];
        if (parameter.ParameterType != record.roots[i].type) {
            detail = "root parameter " + std::to_string(i) + " differs from the record contract";
            return false;
        }
        if (parameter.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL) {
            detail = "root parameter " + std::to_string(i) + " is not visible to all stages";
            return false;
        }
        if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (parameter.Constants.ShaderRegister != record.roots[i].reg ||
                parameter.Constants.RegisterSpace != 0 ||
                parameter.Constants.Num32BitValues != root_constants ||
                parameter.Constants.Num32BitValues != record.roots[i].count) {
                detail = "root constants parameter " + std::to_string(i) +
                         " differs from the record contract";
                return false;
            }
        } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                   parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV) {
            if (parameter.Descriptor.ShaderRegister != record.roots[i].reg ||
                parameter.Descriptor.RegisterSpace != 0 ||
                parameter.Descriptor.Flags != D3D12_ROOT_DESCRIPTOR_FLAG_NONE) {
                detail = "root descriptor " + std::to_string(i) +
                         " differs from the record contract";
                return false;
            }
        } else {
            detail = "the embedded root signature uses an unsupported parameter type";
            return false;
        }
    }
    detail = rts0_contract_text(contract);
    return true;
}

}  // namespace

const char* state_name(State state)
{
    switch (state) {
    case State::missing: return "missing";
    case State::corrupt_request: return "corrupt-request";
    case State::error: return "error";
    case State::stale: return "stale";
    case State::corrupt: return "corrupt";
    case State::refused: return "refused";
    case State::backend_missing: return "backend-missing";
    case State::hit: return "hit";
    case State::busy: return "busy";
    }
    return "error";
}

int state_code(State state) { return static_cast<int>(state); }

Expected Expected::pinned()
{
    Expected expected;
    expected.translator = "compute_store_aot/036cdd081f7f709a";
    expected.decoder = "pixel_aot/607e451cbacf7ddf";
    expected.pipeline = "4c343eb689c60174";
    expected.llvm = "de2e35a4f9b3f6a06d2a8a3342b3f62a3842b1923b8dfc2a6ce48e0cc2d1e85d";
    expected.compiler = "495033935c058fc6d318ac257afdfa910795bf41b7e5ba905a0e7191dc8bd4fa";
    expected.validator = "db108ffef7bcecb984bdccf2b4725719d65b2ce7f1ebc40019dbe6c5a7016e09";
    expected.target = "dxil-ms-dx";
    expected.dxc_flags = "cs_6_0 -E main";
    return expected;
}

// ---------------------------------------------------------------- record

bool parse_record(const void* data, std::size_t size, Record& record, std::string& detail)
{
    record = Record{};
    if (size > max_record) {
        detail = "Record is larger than " + std::to_string(max_record) + " bytes";
        return false;
    }
    if (size < prefix_bytes + digest_bytes) {
        detail = "Record is truncated";
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (std::memcmp(bytes, magic, 4) != 0) {
        detail = "Record magic is not X2WN";
        return false;
    }
    std::size_t offset = 4;
    std::uint32_t* scalars[] = {
        &record.version, &record.stage, &record.isa, &record.profile, &record.guest_wave,
        &record.group_x, &record.group_y, &record.group_z,
        &record.dispatch_x, &record.dispatch_y, &record.dispatch_z,
        &record.user_sgpr_count, &record.table_reg, &record.group_reg, &record.group_reg_y,
        &record.descriptor_offset, &record.constant_descriptor, &record.source_descriptor,
        &record.source_stride, &record.u0_stride,
        &record.address_layout, &record.column_shift, &record.element_bytes,
        &record.guard_axis, &record.guard_op, &record.bound_constant, &record.cap_constant,
        &record.translator_revision, &record.decoder_id, &record.dxc_profile, &record.dxc_flags,
        &record.rsrc2_interpretation, &record.recorded_rsrc2, &record.code_length,
    };
    static_assert(sizeof(scalars) / sizeof(scalars[0]) == scalar_count, "scalar order drift");
    for (std::uint32_t* scalar : scalars) {
        *scalar = read_u32(bytes + offset);
        offset += 4;
    }
    record.root_count = read_u32(bytes + offset);
    offset += 4;
    if (record.root_count > max_root_parameters) {
        detail = "Record declares " + std::to_string(record.root_count) + " root parameters";
        return false;
    }
    if (size < offset + 12 * static_cast<std::size_t>(record.root_count) + digest_bytes) {
        detail = "Record is truncated inside the root parameter list";
        return false;
    }
    for (std::uint32_t i = 0; i < record.root_count; ++i) {
        record.roots[i].type = read_u32(bytes + offset);
        record.roots[i].reg = read_u32(bytes + offset + 4);
        record.roots[i].count = read_u32(bytes + offset + 8);
        offset += 12;
    }
    std::memcpy(record.code_sha256.data(), bytes + offset, digest_bytes);
    offset += digest_bytes;
    if (record.code_length > max_code) {
        detail = "Record declares " + std::to_string(record.code_length) + " code bytes";
        return false;
    }
    if (size != offset + static_cast<std::size_t>(record.code_length)) {
        detail = "Record length does not match its code length";
        return false;
    }
    record.code.assign(bytes + offset, bytes + size);
    if (!record_round_trip(record, data, size)) {
        detail = "The canonical record does not round-trip";
        return false;
    }
    return true;
}

bool serialize_record(const Record& record, std::vector<unsigned char>& out, std::string& detail)
{
    if (record.root_count > max_root_parameters) {
        detail = "Record declares " + std::to_string(record.root_count) + " root parameters";
        return false;
    }
    const std::uint32_t values[] = {
        record.version, record.stage, record.isa, record.profile, record.guest_wave,
        record.group_x, record.group_y, record.group_z,
        record.dispatch_x, record.dispatch_y, record.dispatch_z,
        record.user_sgpr_count, record.table_reg, record.group_reg, record.group_reg_y,
        record.descriptor_offset, record.constant_descriptor, record.source_descriptor,
        record.source_stride, record.u0_stride,
        record.address_layout, record.column_shift, record.element_bytes,
        record.guard_axis, record.guard_op, record.bound_constant, record.cap_constant,
        record.translator_revision, record.decoder_id, record.dxc_profile, record.dxc_flags,
        record.rsrc2_interpretation, record.recorded_rsrc2, record.code_length,
    };
    static_assert(sizeof(values) / sizeof(values[0]) == scalar_count, "scalar order drift");
    out.clear();
    out.reserve(prefix_bytes + root_bytes + digest_bytes + record.code.size());
    out.insert(out.end(), magic, magic + 4);
    unsigned char word[4];
    for (std::uint32_t value : values) {
        write_u32(word, value);
        out.insert(out.end(), word, word + 4);
    }
    write_u32(word, record.root_count);
    out.insert(out.end(), word, word + 4);
    for (std::uint32_t i = 0; i < record.root_count; ++i) {
        write_u32(word, record.roots[i].type);
        out.insert(out.end(), word, word + 4);
        write_u32(word, record.roots[i].reg);
        out.insert(out.end(), word, word + 4);
        write_u32(word, record.roots[i].count);
        out.insert(out.end(), word, word + 4);
    }
    out.insert(out.end(), record.code_sha256.begin(), record.code_sha256.end());
    out.insert(out.end(), record.code.begin(), record.code.end());
    return true;
}

bool validate_record(const Record& record, std::string& detail)
{
    if (record.version != accepted_record_version) {
        detail = "Unsupported record version " + std::to_string(record.version);
        return false;
    }
    if (record.stage != stage_compute) {
        detail = "Unsupported shader stage " + std::to_string(record.stage);
        return false;
    }
    if (record.isa != isa_gfx1013) {
        detail = "Unsupported ISA " + std::to_string(record.isa);
        return false;
    }
    if (record.profile != profile_1d && record.profile != profile_2d) {
        detail = "Unsupported native compute profile " + std::to_string(record.profile);
        return false;
    }
    if (record.guest_wave != guest_wave32) {
        detail = "Unsupported guest wave contract " + std::to_string(record.guest_wave);
        return false;
    }
    if (record.translator_revision != 1) {
        detail = "Unsupported translator revision " + std::to_string(record.translator_revision);
        return false;
    }
    if (record.decoder_id != 1) {
        detail = "Unsupported decoder identity " + std::to_string(record.decoder_id);
        return false;
    }
    if (record.dxc_profile != 0) {
        detail = "Unsupported DXC profile " + std::to_string(record.dxc_profile);
        return false;
    }
    if (record.dxc_flags != 0) {
        detail = "Unsupported DXC flags " + std::to_string(record.dxc_flags);
        return false;
    }
    if (record.rsrc2_interpretation != 0) {
        detail = "COMPUTE_PGM_RSRC2 is recorded, not interpreted: TG_SIZE_EN stays unsupported";
        return false;
    }
    if (record.recorded_rsrc2 != 0) {
        char text[96];
        std::snprintf(text, sizeof(text),
                      "recorded COMPUTE_PGM_RSRC2 0x%08x is not supported: the local translator "
                      "cannot interpret it",
                      record.recorded_rsrc2);
        detail = text;
        return false;
    }

    const std::uint32_t group[3] = {record.group_x, record.group_y, record.group_z};
    const std::uint32_t dispatch[3] = {record.dispatch_x, record.dispatch_y, record.dispatch_z};
    const char* axes = "xyz";
    for (int axis = 0; axis < 3; ++axis) {
        const std::uint32_t size = group[axis];
        const std::uint32_t limit = axis == 2 ? 64u : 1024u;
        if (size < 1 || size > limit) {
            detail = std::string("Unsupported thread group axis ") + axes[axis] + "=" +
                     std::to_string(size);
            return false;
        }
        if (size & (size - 1)) {
            detail = std::string("Thread group axis ") + axes[axis] + "=" + std::to_string(size) +
                     " is not a power of two";
            return false;
        }
    }
    const std::uint64_t product = static_cast<std::uint64_t>(group[0]) * group[1] * group[2];
    if (product > 1024) {
        detail = "Unsupported thread group product " + std::to_string(product);
        return false;
    }
    for (std::uint32_t size : dispatch) {
        if (size < 1 || size > 0xFFFF) {
            detail = "Unsupported dispatch dimensions";
            return false;
        }
    }

    const std::uint32_t user = record.user_sgpr_count;
    if (user < 2 || user > 16) {
        detail = "Unsupported user SGPR count " + std::to_string(user);
        return false;
    }
    if (record.table_reg > 105 || record.table_reg + 1 >= user) {
        detail = "The table pointer does not fit the user SGPRs";
        return false;
    }
    if (record.group_reg >= user || record.group_reg == record.table_reg) {
        detail = "The group X register is outside the user SGPRs";
        return false;
    }
    if (record.group_reg_y != no_register) {
        if (record.group_reg_y >= user) {
            detail = "The group Y register is outside the user SGPRs";
            return false;
        }
        if (record.group_reg_y == record.table_reg || record.group_reg_y == record.group_reg) {
            detail = "The group Y register collides with another entry register";
            return false;
        }
    }

    const std::uint32_t offsets[3] = {record.descriptor_offset, record.constant_descriptor,
                                      record.source_descriptor};
    const char* offset_names[3] = {"descriptor_offset", "constant_descriptor", "source_descriptor"};
    for (int i = 0; i < 3; ++i) {
        if (offsets[i] % 4 || offsets[i] >= 0x1000) {
            detail = std::string(offset_names[i]) + " " + std::to_string(offsets[i]) +
                     " is not a bounded DWORD table offset";
            return false;
        }
    }
    if (offsets[0] == offsets[1] || offsets[0] == offsets[2] || offsets[1] == offsets[2]) {
        detail = "The bound descriptors share a table offset";
        return false;
    }
    if (record.source_stride &&
        (record.source_stride % 4 || record.source_stride >= 0x1000)) {
        detail = "source_stride " + std::to_string(record.source_stride) +
                 " is not a bounded DWORD stride";
        return false;
    }

    if (record.address_layout != 1 && record.address_layout != 2) {
        detail = "Unsupported address layout " + std::to_string(record.address_layout);
        return false;
    }
    if (record.element_bytes != 4) {
        detail = "Unsupported element width " + std::to_string(record.element_bytes);
        return false;
    }
    if (record.column_shift != 2) {
        detail = "Unsupported column shift " + std::to_string(record.column_shift);
        return false;
    }
    if (record.address_layout == 1) {
        if (record.u0_stride != 4) {
            detail = "The index layout requires a four-byte u0 stride";
            return false;
        }
        if (group[1] != 1 || group[2] != 1) {
            detail = "The index layout requires a one-dimensional group";
            return false;
        }
        if (record.group_reg_y != no_register) {
            detail = "The index layout does not use a group Y register";
            return false;
        }
    } else {
        const std::uint64_t row = static_cast<std::uint64_t>(group[0]) << record.column_shift;
        if (record.u0_stride < row || record.u0_stride % 4) {
            detail = "The row stride does not hold one group row";
            return false;
        }
        if (record.group_reg_y == no_register) {
            detail = "The index-offset layout requires a group Y register";
            return false;
        }
    }

    if (record.guard_axis != 0 && record.guard_axis != 1) {
        detail = "Unsupported guard axis " + std::to_string(record.guard_axis);
        return false;
    }
    if (record.guard_op != 1) {
        detail = "Unsupported guard compare " + std::to_string(record.guard_op);
        return false;
    }
    if (record.bound_constant >= 16) {
        detail = "bound_constant " + std::to_string(record.bound_constant) +
                 " is outside the 16-DWORD constant contract";
        return false;
    }
    if (record.cap_constant >= 16) {
        detail = "cap_constant " + std::to_string(record.cap_constant) +
                 " is outside the 16-DWORD constant contract";
        return false;
    }

    static const RootParameter approved[fixture_roots] = {{3, 0, 1}, {4, 0, 1}, {1, 0, 16}};
    if (static_cast<std::size_t>(record.root_count) != fixture_roots) {
        detail = "The root signature contract is not the approved fixture contract";
        return false;
    }
    for (std::uint32_t i = 0; i < fixture_roots; ++i) {
        if (record.roots[i].type != approved[i].type ||
            record.roots[i].reg != approved[i].reg ||
            record.roots[i].count != approved[i].count) {
            detail = "The root signature contract is not the approved fixture contract";
            return false;
        }
    }

    if (record.profile == profile_1d &&
        (group[0] != 32 || group[1] != 1 || group[2] != 1 || record.address_layout != 1)) {
        detail = "Profile 1d requires group (32,1,1) and the index layout";
        return false;
    }
    if (record.profile == profile_2d &&
        (group[0] != 8 || group[1] != 4 || group[2] != 1 ||
         record.address_layout != 2 || record.u0_stride != 96)) {
        detail = "Profile 2d requires group (8,4,1), the index-offset layout and stride 96";
        return false;
    }
    if (record.profile == profile_2d &&
        static_cast<std::uint64_t>(dispatch[0]) * group[0] > (record.u0_stride / 4)) {
        detail = "The dispatch row overruns the declared stride";
        return false;
    }

    if (record.code.size() % 4) {
        detail = "The native code is not a whole number of words";
        return false;
    }
    if (record.code.size() < 4 || record.code.size() > max_code) {
        detail = "Unsupported native code length " + std::to_string(record.code.size());
        return false;
    }
    if (static_cast<std::size_t>(record.code_length) != record.code.size()) {
        detail = "code_length does not match the native code";
        return false;
    }
    if (sha256(record.code.data(), record.code.size()) != record.code_sha256) {
        detail = "The native code digest does not match its bytes";
        return false;
    }
    return true;
}

std::array<unsigned char, digest_bytes> sha256(const void* data, std::size_t size)
{
    return x2w_startup::sha256(data, size);
}

std::uint64_t fnv_bytes(const void* data, std::size_t size, std::uint64_t basis)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = basis;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * fnv_prime;
    return hash;
}

std::array<unsigned char, digest_bytes> record_digest(const Record& record)
{
    std::vector<unsigned char> bytes;
    std::string detail;
    if (!serialize_record(record, bytes, detail)) return {};
    return sha256(bytes.data(), bytes.size());
}

std::uint64_t record_key(const Record& record)
{
    const std::array<unsigned char, digest_bytes> digest = record_digest(record);
    const std::uint64_t seeded = fnv_bytes(revision, sizeof(revision) - 1, fnv_basis);
    return fnv_bytes(digest.data(), digest.size(), seeded);
}

std::string key_text(std::uint64_t key)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(key));
    return text;
}

bool parse_key_text(const char* text, std::uint64_t& key)
{
    if (!text || std::strlen(text) != 16) return false;
    std::uint64_t value = 0;
    for (int i = 0; i < 16; ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    key = value;
    return true;
}

std::string digest_hex(const std::array<unsigned char, digest_bytes>& digest)
{
    char text[65];
    for (int i = 0; i < 32; ++i)
        std::snprintf(text + 2 * i, 3, "%02x", digest[static_cast<std::size_t>(i)]);
    return text;
}

// ---------------------------------------------------------------- manifest

bool parse_manifest(const char* text, std::size_t size,
                    std::map<std::string, std::string>& fields, std::string& detail)
{
    fields.clear();
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = text[i];
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c == '\r') {
            lines.push_back(current);
            current.clear();
            if (i + 1 < size && text[i + 1] == '\n') ++i;
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) {
        detail = "Not a native compute manifest";
        return false;
    }
    std::string header;
    trim_ascii(lines[0].data(), lines[0].size(), header);
    if (header != manifest_header) {
        detail = "Not a native compute manifest";
        return false;
    }
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::string line;
        trim_ascii(lines[i].data(), lines[i].size(), line);
        if (line.empty()) continue;
        const std::size_t space = line.find(' ');
        if (space == std::string::npos || space + 1 >= line.size()) {
            detail = "Manifest line '" + line + "' has no value";
            return false;
        }
        const std::string name = line.substr(0, space);
        const std::string value = line.substr(space + 1);
        if (name.empty() || name[0] < 'a' || name[0] > 'z') {
            detail = "Manifest field name '" + name + "' is not supported";
            return false;
        }
        for (char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok) {
                detail = "Manifest field name '" + name + "' is not supported";
                return false;
            }
        }
        if (fields.count(name)) {
            detail = "Contradictory or repeated manifest field '" + name + "'";
            return false;
        }
        for (char c : value) {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                detail = "Manifest field '" + name + "' has a non-printable value";
                return false;
            }
        }
        if (name.rfind("pso-", 0) == 0) {
            const std::string backend = name.substr(4);
            if (backend != "warp" && backend != "hardware") {
                detail = "Unsupported probe backend '" + name + "'";
                return false;
            }
        } else if (name != "key" && name != "revision" && name != "status" &&
                   name != "record_version" && name != "record_sha256" && name != "code_bytes" &&
                   name != "code_sha256" && name != "group" && name != "hlsl_sha256" &&
                   name != "output_bytes" && name != "output_sha256" && name != "translator" &&
                   name != "decoder" && name != "pipeline" && name != "llvm" &&
                   name != "compiler" && name != "validator" && name != "target" &&
                   name != "dxc_flags" && name != "validation" && name != "rts0" &&
                   name != "gate" && name != "signed" && name != "reason") {
            detail = "Unsupported manifest field '" + name + "'";
            return false;
        }
        fields[name] = value;
    }
    return true;
}

std::map<std::string, std::map<std::string, std::string>>
verified_backends(const std::map<std::string, std::string>& fields)
{
    std::map<std::string, std::map<std::string, std::string>> found;
    for (const auto& field : fields) {
        if (field.first.rfind("pso-", 0) != 0) continue;
        const std::string backend = field.first.substr(4);
        std::map<std::string, std::string> parts;
        std::size_t start = 0;
        while (start < field.second.size()) {
            while (start < field.second.size() && field.second[start] == ' ') ++start;
            std::size_t end = start;
            while (end < field.second.size() && field.second[end] != ' ') ++end;
            const std::string piece = field.second.substr(start, end - start);
            const std::size_t equals = piece.find('=');
            if (equals != std::string::npos)
                parts[piece.substr(0, equals)] = piece.substr(equals + 1);
            start = end;
        }
        auto get = [&parts](const char* name) -> const std::string* {
            const auto it = parts.find(name);
            return it == parts.end() ? nullptr : &it->second;
        };
        const std::string* root = get("root");
        const std::string* pso = get("pso");
        const std::string* adapter = get("adapter");
        const std::string* model = get("shader_model");
        if (!root || !pso || !adapter || !model) continue;
        if (root->rfind("0x", 0) != 0 || pso->rfind("0x", 0) != 0) continue;
        if (*root != "0x00000000" || *pso != "0x00000000") continue;
        if (backend == "warp" && *adapter != "warp") continue;
        if (backend == "hardware" && (*adapter == "warp" || adapter->find(':') == std::string::npos))
            continue;
        found[backend] = parts;
    }
    return found;
}

// ---------------------------------------------------------------- files

ReadStatus read_bounded(const std::wstring& path, std::size_t limit,
                        std::vector<unsigned char>& out)
{
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                   ? ReadStatus::missing
                   : ReadStatus::failure;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return ReadStatus::failure;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > limit) {
        CloseHandle(file);
        return ReadStatus::too_large;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < out.size()) {
        const std::size_t remaining = out.size() - offset;
        const DWORD want = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
        DWORD got = 0;
        if (!ReadFile(file, out.data() + offset, want, &got, nullptr) || got == 0) {
            CloseHandle(file);
            out.clear();
            return ReadStatus::failure;
        }
        offset += got;
    }
    // A file that grew after the size probe must be rejected, not silently truncated.
    unsigned char extra = 0;
    DWORD got = 0;
    const BOOL read = ReadFile(file, &extra, 1, &got, nullptr);
    if (read && got) {
        CloseHandle(file);
        out.clear();
        return ReadStatus::too_large;
    }
    CloseHandle(file);
    return ReadStatus::ok;
}

bool path_exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring cache_file(const std::wstring& directory, std::uint64_t key, const wchar_t* prefix,
                        const wchar_t* suffix)
{
    return key_file(directory, key, prefix, suffix);
}

// ---------------------------------------------------------------- container and root

bool dxbc_parts(const unsigned char* data, std::size_t size,
                std::map<std::string, std::pair<std::size_t, std::size_t>>& parts,
                std::string& detail)
{
    parts.clear();
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0) {
        detail = "Expected a DXBC container";
        return false;
    }
    const std::uint32_t version = read_u32(data + 20);
    const std::uint32_t total = read_u32(data + 24);
    const std::uint32_t count = read_u32(data + 28);
    const std::uint64_t table_end = 32ull + 4ull * count;
    if (version != 1 || total != size || table_end > size) {
        detail = "Container header disagrees with inventory";
        return false;
    }
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t offset = read_u32(data + 32 + 4 * static_cast<std::size_t>(index));
        if (offset < table_end || static_cast<std::uint64_t>(offset) + 8 > size) {
            detail = "Invalid container part offset";
            return false;
        }
        const std::string tag(reinterpret_cast<const char*>(data + offset), 4);
        const std::uint32_t length = read_u32(data + offset + 4);
        const std::uint64_t part_end = static_cast<std::uint64_t>(offset) + 8 + length;
        if (part_end > size || parts.count(tag)) {
            detail = "Invalid container part";
            return false;
        }
        spans.emplace_back(offset, static_cast<std::size_t>(part_end));
        parts[tag] = {offset + 8, length};
    }
    std::sort(spans.begin(), spans.end());
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].first < spans[i - 1].second) {
            detail = "Overlapping container parts";
            return false;
        }
    }
    return true;
}

bool parse_rts0(const unsigned char* data, std::size_t size, RootContract& contract,
                std::string& detail)
{
    contract = RootContract{};
    if (!data || size < 24) {
        detail = "The embedded RTS0 part is truncated";
        return false;
    }
    const std::uint32_t version = read_u32(data + 0);
    const std::uint32_t count = read_u32(data + 4);
    const std::uint32_t parameter_offset = read_u32(data + 8);
    const std::uint32_t samplers = read_u32(data + 12);
    const std::uint32_t sampler_offset = read_u32(data + 16);
    const std::uint32_t flags = read_u32(data + 20);
    if (version != 2) {
        detail = "Unsupported embedded root signature version " + std::to_string(version);
        return false;
    }
    if (count > max_root_parameters) {
        detail = "The embedded root signature declares " + std::to_string(count) + " parameters";
        return false;
    }
    if (samplers > 8) {
        detail = "The embedded root signature declares " + std::to_string(samplers) +
                 " static samplers";
        return false;
    }
    if (sampler_offset > size) {
        detail = "The embedded sampler table is outside the part";
        return false;
    }
    contract.version = version;
    contract.samplers = samplers;
    contract.flags = flags;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t at = static_cast<std::size_t>(parameter_offset) + 12 * index;
        if (at + 12 > size) {
            detail = "A root parameter entry is truncated";
            return false;
        }
        RootParameterInfo parameter;
        parameter.type = read_u32(data + at);
        parameter.visibility = read_u32(data + at + 4);
        const std::uint32_t offset = read_u32(data + at + 8);
        if (parameter.type == 1) {
            if (static_cast<std::uint64_t>(offset) + 12 > size) {
                detail = "A root constants entry is truncated";
                return false;
            }
            parameter.reg = read_u32(data + offset);
            parameter.space = read_u32(data + offset + 4);
            parameter.count = read_u32(data + offset + 8);
            parameter.has_count = true;
            parameter.flags = 0;
        } else if (parameter.type == 3 || parameter.type == 4) {
            if (static_cast<std::uint64_t>(offset) + 12 > size) {
                detail = "A root descriptor entry is truncated";
                return false;
            }
            parameter.reg = read_u32(data + offset);
            parameter.space = read_u32(data + offset + 4);
            parameter.flags = read_u32(data + offset + 8);
        } else {
            detail = "Unsupported embedded root parameter type " + std::to_string(parameter.type);
            return false;
        }
        contract.parameters.push_back(parameter);
    }
    return true;
}

bool verify_rts0(const unsigned char* data, std::size_t size, const Record& record,
                 std::string& detail)
{
    RootContract contract;
    if (!parse_rts0(data, size, contract, detail)) return false;
    if (contract.samplers != 0) {
        detail = "The supported root signature has no static samplers";
        return false;
    }
    if (contract.flags != 0) {
        detail = "The supported root signature has flags";
        return false;
    }
    if (contract.parameters.size() != record.root_count) {
        detail = "The embedded root signature parameter count differs from the record";
        return false;
    }
    for (std::size_t index = 0; index < contract.parameters.size(); ++index) {
        const RootParameterInfo& parameter = contract.parameters[index];
        if (parameter.type != record.roots[index].type ||
            parameter.reg != record.roots[index].reg) {
            detail = "Root parameter " + std::to_string(index) + " differs from the record contract";
            return false;
        }
        if (parameter.visibility != 0) {
            detail = "Root parameter " + std::to_string(index) + " is not visible to all stages";
            return false;
        }
        if (parameter.space != 0) {
            detail = "Root parameter " + std::to_string(index) + " uses register space " +
                     std::to_string(parameter.space);
            return false;
        }
        if (parameter.type == 1) {
            if (parameter.count != record.roots[index].count ||
                parameter.count != root_constants) {
                detail = "Root constants parameter " + std::to_string(index) + " has " +
                         std::to_string(parameter.count) + " values";
                return false;
            }
        } else if (parameter.flags != 0) {
            detail = "Root descriptor " + std::to_string(index) + " has flags";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- acceptance

Acceptance inspect(const std::wstring& directory, std::uint64_t key,
                   const std::vector<std::string>& required, const Expected& expected)
{
    Acceptance result;
    if (required.empty()) {
        result.state = State::error;
        result.detail = "no required backend selection";
        return result;
    }
    for (const std::string& backend : required) {
        if (backend != "warp" && backend != "hardware") {
            result.state = State::error;
            result.detail = "unsupported required backend '" + backend + "'";
            return result;
        }
    }

    const std::wstring corrupt = key_file(directory, key, L"", L".csn.corrupt");
    if (path_exists(corrupt)) {
        std::vector<unsigned char> bytes;
        if (read_bounded(corrupt, 4096, bytes) == ReadStatus::ok)
            result.detail = narrow_ascii(bytes, 4096);
        else
            result.detail = "the quarantine marker could not be read";
        result.state = State::corrupt_request;
        return result;
    }
    const std::wstring error = key_file(directory, key, L"", L".csn.error");
    if (path_exists(error)) {
        std::vector<unsigned char> bytes;
        if (read_bounded(error, 4096, bytes) == ReadStatus::ok)
            result.detail = narrow_ascii(bytes, 4096);
        else
            result.detail = "the error marker could not be read";
        result.state = State::error;
        return result;
    }
    const std::wstring manifest_path = key_file(directory, key, L"", L".csn.manifest");
    std::vector<unsigned char> manifest_bytes;
    const ReadStatus manifest_status = read_bounded(manifest_path, max_manifest, manifest_bytes);
    if (manifest_status == ReadStatus::missing) return result;
    if (manifest_status == ReadStatus::too_large) {
        result.state = State::corrupt;
        result.detail = "the manifest is larger than " + std::to_string(max_manifest) + " bytes";
        return result;
    }
    if (manifest_status != ReadStatus::ok) {
        result.state = State::corrupt;
        result.detail = "the manifest could not be read";
        return result;
    }
    std::map<std::string, std::string> fields;
    std::string detail;
    if (!parse_manifest(reinterpret_cast<const char*>(manifest_bytes.data()),
                        manifest_bytes.size(), fields, detail)) {
        result.state = State::corrupt;
        result.detail = detail;
        return result;
    }
    result.has_fields = true;
    result.fields = fields;

    const char* common[] = {"key", "revision", "status", "record_version", "record_sha256"};
    for (const char* name : common) {
        if (!fields.count(name)) result.problems.push_back(std::string("missing: ") + name);
    }
    if (!result.problems.empty()) {
        result.state = State::corrupt;
        result.detail = result.problems.front();
        return result;
    }

    const std::string expected_key = key_text(key);
    if (fields["key"] != expected_key) {
        result.state = State::stale;
        result.detail = "manifest key does not match the filename";
        return result;
    }
    if (fields["revision"] != revision) {
        result.state = State::stale;
        result.detail = "manifest revision '" + fields["revision"] + "' is not supported";
        return result;
    }

    if (fields["status"] == "refused") {
        Record record;
        if (!load_record(directory, key, record, detail)) {
            result.state = State::corrupt;
            result.detail = "refusal record: " + detail;
            return result;
        }
        result.has_record = true;
        result.record = record;
        if (fields["record_sha256"] != digest_hex(record_digest(record))) {
            result.state = State::corrupt;
            result.detail = "refusal record digest does not match";
            return result;
        }
        if (fields["record_version"] != std::to_string(record.version)) {
            result.state = State::corrupt;
            result.detail = "refusal record version does not match";
            return result;
        }
        if (fields["code_bytes"] != std::to_string(record.code.size()) ||
            fields["code_sha256"] != digest_hex(sha256(record.code.data(), record.code.size()))) {
            result.state = State::corrupt;
            result.detail = "refusal record code identity does not match";
            return result;
        }
        result.state = State::refused;
        result.detail = fields.count("reason") ? fields["reason"] : "";
        return result;
    }
    if (fields["status"] != "validated") {
        result.state = State::corrupt;
        result.detail = "unknown manifest status '" + fields["status"] + "'";
        return result;
    }

    const char* validated[] = {
        "key", "revision", "status", "record_version", "record_sha256",
        "code_bytes", "code_sha256", "group", "hlsl_sha256", "output_bytes", "output_sha256",
        "translator", "decoder", "pipeline", "llvm", "compiler", "validator", "target",
        "dxc_flags", "validation", "rts0", "gate", "signed",
    };
    for (const char* name : validated) {
        if (!fields.count(name)) result.problems.push_back(std::string("missing: ") + name);
    }
    struct Identity {
        const char* name;
        const std::string* value;
    };
    const Identity identities[] = {
        {"translator", &expected.translator}, {"decoder", &expected.decoder},
        {"pipeline", &expected.pipeline},     {"llvm", &expected.llvm},
        {"compiler", &expected.compiler},     {"validator", &expected.validator},
        {"target", &expected.target},         {"dxc_flags", &expected.dxc_flags},
    };
    for (const Identity& identity : identities) {
        const auto it = fields.find(identity.name);
        if (it != fields.end() && it->second != *identity.value) {
            result.state = State::stale;
            result.detail = std::string("manifest ") + identity.name + " '" + it->second +
                            "' is not the pinned '" + *identity.value + "'";
            return result;
        }
    }
    struct Evidence {
        const char* name;
        const char* value;
    };
    const Evidence evidence[] = {
        {"validation", "container=0x00000000 module=0x00000000"},
        {"rts0", "present"},
        {"gate", "ALLOW"},
        {"signed", "dxil-digest-nonzero"},
    };
    for (const Evidence& item : evidence) {
        const auto it = fields.find(item.name);
        if (it == fields.end()) continue;
        if (it->second != item.value)
            result.problems.push_back(std::string(item.name) + ": '" + it->second +
                                      "' is not '" + item.value + "'");
    }
    if (!result.problems.empty()) {
        result.state = State::corrupt;
        result.detail = result.problems.front();
        return result;
    }

    Record record;
    if (!load_record(directory, key, record, detail)) {
        result.state = State::corrupt;
        result.detail = "record: " + detail;
        return result;
    }
    result.has_record = true;
    result.record = record;
    if (fields["record_version"] != std::to_string(record.version))
        result.problems.push_back("record_version: does not match the record");
    if (fields["record_sha256"] != digest_hex(record_digest(record)))
        result.problems.push_back("record_sha256: does not match the record");
    if (fields["code_bytes"] != std::to_string(record.code.size()))
        result.problems.push_back("code_bytes: does not match the record");
    if (fields["code_sha256"] != digest_hex(sha256(record.code.data(), record.code.size())))
        result.problems.push_back("code_sha256: does not match the record");
    const std::string group = std::to_string(record.group_x) + "x" +
                              std::to_string(record.group_y) + "x" +
                              std::to_string(record.group_z);
    if (fields["group"] != group) result.problems.push_back("group: does not match the record");
    if (!result.problems.empty()) {
        result.state = State::corrupt;
        result.detail = result.problems.front();
        return result;
    }

    const std::wstring hlsl_path = key_file(directory, key, L"", L".csn.hlsl");
    std::vector<unsigned char> hlsl;
    const ReadStatus hlsl_status = read_bounded(hlsl_path, max_hlsl, hlsl);
    if (hlsl_status != ReadStatus::ok) {
        result.state = State::corrupt;
        result.detail = hlsl_status == ReadStatus::missing ? "hlsl: the file is missing"
                                                           : "hlsl: the file could not be read";
        return result;
    }
    if (fields["hlsl_sha256"] != digest_hex(sha256(hlsl.data(), hlsl.size()))) {
        result.state = State::corrupt;
        result.detail = "hlsl digest does not match";
        return result;
    }
    result.hlsl = hlsl;

    const std::wstring output_path = key_file(directory, key, L"", L".csn.cso");
    std::vector<unsigned char> output;
    const ReadStatus output_status = read_bounded(output_path, max_output, output);
    if (output_status != ReadStatus::ok) {
        result.state = State::corrupt;
        result.detail = output_status == ReadStatus::missing ? "output: the file is missing"
                                                             : "output: the file could not be read";
        return result;
    }
    if (fields["output_bytes"] != std::to_string(output.size())) {
        result.state = State::corrupt;
        result.detail = "output size does not match";
        return result;
    }
    if (fields["output_sha256"] != digest_hex(sha256(output.data(), output.size()))) {
        result.state = State::corrupt;
        result.detail = "output digest does not match";
        return result;
    }
    if (output.size() < 4 || std::memcmp(output.data(), "DXBC", 4) != 0) {
        result.state = State::corrupt;
        result.detail = "output is not a DXBC container";
        return result;
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
    if (!output_parts(output, parts, detail)) {
        result.state = State::corrupt;
        result.detail = "output container: " + detail;
        return result;
    }
    if (!parts.count("DXIL")) {
        result.state = State::corrupt;
        result.detail = "output carries no DXIL part";
        return result;
    }
    if (!parts.count("RTS0")) {
        result.state = State::corrupt;
        result.detail = "output carries no embedded root signature";
        return result;
    }
    bool signed_digest = false;
    for (std::size_t i = 4; i < 20 && i < output.size(); ++i) {
        if (output[i]) {
            signed_digest = true;
            break;
        }
    }
    if (!signed_digest) {
        result.state = State::corrupt;
        result.detail = "output container digest is zero (unsigned)";
        return result;
    }
    const auto rts0 = parts["RTS0"];
    if (!verify_rts0(output.data() + rts0.first, rts0.second, record, detail)) {
        result.state = State::corrupt;
        result.detail = "root contract: " + detail;
        return result;
    }
    result.output = output;

    const auto backends = verified_backends(fields);
    std::string missing;
    for (const std::string& backend : required) {
        if (!backends.count(backend)) {
            if (!missing.empty()) missing += ",";
            missing += backend;
        }
    }
    if (!missing.empty()) {
        result.state = State::backend_missing;
        result.detail = "missing required backend " + missing;
        return result;
    }
    result.state = State::hit;
    result.detail.clear();
    return result;
}

// ---------------------------------------------------------------- device

void Pipeline::reset()
{
    if (pso) {
        pso->Release();
        pso = nullptr;
    }
    if (root) {
        root->Release();
        root = nullptr;
    }
    contract = RootContract{};
}

HRESULT create_pipeline(ID3D12Device* device, const Record& record,
                        const std::vector<unsigned char>& output, Pipeline& pipeline,
                        std::string& detail)
{
    pipeline.reset();
    if (!device) {
        detail = "no desktop device";
        return E_POINTER;
    }
    std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
    if (!dxbc_parts(output.data(), output.size(), parts, detail)) return E_FAIL;
    const auto found = parts.find("RTS0");
    if (found == parts.end()) {
        detail = "the output carries no embedded root signature";
        return E_FAIL;
    }
    const unsigned char* rts0 = output.data() + found->second.first;
    const std::size_t rts0_size = found->second.second;
    RootContract contract;
    if (!verify_rts0(rts0, rts0_size, record, detail)) return E_FAIL;

    // D3D12 accepts the DXBC container and extracts the embedded signature; the bare RTS0 part is
    // structural evidence only (the runtime rejects it as a serialized signature). The wrapper's
    // read_root_parameters deserializes the same container, so the recorded metadata matches.
    ID3D12RootSignature* root = nullptr;
    HRESULT hr = device->CreateRootSignature(0, output.data(), output.size(), IID_PPV_ARGS(&root));
    if (FAILED(hr) || !root) {
        detail = "CreateRootSignature failed";
        if (root) root->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    if (!deserialize_matches(output.data(), output.size(), record, contract, detail)) {
        root->Release();
        return E_FAIL;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {output.data(), static_cast<UINT>(output.size())};
    ID3D12PipelineState* pso = nullptr;
    hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr) || !pso) {
        detail = "CreateComputePipelineState failed";
        root->Release();
        if (pso) pso->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    pipeline.root = root;
    pipeline.pso = pso;
    pipeline.contract = contract;
    detail = rts0_contract_text(contract);
    return S_OK;
}

// ---------------------------------------------------------------- writer

namespace {

std::string make_token()
{
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device() ^
                           static_cast<std::uint64_t>(GetCurrentProcessId()) ^
                           static_cast<std::uint64_t>(GetTickCount64()));
    static LONG counter;
    const unsigned long long serial =
        static_cast<unsigned long long>(InterlockedIncrement(&counter));
    char text[96];
    std::snprintf(text, sizeof(text), "%lu-%016llx%016llx%llu",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(engine()),
                  static_cast<unsigned long long>(engine()), serial);
    return text;
}

bool read_token(const std::wstring& path, std::string& token)
{
    std::vector<unsigned char> bytes;
    if (read_bounded(path, 256, bytes) != ReadStatus::ok) return false;
    token = narrow_ascii(bytes, 256);
    return true;
}

}  // namespace

LockResult acquire_lock(const std::wstring& directory, std::uint64_t key, Writer& writer,
                        std::string& detail)
{
    writer = Writer{};
    const std::wstring path = key_file(directory, key, L"", L".csn.lock");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            detail = "another writer holds the lock; an abandoned lock is a manual-repair condition";
            return LockResult::busy;
        }
        detail = "the writer lock could not be created";
        return LockResult::failure;
    }
    const std::string token = make_token();
    const std::string body = token + "\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written,
                              nullptr) && written == body.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        detail = "the writer token could not be written";
        return LockResult::failure;
    }
    writer.lock_path = path;
    writer.token = token;
    writer.held = true;
    return LockResult::acquired;
}

bool assert_owner(const Writer& writer, std::string& detail)
{
    if (!writer.held) {
        detail = "the writer does not hold a lock";
        return false;
    }
    std::string current;
    if (!read_token(writer.lock_path, current)) {
        detail = "the writer lock disappeared; no mutation was made";
        return false;
    }
    if (current != writer.token) {
        detail = "the writer lock is held by another writer; no mutation was made";
        return false;
    }
    return true;
}

void release_lock(Writer& writer)
{
    if (!writer.held) return;
    std::string current;
    if (read_token(writer.lock_path, current) && current == writer.token)
        DeleteFileW(writer.lock_path.c_str());
    writer = Writer{};
}

bool write_atomic(const std::wstring& path, const void* data, std::size_t size,
                  std::string& detail)
{
    static LONG serial;
    wchar_t suffix[64];
    std::swprintf(suffix, 64, L".%lu.%lld.tmp", static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<long long>(InterlockedIncrement(&serial)));
    const std::wstring temporary = path + suffix;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        detail = "the temporary file could not be created";
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t offset = 0;
    bool ok = true;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const DWORD want = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, want, &written, nullptr) || written != want) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) ok = false;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) {
        DeleteFileW(temporary.c_str());
        detail = "the atomic replacement failed";
        return false;
    }
    return true;
}

bool clear_corrupt_marker(const std::wstring& directory, std::uint64_t key)
{
    return DeleteFileW(key_file(directory, key, L"", L".csn.corrupt").c_str()) != 0;
}

bool publish_request(const std::wstring& directory, std::uint64_t key, const Record& record,
                     State& state, std::string& detail)
{
    if (record_key(record) != key) {
        state = State::corrupt;
        detail = "the record bytes do not hash to the request key";
        return false;
    }
    // Match the accepted stage_request: an accepted or refused entry is not rewritten.
    const Acceptance existing = inspect(directory, key, {"warp", "hardware"});
    if (existing.state == State::hit || existing.state == State::refused) {
        state = existing.state;
        detail = "the entry already reports ";
        detail += state_name(existing.state);
        return true;
    }
    Writer writer;
    const LockResult lock = acquire_lock(directory, key, writer, detail);
    if (lock == LockResult::busy) {
        state = State::busy;
        return false;
    }
    if (lock != LockResult::acquired) {
        state = State::error;
        return false;
    }
    std::vector<unsigned char> bytes;
    if (!serialize_record(record, bytes, detail) || !assert_owner(writer, detail) ||
        !write_atomic(key_file(directory, key, L"req_", L".csn"), bytes.data(), bytes.size(),
                      detail)) {
        release_lock(writer);
        state = State::error;
        return false;
    }
    const std::string digest = digest_hex(record_digest(record));
    const std::string code = digest_hex(sha256(record.code.data(), record.code.size()));
    const std::string text = manifest_text({
        {"key", key_text(key)},
        {"revision", revision},
        {"status", "request"},
        {"record_version", std::to_string(record.version)},
        {"record_sha256", digest},
        {"code_bytes", std::to_string(record.code.size())},
        {"code_sha256", code},
    });
    if (!assert_owner(writer, detail) ||
        !write_atomic(key_file(directory, key, L"", L".csn.src"), text.data(), text.size(),
                      detail)) {
        release_lock(writer);
        state = State::error;
        return false;
    }
    clear_corrupt_marker(directory, key);
    release_lock(writer);
    state = State::missing;
    detail = "request published";
    return true;
}

}  // namespace x2w_native
