#pragma once
// Bounded C++ consumer of the accepted revision-2 native compute cache (#1412, Stage 4C / N2).
//
// This translation unit is independent of the startup wrapper: it parses the canonical record,
// reproduces the FNV/SHA-256 identity, applies the one authoritative acceptance decision, verifies
// the embedded root-signature contract and creates the desktop root/PSO on a caller-supplied
// device. It never translates, compiles, dispatches or interprets production slot-67 memory.
//
// The accepted contract is out/stage4b-hardening/cache-contract.md and
// tools/startup/compute_native.py. The expected dependency identities are hardcoded below; a
// supported toolchain change is an explicit edit of pinned_expected() and a cache regeneration, not
// a value accepted from a manifest.
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace x2w_native {

inline constexpr char revision[] = "x2w-startup-compute-native-2";
inline constexpr char manifest_header[] = "x2w-native-compute 2";

inline constexpr std::uint32_t accepted_record_version = 1;
inline constexpr std::uint32_t stage_compute = 5;
inline constexpr std::uint32_t isa_gfx1013 = 1;
inline constexpr std::uint32_t guest_wave32 = 32;
inline constexpr std::uint32_t profile_1d = 1;
inline constexpr std::uint32_t profile_2d = 2;
inline constexpr std::uint32_t no_register = 0xFFFFFFFFu;

inline constexpr std::size_t scalar_count = 34;
inline constexpr std::size_t prefix_bytes = 4 + 4 * scalar_count + 4;  // magic, scalars, root count
inline constexpr std::size_t root_bytes = 36;                         // three {type, register, count}
inline constexpr std::size_t digest_bytes = 32;
inline constexpr std::size_t code_offset = prefix_bytes + root_bytes + digest_bytes;  // 212
inline constexpr std::size_t max_code = 64 * 1024;
inline constexpr std::size_t max_record = max_code + 4096;            // 69,632 outer read limit
inline constexpr std::size_t max_manifest = 1 << 16;
inline constexpr std::size_t max_output = 1 << 20;
inline constexpr std::size_t max_hlsl = 256 * 1024;
inline constexpr std::size_t max_root_parameters = 8;
inline constexpr std::size_t fixture_roots = 3;
inline constexpr std::uint32_t root_constants = 16;

inline constexpr std::uint64_t golden_key_1d = 0xd55528b6b2cfa1f8ull;
inline constexpr std::uint64_t golden_key_2d = 0x95c4c65e03a8b336ull;

struct RootParameter {
    std::uint32_t type = 0;
    std::uint32_t reg = 0;
    std::uint32_t count = 0;
};

// The parsed canonical record. Scalars keep their wire names and order; roots holds every declared
// parameter (validate_record requires exactly the three approved ones).
struct Record {
    std::uint32_t version = 0, stage = 0, isa = 0, profile = 0, guest_wave = 0;
    std::uint32_t group_x = 0, group_y = 0, group_z = 0;
    std::uint32_t dispatch_x = 0, dispatch_y = 0, dispatch_z = 0;
    std::uint32_t user_sgpr_count = 0, table_reg = 0, group_reg = 0, group_reg_y = 0;
    std::uint32_t descriptor_offset = 0, constant_descriptor = 0, source_descriptor = 0;
    std::uint32_t source_stride = 0, u0_stride = 0;
    std::uint32_t address_layout = 0, column_shift = 0, element_bytes = 0;
    std::uint32_t guard_axis = 0, guard_op = 0, bound_constant = 0, cap_constant = 0;
    std::uint32_t translator_revision = 0, decoder_id = 0, dxc_profile = 0, dxc_flags = 0;
    std::uint32_t rsrc2_interpretation = 0, recorded_rsrc2 = 0, code_length = 0;
    std::uint32_t root_count = 0;
    RootParameter roots[max_root_parameters]{};
    std::array<unsigned char, digest_bytes> code_sha256{};
    std::vector<unsigned char> code;
};

enum class State {
    missing,
    corrupt_request,
    error,
    stale,
    corrupt,
    refused,
    backend_missing,
    hit,
    busy,
};

const char* state_name(State state);
int state_code(State state);

struct Expected {
    std::string translator, decoder, pipeline, llvm, compiler, validator, target, dxc_flags;
    // The independently established identity. A dependency change edits these values and requires
    // regenerating every artifact; a manifest can never supply its own identity.
    static Expected pinned();
};

struct Acceptance {
    State state = State::missing;
    std::string detail;
    std::vector<std::string> problems;
    bool has_fields = false;
    std::map<std::string, std::string> fields;
    bool has_record = false;
    Record record;
    std::vector<unsigned char> output;  // exactly the CSO bytes that passed verification
    std::vector<unsigned char> hlsl;
    bool accepted() const { return state == State::hit; }
};

struct RootParameterInfo {
    std::uint32_t type = 0, visibility = 0, reg = 0, space = 0, count = 0, flags = 0;
    bool has_count = false;
};

struct RootContract {
    std::uint32_t version = 0, samplers = 0, flags = 0;
    std::vector<RootParameterInfo> parameters;
};

// Record: parse is structural only; validate applies the fixed supported profiles and digest.
bool parse_record(const void* data, std::size_t size, Record& record, std::string& detail);
bool validate_record(const Record& record, std::string& detail);
bool serialize_record(const Record& record, std::vector<unsigned char>& out, std::string& detail);

std::array<unsigned char, digest_bytes> sha256(const void* data, std::size_t size);
std::uint64_t fnv_bytes(const void* data, std::size_t size,
                        std::uint64_t basis = 1469598103934665603ull);
std::array<unsigned char, digest_bytes> record_digest(const Record& record);
std::uint64_t record_key(const Record& record);
std::string key_text(std::uint64_t key);
bool parse_key_text(const char* text, std::uint64_t& key);
std::string digest_hex(const std::array<unsigned char, digest_bytes>& digest);

bool parse_manifest(const char* text, std::size_t size,
                    std::map<std::string, std::string>& fields, std::string& detail);
std::map<std::string, std::map<std::string, std::string>>
verified_backends(const std::map<std::string, std::string>& fields);

// Bounded filesystem reads. A file that grows after the size probe is rejected, not truncated.
enum class ReadStatus { ok, missing, too_large, failure };
ReadStatus read_bounded(const std::wstring& path, std::size_t limit,
                        std::vector<unsigned char>& out);
bool path_exists(const std::wstring& path);
std::wstring cache_file(const std::wstring& directory, std::uint64_t key, const wchar_t* prefix,
                        const wchar_t* suffix);

bool dxbc_parts(const unsigned char* data, std::size_t size,
                std::map<std::string, std::pair<std::size_t, std::size_t>>& parts,
                std::string& detail);
bool parse_rts0(const unsigned char* data, std::size_t size, RootContract& contract,
                std::string& detail);
bool verify_rts0(const unsigned char* data, std::size_t size, const Record& record,
                 std::string& detail);

// The one authoritative acceptance decision. required must be nonempty and each name is warp or
// hardware; an empty selection is an error and can never be a hit.
Acceptance inspect(const std::wstring& directory, std::uint64_t key,
                   const std::vector<std::string>& required,
                   const Expected& expected = Expected::pinned());

// Root and PSO are created on the same device from the exact verified CSO buffer. The RTS0 part is
// extracted and structurally verified; D3D12 accepts the signed DXBC container (not the bare part
// on this runtime), so the whole verified buffer is passed to CreateRootSignature and the
// deserialized 1.1 description is compared against the record. Output pointers stay null on
// failure.
struct Pipeline {
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    RootContract contract;
    void reset();
    ~Pipeline() { reset(); }
    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
};
HRESULT create_pipeline(ID3D12Device* device, const Record& record,
                        const std::vector<unsigned char>& output, Pipeline& pipeline,
                        std::string& detail);

// Minimal writer used only by tests that exercise request publication/recovery. It is not wired
// into the fixture activation path, never translates and never compiles.
struct Writer {
    std::wstring lock_path;
    std::string token;
    bool held = false;
};
enum class LockResult { acquired, busy, failure };
LockResult acquire_lock(const std::wstring& directory, std::uint64_t key, Writer& writer,
                        std::string& detail);
bool assert_owner(const Writer& writer, std::string& detail);
void release_lock(Writer& writer);
bool write_atomic(const std::wstring& path, const void* data, std::size_t size,
                  std::string& detail);
bool publish_request(const std::wstring& directory, std::uint64_t key, const Record& record,
                     State& state, std::string& detail);
bool clear_corrupt_marker(const std::wstring& directory, std::uint64_t key);

}  // namespace x2w_native
