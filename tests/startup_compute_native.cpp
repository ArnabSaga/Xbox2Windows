// Native compute cache consumer controls (#1412, Stage 4C / N2).
//
//   startup_compute_native controls <cache-dir> [legacy-dir]   cache/record/manifest/writer only
//   startup_compute_native warp|hardware|debug <cache-dir> [legacy-dir]
//                                                             controls + real device + fixture bridge
//
// Exit 0 pass, 1 failure, 77 capability skip (cache or adapter unavailable).
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
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using x2w_native::Acceptance;
using x2w_native::Record;
using x2w_native::State;

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*,
                                                                     REFIID, void**);
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

namespace {

using Word = std::uint64_t;

Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0,
            Word e = 0, Word f = 0)
{
    using Slot = Word(WINAPI*)(void*, Word, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e, f);
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

bool read_all(const std::wstring& path, std::vector<unsigned char>& out)
{
    return x2w_native::read_bounded(path, (std::numeric_limits<std::size_t>::max)(), out) ==
           x2w_native::ReadStatus::ok;
}

bool read_text(const std::wstring& path, std::string& out)
{
    std::vector<unsigned char> bytes;
    if (!read_all(path, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool write_all(const std::wstring& path, const void* data, std::size_t size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
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
    CloseHandle(file);
    return ok;
}

bool write_text(const std::wstring& path, const std::string& text)
{
    return write_all(path, text.data(), text.size());
}

bool copy_path(const std::wstring& from, const std::wstring& to)
{
    std::vector<unsigned char> bytes;
    if (!read_all(from, bytes)) return false;
    return write_all(to, bytes.data(), bytes.size());
}

std::wstring temp_root()
{
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (!length || length >= MAX_PATH) return L".";
    return buffer;
}

std::wstring make_temp_dir(const char* tag)
{
    static LONG serial;
    wchar_t name[160];
    std::swprintf(name, 160, L"x2w-compute-native-%lu-%hs-%ld", static_cast<unsigned long>(GetCurrentProcessId()),
                  tag, static_cast<long>(InterlockedIncrement(&serial)));
    const std::wstring path = temp_root() + name;
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
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

std::wstring stem_path(const std::wstring& directory, std::uint64_t key, const wchar_t* prefix,
                       const wchar_t* suffix)
{
    return x2w_native::cache_file(directory, key, prefix, suffix);
}

// A fresh copy of the accepted entry for one key. Mutations never touch the accepted cache.
struct Clone {
    std::wstring directory;
    std::uint64_t key = 0;
    bool ok = false;
};

Clone clone_entry(const std::wstring& cache, std::uint64_t key, const char* tag)
{
    Clone clone;
    clone.key = key;
    clone.directory = make_temp_dir(tag);
    struct File {
        const wchar_t* prefix;
        const wchar_t* suffix;
    };
    const File files[] = {{L"req_", L".csn"},       {L"", L".csn.hlsl"}, {L"", L".csn.cso"},
                          {L"", L".csn.manifest"},  {L"", L".csn.src"}};
    for (const File& file : files) {
        const std::wstring from = stem_path(cache, key, file.prefix, file.suffix);
        const std::wstring to = stem_path(clone.directory, key, file.prefix, file.suffix);
        if (!x2w_native::path_exists(from)) continue;
        if (!copy_path(from, to)) return clone;
    }
    clone.ok = true;
    return clone;
}

bool manifest_lines(const std::wstring& directory, std::uint64_t key, std::vector<std::string>& lines)
{
    std::string text;
    if (!read_text(stem_path(directory, key, L"", L".csn.manifest"), text)) return false;
    lines.clear();
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return !lines.empty();
}

bool write_manifest(const std::wstring& directory, std::uint64_t key,
                    const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return write_text(stem_path(directory, key, L"", L".csn.manifest"), text);
}

bool manifest_set(const std::wstring& directory, std::uint64_t key, const char* name,
                  const char* value)
{
    std::vector<std::string> lines;
    if (!manifest_lines(directory, key, lines)) return false;
    const std::string prefix = std::string(name) + " ";
    bool replaced = false;
    for (std::string& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            line = prefix + value;
            replaced = true;
        }
    }
    if (!replaced) lines.push_back(prefix + value);
    return write_manifest(directory, key, lines);
}

bool manifest_remove(const std::wstring& directory, std::uint64_t key, const char* name)
{
    std::vector<std::string> lines;
    if (!manifest_lines(directory, key, lines)) return false;
    const std::string prefix = std::string(name) + " ";
    const auto end = std::remove_if(lines.begin(), lines.end(),
                                    [&prefix](const std::string& line) {
                                        return line.rfind(prefix, 0) == 0;
                                    });
    lines.erase(end, lines.end());
    return write_manifest(directory, key, lines);
}

bool manifest_append(const std::wstring& directory, std::uint64_t key, const std::string& line)
{
    std::vector<std::string> lines;
    if (!manifest_lines(directory, key, lines)) return false;
    lines.push_back(line);
    return write_manifest(directory, key, lines);
}

bool rewrite_output_evidence(const std::wstring& directory, std::uint64_t key,
                             const std::vector<unsigned char>& output)
{
    return manifest_set(directory, key, "output_bytes", std::to_string(output.size()).c_str()) &&
           manifest_set(directory, key, "output_sha256",
                        x2w_native::digest_hex(x2w_native::sha256(output.data(), output.size()))
                            .c_str());
}

bool mutate_output(const std::wstring& directory, std::uint64_t key,
                   const std::function<bool(std::vector<unsigned char>&)>& mutate,
                   bool update_manifest)
{
    std::vector<unsigned char> output;
    if (!read_all(stem_path(directory, key, L"", L".csn.cso"), output)) return false;
    if (!mutate(output)) return false;
    if (!write_all(stem_path(directory, key, L"", L".csn.cso"), output.data(), output.size()))
        return false;
    return !update_manifest || rewrite_output_evidence(directory, key, output);
}

bool mutate_rts0(const std::wstring& directory, std::uint64_t key,
                 const std::function<bool(std::vector<unsigned char>&)>& mutate)
{
    return mutate_output(
        directory, key,
        [&mutate](std::vector<unsigned char>& output) {
            std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
            std::string detail;
            if (!x2w_native::dxbc_parts(output.data(), output.size(), parts, detail)) return false;
            const auto found = parts.find("RTS0");
            if (found == parts.end()) return false;
            const std::size_t begin = found->second.first;
            std::vector<unsigned char> payload(output.begin() + begin,
                                               output.begin() + begin + found->second.second);
            if (!mutate(payload)) return false;
            if (payload.size() != found->second.second) return false;
            std::copy(payload.begin(), payload.end(), output.begin() + begin);
            return true;
        },
        true);
}

// ---------------------------------------------------------------- expectations

bool expect_state(Report& report, const std::string& name, const std::wstring& directory,
                  std::uint64_t key, const std::vector<std::string>& required, State expected,
                  const std::string& detail = "")
{
    const Acceptance acceptance = x2w_native::inspect(directory, key, required);
    const bool ok = acceptance.state == expected;
    report.check(name, ok,
                 ok ? detail
                    : std::string("got ") + x2w_native::state_name(acceptance.state) + " (" +
                          acceptance.detail + "), expected " + x2w_native::state_name(expected));
    return ok;
}

bool record_invalid(const Record& base, const std::function<void(Record&)>& mutate,
                    std::string& detail)
{
    Record record = base;
    mutate(record);
    std::vector<unsigned char> bytes;
    if (!x2w_native::serialize_record(record, bytes, detail)) return true;
    Record parsed;
    if (!x2w_native::parse_record(bytes.data(), bytes.size(), parsed, detail)) return true;
    return !x2w_native::validate_record(parsed, detail);
}

// ---------------------------------------------------------------- controls

bool load_golden(const std::wstring& cache, Record& one, Record& two, Report& report)
{
    const std::wstring one_path = stem_path(cache, x2w_native::golden_key_1d, L"req_", L".csn");
    const std::wstring two_path = stem_path(cache, x2w_native::golden_key_2d, L"req_", L".csn");
    std::vector<unsigned char> one_bytes, two_bytes;
    if (!read_all(one_path, one_bytes) || !read_all(two_path, two_bytes)) {
        report.check("golden records are readable", false, "the accepted cache is incomplete");
        return false;
    }
    std::string detail;
    if (!x2w_native::parse_record(one_bytes.data(), one_bytes.size(), one, detail) ||
        !x2w_native::validate_record(one, detail) ||
        !x2w_native::parse_record(two_bytes.data(), two_bytes.size(), two, detail) ||
        !x2w_native::validate_record(two, detail)) {
        report.check("golden records parse and validate", false, detail);
        return false;
    }
    report.check("golden records parse and validate", true);
    return true;
}

void golden_vectors(const std::wstring& cache, Report& report)
{
    Record one, two;
    if (!load_golden(cache, one, two, report)) return;

    struct Vector {
        const char* name;
        const Record* record;
        std::uint64_t key;
        std::size_t bytes;
        std::size_t code_bytes;
        const char* record_sha;
        const char* code_sha;
    };
    const Vector vectors[] = {
        {"1d", &one, x2w_native::golden_key_1d, 336, 124,
         "420b4d3125fcf4b1b149c614261b388885aa5992eaf7955510b54bc4e44a6ddd",
         "57f31991be3740eecab63ab25718862c43f22c6e3d4a8a1db1212458aa6a49e5"},
        {"2d", &two, x2w_native::golden_key_2d, 340, 128,
         "15c4cc211a18a2464ad99779f074150cd951704c77ac21f72135239bcddd67fd",
         "8374d7b39c8eb4c298aaa6965165b58b564d32b95879f3cb957eec4aeea79149"},
    };
    for (const Vector& vector : vectors) {
        const Record& record = *vector.record;
        std::vector<unsigned char> bytes;
        std::string detail;
        x2w_native::serialize_record(record, bytes, detail);
        const std::string tag = std::string("record/") + vector.name;
        report.check(tag + " length " + std::to_string(vector.bytes), bytes.size() == vector.bytes);
        report.check(tag + " canonical sha256",
                     x2w_native::digest_hex(x2w_native::record_digest(record)) == vector.record_sha,
                     x2w_native::digest_hex(x2w_native::record_digest(record)));
        report.check(tag + " FNV cache key",
                     x2w_native::record_key(record) == vector.key &&
                         x2w_native::key_text(x2w_native::record_key(record)) ==
                             x2w_native::key_text(vector.key),
                     x2w_native::key_text(x2w_native::record_key(record)));
        report.check(tag + " native code length " + std::to_string(vector.code_bytes),
                     record.code.size() == vector.code_bytes);
        report.check(tag + " native code sha256",
                     x2w_native::digest_hex(x2w_native::sha256(record.code.data(), record.code.size())) ==
                         vector.code_sha);
        report.check(tag + " root contract",
                     record.root_count == 3 && record.roots[0].type == 3 &&
                         record.roots[0].reg == 0 && record.roots[0].count == 1 &&
                         record.roots[1].type == 4 && record.roots[1].reg == 0 &&
                         record.roots[1].count == 1 && record.roots[2].type == 1 &&
                         record.roots[2].reg == 0 && record.roots[2].count == 16);
        report.check(tag + " guest wave32", record.guest_wave == 32);
        report.check(tag + " rsrc2 not interpreted",
                     record.rsrc2_interpretation == 0 && record.recorded_rsrc2 == 0);
    }
    report.check("golden 1d group and dispatch",
                 one.group_x == 32 && one.group_y == 1 && one.group_z == 1 &&
                     one.dispatch_x == 6 && one.dispatch_y == 1 && one.dispatch_z == 1 &&
                     one.profile == 1 && one.address_layout == 1);
    report.check("golden 2d group and dispatch",
                 two.group_x == 8 && two.group_y == 4 && two.group_z == 1 &&
                     two.dispatch_x == 3 && two.dispatch_y == 2 && two.dispatch_z == 1 &&
                     two.profile == 2 && two.address_layout == 2 && two.u0_stride == 96);

    // Dispatch-field sensitivity: one changed scalar yields a different key.
    Record changed = two;
    changed.dispatch_x = 4;
    report.check("dispatch sensitivity key 3be6db162186a222",
                 x2w_native::key_text(x2w_native::record_key(changed)) == "3be6db162186a222",
                 x2w_native::key_text(x2w_native::record_key(changed)));

    // The FNV seed is the ASCII revision; the digest is hashed as 32 raw bytes, not its hex text.
    const auto digest = x2w_native::record_digest(two);
    const std::uint64_t seeded =
        x2w_native::fnv_bytes(x2w_native::revision, sizeof(x2w_native::revision) - 1);
    report.check("FNV hashes the raw record digest",
                 x2w_native::fnv_bytes(digest.data(), digest.size(), seeded) ==
                     x2w_native::record_key(two));
}

void record_controls(const std::wstring& cache, Report& report)
{
    Record one, two;
    if (!load_golden(cache, one, two, report)) return;
    const Record& base = two;
    std::string detail;
    const auto invalid = [&report, &base](const char* name,
                                          const std::function<void(Record&)>& mutate) {
        std::string why;
        const bool rejected = record_invalid(base, mutate, why);
        report.check(std::string("record: ") + name + " is refused", rejected,
                     rejected ? why : "accepted");
    };

    invalid("version 2", [](Record& r) { r.version = 2; });
    invalid("stage 4", [](Record& r) { r.stage = 4; });
    invalid("ISA 2", [](Record& r) { r.isa = 2; });
    invalid("profile 3", [](Record& r) { r.profile = 3; });
    invalid("guest wave 64", [](Record& r) { r.guest_wave = 64; });
    invalid("translator revision 2", [](Record& r) { r.translator_revision = 2; });
    invalid("decoder identity 2", [](Record& r) { r.decoder_id = 2; });
    invalid("DXC profile 1", [](Record& r) { r.dxc_profile = 1; });
    invalid("DXC flags 1", [](Record& r) { r.dxc_flags = 1; });
    invalid("rsrc2 interpretation 1", [](Record& r) { r.rsrc2_interpretation = 1; });
    invalid("recorded_rsrc2 0x984", [](Record& r) { r.recorded_rsrc2 = 0x984; });
    invalid("recorded_rsrc2 0xffffffff", [](Record& r) { r.recorded_rsrc2 = 0xFFFFFFFFu; });
    invalid("group axis 48", [](Record& r) { r.group_x = 48; });
    invalid("group axis 2048", [](Record& r) { r.group_x = 2048; });
    invalid("group axis zero", [](Record& r) { r.group_y = 0; });
    invalid("group product 2048", [](Record& r) {
        r.group_x = 1024;
        r.group_y = 2;
        r.address_layout = 2;
        r.u0_stride = 4096;
    });
    invalid("dispatch zero", [](Record& r) { r.dispatch_x = 0; });
    invalid("dispatch 65536", [](Record& r) { r.dispatch_z = 0x10000; });
    invalid("user SGPR 1", [](Record& r) { r.user_sgpr_count = 1; });
    invalid("user SGPR 17", [](Record& r) { r.user_sgpr_count = 17; });
    invalid("table pointer outside SGPRs", [](Record& r) { r.table_reg = 15; });
    invalid("group X collides with table", [](Record& r) { r.group_reg = r.table_reg; });
    invalid("group Y collides with table", [](Record& r) { r.group_reg_y = r.table_reg; });
    invalid("descriptor offset 2", [](Record& r) { r.descriptor_offset = 2; });
    invalid("descriptor offset 0x1000", [](Record& r) { r.descriptor_offset = 0x1000; });
    invalid("shared descriptor offset", [](Record& r) { r.constant_descriptor = r.descriptor_offset; });
    invalid("source stride 6", [](Record& r) { r.source_stride = 6; });
    invalid("address layout 3", [](Record& r) { r.address_layout = 3; });
    invalid("element width 8", [](Record& r) { r.element_bytes = 8; });
    invalid("column shift 3", [](Record& r) { r.column_shift = 3; });
    invalid("layout 1 on 2d group", [](Record& r) { r.address_layout = 1; });
    invalid("layout 1 u0 stride 8", [](Record& r) { r.u0_stride = 8; });
    invalid("layout 2 without group Y", [](Record& r) { r.group_reg_y = 0xFFFFFFFFu; });
    invalid("layout 2 short row stride", [](Record& r) { r.u0_stride = 4; });
    invalid("layout 2 unaligned stride", [](Record& r) { r.u0_stride = 98; });
    invalid("guard axis 2", [](Record& r) { r.guard_axis = 2; });
    invalid("guard compare 2", [](Record& r) { r.guard_op = 2; });
    invalid("bound constant 16", [](Record& r) { r.bound_constant = 16; });
    invalid("cap constant 16", [](Record& r) { r.cap_constant = 16; });
    invalid("reordered root parameters", [](Record& r) {
        const auto type = r.roots[0].type;
        r.roots[0].type = r.roots[1].type;
        r.roots[1].type = type;
    });
    invalid("root constants count 15", [](Record& r) { r.roots[2].count = 15; });
    invalid("profile 1d on 2d group", [](Record& r) { r.profile = 1; });
    invalid("profile 2d stride 92", [](Record& r) { r.u0_stride = 92; });
    invalid("dispatch row overruns stride", [](Record& r) { r.dispatch_x = 4; });
    invalid("code is not whole words", [](Record& r) { r.code.resize(2); r.code_length = 2; });
    invalid("code shorter than one word", [](Record& r) { r.code.clear(); r.code_length = 0; });
    invalid("code_length disagrees", [](Record& r) { r.code_length = static_cast<std::uint32_t>(r.code.size() - 4); });
    invalid("code digest mismatch", [](Record& r) { r.code_sha256[0] ^= 0xFF; });

    // Structural byte edits that never reach semantic validation.
    std::vector<unsigned char> bytes;
    if (x2w_native::serialize_record(base, bytes, detail)) {
        const auto rejected = [&report](const char* name, const std::vector<unsigned char>& data) {
            Record parsed;
            std::string why;
            const bool parsed_ok = x2w_native::parse_record(data.data(), data.size(), parsed, why);
            const bool rejected_ok = !parsed_ok || !x2w_native::validate_record(parsed, why);
            report.check(std::string("record: ") + name + " is refused", rejected_ok,
                         rejected_ok ? why : "accepted");
        };
        rejected("empty record", {});
        rejected("truncated record", std::vector<unsigned char>(bytes.begin(), bytes.begin() + 175));
        rejected("oversized record", std::vector<unsigned char>(x2w_native::max_record + 1, 0));
        std::vector<unsigned char> bad_magic = bytes;
        bad_magic[0] = 'Y';
        rejected("bad magic", bad_magic);
        std::vector<unsigned char> root_count = bytes;
        root_count[140] = 9;
        rejected("root count 9", root_count);
        std::vector<unsigned char> trailing = bytes;
        trailing.push_back(0);
        rejected("trailing data", trailing);
        std::vector<unsigned char> code_length = bytes;
        code_length[136] ^= 0x04;
        rejected("code length mismatch", code_length);
        std::vector<unsigned char> code_digest = bytes;
        code_digest[180] ^= 0xFF;
        rejected("code digest", code_digest);
        std::vector<unsigned char> root_count_two = bytes;
        root_count_two[140] = 2;
        rejected("root count 2", root_count_two);
    }
}

void cache_controls(const std::wstring& cache, Report& report)
{
    const std::vector<std::string> both = {"warp", "hardware"};
    for (const auto& entry : {std::make_pair("1d", x2w_native::golden_key_1d),
                              std::make_pair("2d", x2w_native::golden_key_2d)}) {
        Acceptance acceptance = x2w_native::inspect(cache, entry.second, both);
        report.check(std::string("cache: ") + entry.first + " is a hit",
                     acceptance.state == State::hit, acceptance.detail);
        report.check(std::string("cache: ") + entry.first + " returns a record and output",
                     acceptance.has_record && !acceptance.output.empty() &&
                         !acceptance.hlsl.empty());
        if (acceptance.accepted()) {
            const std::size_t expected =
                std::string(entry.first) == "1d" ? 3640u : 3660u;
            report.check(std::string("cache: ") + entry.first + " CSO size " +
                             std::to_string(expected),
                         acceptance.output.size() == expected);
            report.check(std::string("cache: ") + entry.first + " output digest matches manifest",
                         acceptance.fields["output_sha256"] ==
                             x2w_native::digest_hex(x2w_native::sha256(
                                 acceptance.output.data(), acceptance.output.size())));
            report.check(std::string("cache: ") + entry.first + " HLSL digest matches manifest",
                         acceptance.fields["hlsl_sha256"] ==
                             x2w_native::digest_hex(x2w_native::sha256(
                                 acceptance.hlsl.data(), acceptance.hlsl.size())));
        }
    }
    expect_state(report, "cache: required warp alone is a hit", cache,
                 x2w_native::golden_key_2d, {"warp"}, State::hit);
    expect_state(report, "cache: required hardware alone is a hit", cache,
                 x2w_native::golden_key_2d, {"hardware"}, State::hit);
    expect_state(report, "cache: empty required selection is never a hit", cache,
                 x2w_native::golden_key_2d, {}, State::error);
    expect_state(report, "cache: unknown required backend is an error", cache,
                 x2w_native::golden_key_2d, {"vulkan"}, State::error);

    x2w_native::Expected foreign = x2w_native::Expected::pinned();
    foreign.translator = "foreign/0000000000000000";
    foreign.decoder = "foreign/0000000000000000";
    foreign.pipeline = "0000000000000000";
    foreign.llvm = std::string(64, '0');
    foreign.compiler = std::string(64, '0');
    foreign.validator = std::string(64, '0');
    foreign.target = "foreign";
    foreign.dxc_flags = "foreign";
    const Acceptance stale =
        x2w_native::inspect(cache, x2w_native::golden_key_2d, both, foreign);
    report.check("cache: a foreign trusted identity makes the entry stale",
                 stale.state == State::stale, stale.detail);

    // Bounded reading: a real 3.6 KB artifact cannot be read under a smaller declared limit.
    std::vector<unsigned char> bounded;
    report.check("cache: bounded read refuses an oversized artifact",
                 x2w_native::read_bounded(stem_path(cache, x2w_native::golden_key_2d, L"", L".csn.cso"),
                                          1024, bounded) == x2w_native::ReadStatus::too_large);
}

void acceptance_controls(const std::wstring& cache, const std::wstring& legacy, Report& report)
{
    const std::uint64_t key = x2w_native::golden_key_2d;
    const std::vector<std::string> both = {"warp", "hardware"};
    const auto control = [&report, &cache, key, &both](const char* name, State expected,
                                                       const std::function<bool(Clone&)>& mutate,
                                                       const std::vector<std::string>& required) {
        Clone clone = clone_entry(cache, key, "control");
        if (!clone.ok) {
            report.check(std::string("manifest: ") + name, false, "clone failed");
            return;
        }
        if (mutate && !mutate(clone)) {
            report.check(std::string("manifest: ") + name, false, "mutation failed");
            remove_tree(clone.directory);
            return;
        }
        expect_state(report, std::string("manifest: ") + name, clone.directory, key, required,
                     expected);
        remove_tree(clone.directory);
    };

    // Missing mandatory fields, one at a time.
    const char* mandatory[] = {
        "key", "revision", "status", "record_version", "record_sha256", "code_bytes",
        "code_sha256", "group", "hlsl_sha256", "output_bytes", "output_sha256", "translator",
        "decoder", "pipeline", "llvm", "compiler", "validator", "target", "dxc_flags",
        "validation", "rts0", "gate", "signed",
    };
    for (const char* name : mandatory)
        control((std::string("missing ") + name + " is not a hit").c_str(), State::corrupt,
                [name](Clone& clone) {
                    return manifest_remove(clone.directory, clone.key, name);
                },
                both);

    control("duplicate field is corrupt", State::corrupt, [](Clone& clone) {
        std::string value;
        std::vector<std::string> lines;
        if (!manifest_lines(clone.directory, clone.key, lines)) return false;
        for (const std::string& line : lines)
            if (line.rfind("translator ", 0) == 0) value = line;
        if (value.empty()) return false;
        lines.push_back(value);
        return write_manifest(clone.directory, clone.key, lines);
    }, both);
    control("unknown field is corrupt", State::corrupt, [](Clone& clone) {
        return manifest_append(clone.directory, clone.key, "mystery 1");
    }, both);
    control("unknown probe backend is corrupt", State::corrupt, [](Clone& clone) {
        return manifest_append(clone.directory, clone.key,
                               "pso-vulkan adapter=warp root=0x00000000 pso=0x00000000 shader_model=0x65");
    }, both);
    control("bad header is corrupt", State::corrupt, [](Clone& clone) {
        std::vector<std::string> lines;
        if (!manifest_lines(clone.directory, clone.key, lines)) return false;
        lines[0] = "x2w-native-compute 1";
        return write_manifest(clone.directory, clone.key, lines);
    }, both);

    struct Field {
        const char* name;
        const char* value;
        State state;
    };
    const Field fields[] = {
        {"key", "0000000000000000", State::stale},
        {"revision", "x2w-startup-compute-native-1", State::stale},
        {"status", "request", State::corrupt},
        {"record_version", "2", State::corrupt},
        {"record_sha256", "0000000000000000000000000000000000000000000000000000000000000000", State::corrupt},
        {"code_bytes", "1", State::corrupt},
        {"code_sha256", "0000000000000000000000000000000000000000000000000000000000000000", State::corrupt},
        {"group", "1x1x1", State::corrupt},
        {"hlsl_sha256", "0000000000000000000000000000000000000000000000000000000000000000", State::corrupt},
        {"output_bytes", "1", State::corrupt},
        {"output_sha256", "0000000000000000000000000000000000000000000000000000000000000000", State::corrupt},
        {"translator", "compute_store_aot/0000000000000000", State::stale},
        {"decoder", "pixel_aot/0000000000000000", State::stale},
        {"pipeline", "0000000000000000", State::stale},
        {"llvm", "0000000000000000000000000000000000000000000000000000000000000000", State::stale},
        {"compiler", "0000000000000000000000000000000000000000000000000000000000000000", State::stale},
        {"validator", "0000000000000000000000000000000000000000000000000000000000000000", State::stale},
        {"target", "dxil-ms-dx1", State::stale},
        {"dxc_flags", "cs_6_1 -E main", State::stale},
        {"validation", "container=0x80070057 module=0x00000000", State::corrupt},
        {"rts0", "absent", State::corrupt},
        {"gate", "DENY", State::corrupt},
        {"signed", "unsigned", State::corrupt},
    };
    for (const Field& field : fields)
        control((std::string(field.name) + " " + field.value + " fails closed").c_str(),
                field.state,
                [&field](Clone& clone) {
                    return manifest_set(clone.directory, clone.key, field.name, field.value);
                },
                both);

    control("missing pso-warp is backend-missing", State::backend_missing, [](Clone& clone) {
        return manifest_remove(clone.directory, clone.key, "pso-warp");
    }, both);
    control("missing pso-hardware is a hit when only warp is required", State::hit,
            [](Clone& clone) {
                return manifest_remove(clone.directory, clone.key, "pso-hardware");
            },
            {"warp"});
    control("failed pso-hardware is backend-missing", State::backend_missing, [](Clone& clone) {
        std::vector<std::string> lines;
        if (!manifest_lines(clone.directory, clone.key, lines)) return false;
        for (std::string& line : lines) {
            if (line.rfind("pso-hardware ", 0) == 0) {
                const std::size_t pso = line.find("pso=");
                if (pso != std::string::npos) line.replace(pso, 14, "pso=0x80070057");
            }
        }
        return write_manifest(clone.directory, clone.key, lines);
    }, both);
    control("failed root on pso-hardware is backend-missing", State::backend_missing,
            [](Clone& clone) {
                std::vector<std::string> lines;
                if (!manifest_lines(clone.directory, clone.key, lines)) return false;
                for (std::string& line : lines) {
                    if (line.rfind("pso-hardware ", 0) == 0) {
                        const std::size_t root = line.find("root=");
                        if (root != std::string::npos) line.replace(root, 15, "root=0x80070057");
                    }
                }
                return write_manifest(clone.directory, clone.key, lines);
            },
            both);
    control("a warp line with a hardware adapter is backend-missing", State::backend_missing,
            [](Clone& clone) {
                std::vector<std::string> lines;
                if (!manifest_lines(clone.directory, clone.key, lines)) return false;
                for (std::string& line : lines) {
                    if (line.rfind("pso-warp ", 0) == 0)
                        line = "pso-warp adapter=8086:9a49 root=0x00000000 pso=0x00000000 "
                               "shader_model=0x65";
                }
                return write_manifest(clone.directory, clone.key, lines);
            },
            both);
    control("a hardware line with a warp adapter is backend-missing", State::backend_missing,
            [](Clone& clone) {
                std::vector<std::string> lines;
                if (!manifest_lines(clone.directory, clone.key, lines)) return false;
                for (std::string& line : lines) {
                    if (line.rfind("pso-hardware ", 0) == 0)
                        line = "pso-hardware adapter=warp root=0x00000000 pso=0x00000000 "
                               "shader_model=0x65";
                }
                return write_manifest(clone.directory, clone.key, lines);
            },
            both);
    control("a hardware line without a vendor:device adapter is backend-missing",
            State::backend_missing,
            [](Clone& clone) {
                std::vector<std::string> lines;
                if (!manifest_lines(clone.directory, clone.key, lines)) return false;
                for (std::string& line : lines) {
                    if (line.rfind("pso-hardware ", 0) == 0)
                        line = "pso-hardware adapter=intel root=0x00000000 pso=0x00000000 "
                               "shader_model=0x65";
                }
                return write_manifest(clone.directory, clone.key, lines);
            },
            both);

    // Artifacts.
    control("missing CSO is corrupt", State::corrupt, [](Clone& clone) {
        return DeleteFileW(stem_path(clone.directory, clone.key, L"", L".csn.cso").c_str()) != 0;
    }, both);
    control("missing HLSL is corrupt", State::corrupt, [](Clone& clone) {
        return DeleteFileW(stem_path(clone.directory, clone.key, L"", L".csn.hlsl").c_str()) != 0;
    }, both);
    control("missing request is corrupt", State::corrupt, [](Clone& clone) {
        return DeleteFileW(stem_path(clone.directory, clone.key, L"req_", L".csn").c_str()) != 0;
    }, both);
    control("missing manifest is missing", State::missing, [](Clone& clone) {
        return DeleteFileW(
                   stem_path(clone.directory, clone.key, L"", L".csn.manifest").c_str()) != 0;
    }, both);
    control("oversized request is corrupt", State::corrupt, [](Clone& clone) {
        std::vector<unsigned char> large(x2w_native::max_record + 1, 0);
        return write_all(stem_path(clone.directory, clone.key, L"req_", L".csn"),
                         large.data(), large.size());
    }, both);
    control("oversized HLSL is corrupt", State::corrupt, [](Clone& clone) {
        std::vector<unsigned char> large(x2w_native::max_hlsl + 1, 'x');
        return write_all(stem_path(clone.directory, clone.key, L"", L".csn.hlsl"),
                         large.data(), large.size());
    }, both);
    control("oversized CSO is corrupt", State::corrupt, [](Clone& clone) {
        std::vector<unsigned char> large(x2w_native::max_output + 1, 0);
        return write_all(stem_path(clone.directory, clone.key, L"", L".csn.cso"),
                         large.data(), large.size());
    }, both);
    control("tampered CSO is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_output(clone.directory, clone.key,
                             [](std::vector<unsigned char>& output) {
                                 if (output.empty()) return false;
                                 output.back() ^= 0xFF;
                                 return true;
                             },
                             false);
    }, both);
    control("tampered HLSL is corrupt", State::corrupt, [](Clone& clone) {
        std::vector<unsigned char> hlsl;
        const std::wstring path = stem_path(clone.directory, clone.key, L"", L".csn.hlsl");
        if (!read_all(path, hlsl) || hlsl.empty()) return false;
        hlsl[0] ^= 0x01;
        return write_all(path, hlsl.data(), hlsl.size());
    }, both);
    control("zero DXBC digest is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_output(clone.directory, clone.key,
                             [](std::vector<unsigned char>& output) {
                                 if (output.size() < 20) return false;
                                 std::fill(output.begin() + 4, output.begin() + 20, 0);
                                 return true;
                             },
                             true);
    }, both);
    control("missing DXIL part is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_output(clone.directory, clone.key,
                             [](std::vector<unsigned char>& output) {
                                 std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
                                 std::string detail;
                                 if (!x2w_native::dxbc_parts(output.data(), output.size(), parts,
                                                             detail) ||
                                     !parts.count("DXIL"))
                                     return false;
                                 const std::size_t tag = parts["DXIL"].first - 8;
                                 std::memcpy(output.data() + tag, "DXXL", 4);
                                 return true;
                             },
                             true);
    }, both);
    control("missing RTS0 part is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_output(clone.directory, clone.key,
                             [](std::vector<unsigned char>& output) {
                                 std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
                                 std::string detail;
                                 if (!x2w_native::dxbc_parts(output.data(), output.size(), parts,
                                                             detail) ||
                                     !parts.count("RTS0"))
                                     return false;
                                 const std::size_t tag = parts["RTS0"].first - 8;
                                 std::memcpy(output.data() + tag, "RTS1", 4);
                                 return true;
                             },
                             true);
    }, both);
    control("malformed part offset is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_output(clone.directory, clone.key,
                             [](std::vector<unsigned char>& output) {
                                 if (output.size() < 36) return false;
                                 output[32] = 4;
                                 output[33] = output[34] = output[35] = 0;
                                 return true;
                             },
                             true);
    }, both);
    control("root parameter order is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key,
                           [](std::vector<unsigned char>& rts0) {
                               if (rts0.size() < 32) return false;
                               const std::uint32_t offset = rts0[8] | (rts0[9] << 8) |
                                                           (rts0[10] << 16) | (rts0[11] << 24);
                               if (static_cast<std::size_t>(offset) + 24 > rts0.size()) return false;
                               std::array<unsigned char, 12> first{};
                               std::copy(rts0.begin() + offset, rts0.begin() + offset + 12,
                                         first.begin());
                               std::copy(rts0.begin() + offset + 12, rts0.begin() + offset + 24,
                                         rts0.begin() + offset);
                               std::copy(first.begin(), first.end(), rts0.begin() + offset + 12);
                               return true;
                           });
    }, both);
    control("root flags are corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key, [](std::vector<unsigned char>& rts0) {
            if (rts0.size() < 24) return false;
            rts0[20] = 1;
            rts0[21] = rts0[22] = rts0[23] = 0;
            return true;
        });
    }, both);
    control("root version 1 is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key, [](std::vector<unsigned char>& rts0) {
            if (rts0.size() < 4) return false;
            rts0[0] = 1;
            rts0[1] = rts0[2] = rts0[3] = 0;
            return true;
        });
    }, both);
    control("static samplers are corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key, [](std::vector<unsigned char>& rts0) {
            if (rts0.size() < 16) return false;
            rts0[12] = 1;
            rts0[13] = rts0[14] = rts0[15] = 0;
            return true;
        });
    }, both);
    control("root parameter visibility is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key, [](std::vector<unsigned char>& rts0) {
            if (rts0.size() < 32) return false;
            const std::uint32_t offset = rts0[8] | (rts0[9] << 8) | (rts0[10] << 16) |
                                         (rts0[11] << 24);
            if (static_cast<std::size_t>(offset) + 8 > rts0.size()) return false;
            rts0[offset + 4] = 1;
            return true;
        });
    }, both);
    control("root constants count is corrupt", State::corrupt, [](Clone& clone) {
        return mutate_rts0(clone.directory, clone.key, [](std::vector<unsigned char>& rts0) {
            if (rts0.size() < 32) return false;
            const std::uint32_t offset = rts0[8] | (rts0[9] << 8) | (rts0[10] << 16) |
                                         (rts0[11] << 24);
            const std::size_t entry = static_cast<std::size_t>(offset) + 24;
            if (entry + 12 > rts0.size()) return false;
            const std::uint32_t data = rts0[entry + 8] | (rts0[entry + 9] << 8) |
                                       (rts0[entry + 10] << 16) | (rts0[entry + 11] << 24);
            if (static_cast<std::size_t>(data) + 12 > rts0.size()) return false;
            rts0[data + 8] = 15;
            rts0[data + 9] = rts0[data + 10] = rts0[data + 11] = 0;
            return true;
        });
    }, both);
    control("mixed-generation artifacts are corrupt", State::corrupt, [&cache](Clone& clone) {
        std::vector<unsigned char> other;
        if (!read_all(stem_path(cache, x2w_native::golden_key_1d, L"", L".csn.cso"), other))
            return false;
        return write_all(stem_path(clone.directory, clone.key, L"", L".csn.cso"), other.data(),
                         other.size());
    }, both);
    if (!legacy.empty() && x2w_native::path_exists(legacy)) {
        control("revision-1 manifest bytes are corrupt", State::corrupt, [&legacy](Clone& clone) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((legacy + L"\\*.csn.manifest").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) return false;
            std::vector<unsigned char> legacy_bytes;
            const std::wstring path = std::wstring(legacy) + L"\\" + data.cFileName;
            FindClose(find);
            if (!read_all(path, legacy_bytes)) return false;
            return write_all(stem_path(clone.directory, clone.key, L"", L".csn.manifest"),
                             legacy_bytes.data(), legacy_bytes.size());
        }, both);
    } else {
        report.note("the legacy revision-1 cache is absent; its manifest control is skipped");
    }
    control("foreign filename key is corrupt", State::corrupt, [&cache](Clone& clone) {
        std::vector<unsigned char> foreign;
        if (!read_all(stem_path(cache, x2w_native::golden_key_1d, L"req_", L".csn"), foreign))
            return false;
        return write_all(stem_path(clone.directory, clone.key, L"req_", L".csn"),
                         foreign.data(), foreign.size());
    }, both);
    control("error marker takes precedence", State::error, [](Clone& clone) {
        const std::string text = "retryable infrastructure failure\n";
        return write_text(stem_path(clone.directory, clone.key, L"", L".csn.error"), text);
    }, both);
    control("quarantine marker takes precedence", State::corrupt_request, [](Clone& clone) {
        const std::string text = "malformed request\n";
        return write_text(stem_path(clone.directory, clone.key, L"", L".csn.corrupt"), text);
    }, both);
    control("quarantine outranks an error marker", State::corrupt_request, [](Clone& clone) {
        return write_text(stem_path(clone.directory, clone.key, L"", L".csn.corrupt"), "bad\n") &&
               write_text(stem_path(clone.directory, clone.key, L"", L".csn.error"), "retry\n");
    }, both);
    control("oversized manifest is corrupt", State::corrupt, [](Clone& clone) {
        const std::string text(x2w_native::max_manifest + 1, 'x');
        return write_text(stem_path(clone.directory, clone.key, L"", L".csn.manifest"), text);
    }, both);

    // Refusal entries: revision-scoped permanence, identity still verified.
    control("refusal entry is a permanent refusal", State::refused, [](Clone& clone) {
        Record record;
        std::string detail;
        std::vector<unsigned char> bytes;
        if (!read_all(stem_path(clone.directory, clone.key, L"req_", L".csn"), bytes) ||
            !x2w_native::parse_record(bytes.data(), bytes.size(), record, detail) ||
            !x2w_native::validate_record(record, detail))
            return false;
        const std::string text =
            std::string(x2w_native::manifest_header) + "\n" +
            "key " + x2w_native::key_text(clone.key) + "\n" +
            "revision " + x2w_native::revision + "\n" + "status refused\n" +
            "record_version " + std::to_string(record.version) + "\n" +
            "record_sha256 " + x2w_native::digest_hex(x2w_native::record_digest(record)) + "\n" +
            "code_bytes " + std::to_string(record.code.size()) + "\n" +
            "code_sha256 " +
            x2w_native::digest_hex(x2w_native::sha256(record.code.data(), record.code.size())) +
            "\n" + "reason Unsupported program: unverified private LDS read\n";
        return write_text(stem_path(clone.directory, clone.key, L"", L".csn.manifest"), text);
    }, both);
    control("refusal with a foreign record digest is corrupt", State::corrupt,
            [](Clone& clone) {
                if (!manifest_set(clone.directory, clone.key, "status", "refused")) return false;
                return manifest_set(clone.directory, clone.key, "reason", "refused") &&
                       manifest_set(clone.directory, clone.key, "record_sha256",
                                    std::string(64, '0').c_str());
            },
            both);

    // Refusals are revision-scoped: a foreign dependency identity does not stale them.
    {
        Clone clone = clone_entry(cache, key, "refusal-identity");
        Record record;
        std::string detail;
        if (clone.ok) {
            std::vector<unsigned char> bytes;
            if (read_all(stem_path(clone.directory, key, L"req_", L".csn"), bytes) &&
                x2w_native::parse_record(bytes.data(), bytes.size(), record, detail) &&
                x2w_native::validate_record(record, detail)) {
                const std::string text =
                    std::string(x2w_native::manifest_header) + "\n" +
                    "key " + x2w_native::key_text(key) + "\n" +
                    "revision " + x2w_native::revision + "\n" + "status refused\n" +
                    "record_version " + std::to_string(record.version) + "\n" +
                    "record_sha256 " + x2w_native::digest_hex(x2w_native::record_digest(record)) +
                    "\n" + "code_bytes " + std::to_string(record.code.size()) + "\n" +
                    "code_sha256 " +
                    x2w_native::digest_hex(
                        x2w_native::sha256(record.code.data(), record.code.size())) +
                    "\n" + "reason revision-scoped refusal\n";
                write_text(stem_path(clone.directory, key, L"", L".csn.manifest"), text);
                x2w_native::Expected foreign = x2w_native::Expected::pinned();
                foreign.translator = "foreign/0000000000000000";
                foreign.llvm = std::string(64, '0');
                const Acceptance acceptance = x2w_native::inspect(clone.directory, key, both, foreign);
                report.check("refusal: a foreign identity does not invalidate a revision-scoped refusal",
                             acceptance.state == State::refused, acceptance.detail);
            } else {
                report.check("refusal: a foreign identity does not invalidate a revision-scoped refusal",
                             false, "could not build the refusal");
            }
        }
        remove_tree(clone.directory);
    }

    // Quarantine repair: the marker is per key; removing it restores the accepted entry.
    {
        Clone clone = clone_entry(cache, key, "repair");
        if (clone.ok) {
            write_text(stem_path(clone.directory, key, L"", L".csn.corrupt"), "bad\n");
            expect_state(report, "quarantine: marker is corrupt-request", clone.directory, key,
                         both, State::corrupt_request);
            DeleteFileW(stem_path(clone.directory, key, L"", L".csn.corrupt").c_str());
            expect_state(report, "quarantine: a repaired request is a hit again", clone.directory,
                         key, both, State::hit);
        }
        remove_tree(clone.directory);
    }
}

void legacy_controls(const std::wstring& legacy, Report& report)
{
    if (legacy.empty() || GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) {
        report.note("legacy revision-1 cache is absent; the revision-1 rejection control is skipped");
        return;
    }
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((legacy + L"\\*.csn.manifest").c_str(), &data);
    int inspected = 0;
    bool rejected = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = data.cFileName;
            const std::size_t dot = name.find(L'.');
            if (dot != 16) continue;
            std::uint64_t key = 0;
            std::string text;
            for (std::size_t i = 0; i < dot; ++i) text.push_back(static_cast<char>(name[i]));
            if (!x2w_native::parse_key_text(text.c_str(), key)) continue;
            ++inspected;
            const Acceptance acceptance =
                x2w_native::inspect(legacy, key, {"warp", "hardware"});
            if (acceptance.state == State::hit) {
                rejected = false;
                report.check("legacy: revision-1 entry " + text + " is rejected", false,
                             "it was accepted as a hit");
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    if (inspected == 0) {
        report.note("the legacy revision-1 cache has no manifest; the revision-1 rejection control is skipped");
        return;
    }
    report.check("legacy: every revision-1 entry is rejected (" + std::to_string(inspected) +
                     " inspected)",
                 rejected);
}

void writer_controls(const std::wstring& cache, Report& report)
{
    const std::uint64_t key = x2w_native::golden_key_2d;
    Record record, other_record;
    std::string detail;
    std::vector<unsigned char> bytes, other_bytes;
    if (!read_all(stem_path(cache, key, L"req_", L".csn"), bytes) ||
        !x2w_native::parse_record(bytes.data(), bytes.size(), record, detail) ||
        !x2w_native::validate_record(record, detail) ||
        !read_all(stem_path(cache, x2w_native::golden_key_1d, L"req_", L".csn"), other_bytes) ||
        !x2w_native::parse_record(other_bytes.data(), other_bytes.size(), other_record, detail) ||
        !x2w_native::validate_record(other_record, detail)) {
        report.check("writer: the golden records are available", false, detail);
        return;
    }

    const auto fresh = [key, &bytes](const char* tag) {
        const std::wstring directory = make_temp_dir(tag);
        write_all(stem_path(directory, key, L"req_", L".csn"), bytes.data(), bytes.size());
        return directory;
    };

    // An accepted entry is not rewritten: the translated receipt must survive.
    Clone clone = clone_entry(cache, key, "writer-hit");
    if (clone.ok) {
        std::string before;
        read_text(stem_path(clone.directory, key, L"", L".csn.src"), before);
        State state = State::missing;
        const bool published =
            x2w_native::publish_request(clone.directory, key, record, state, detail);
        std::string after;
        read_text(stem_path(clone.directory, key, L"", L".csn.src"), after);
        report.check("writer: an accepted entry is not rewritten",
                     published && state == State::hit && before == after &&
                         after.find("\nstatus translated\n") != std::string::npos,
                     detail);
        remove_tree(clone.directory);
    } else {
        report.check("writer: clone for the accepted-entry control", false, "clone failed");
    }

    std::wstring work = fresh("writer-publish");
    const std::wstring lock_path = stem_path(work, key, L"", L".csn.lock");
    const std::wstring request_path = stem_path(work, key, L"req_", L".csn");
    State state = State::missing;
    report.check("writer: a request publishes under an exclusive lock",
                 x2w_native::publish_request(work, key, record, state, detail) &&
                     state == State::missing && !x2w_native::path_exists(lock_path),
                 detail);
    std::string receipt;
    report.check("writer: the request receipt has status request",
                 read_text(stem_path(work, key, L"", L".csn.src"), receipt) &&
                     receipt.find("\nstatus request\n") != std::string::npos);

    // Contention: a held lock is busy, mutates nothing and writes no error marker.
    x2w_native::Writer held;
    std::string lock_detail;
    if (x2w_native::acquire_lock(work, key, held, lock_detail) ==
        x2w_native::LockResult::acquired) {
        std::vector<unsigned char> before;
        read_all(request_path, before);
        const bool published = x2w_native::publish_request(work, key, record, state, detail);
        std::vector<unsigned char> after;
        read_all(request_path, after);
        report.check("writer: contention is busy without mutation",
                     !published && state == State::busy && before == after &&
                         !x2w_native::path_exists(
                             stem_path(work, key, L"", L".csn.error")),
                     detail);
        // Wrong-owner cleanup must not remove another writer's lock.
        x2w_native::Writer other = held;
        other.token = "not-the-owner";
        x2w_native::release_lock(other);
        report.check("writer: a wrong-owner release leaves the lock",
                     x2w_native::path_exists(lock_path));
        x2w_native::release_lock(held);
        report.check("writer: the owner release removes the lock", !x2w_native::path_exists(lock_path));
    } else {
        report.check("writer: a lock can be acquired", false, lock_detail);
    }

    // An old lock is never reclaimed by age.
    HANDLE old = CreateFileW(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (old != INVALID_HANDLE_VALUE) {
        const char token[] = "1-old\n";
        DWORD written = 0;
        WriteFile(old, token, sizeof(token) - 1, &written, nullptr);
        CloseHandle(old);
        SYSTEMTIME past{};
        past.wYear = 2010;
        past.wMonth = 1;
        past.wDay = 1;
        FILETIME time{};
        SystemTimeToFileTime(&past, &time);
        HANDLE file = CreateFileW(lock_path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            SetFileTime(file, nullptr, nullptr, &time);
            CloseHandle(file);
        }
        const bool published = x2w_native::publish_request(work, key, record, state, detail);
        report.check("writer: an old held lock is still busy", !published && state == State::busy,
                     detail);
        DeleteFileW(lock_path.c_str());
    }

    // A lost lock stops the writer before mutation.
    if (x2w_native::acquire_lock(work, key, held, lock_detail) ==
        x2w_native::LockResult::acquired) {
        DeleteFileW(lock_path.c_str());
        report.check("writer: assert_owner refuses a vanished lock",
                     !x2w_native::assert_owner(held, detail));
        x2w_native::release_lock(held);
    }

    // A corrupt marker is cleared by a successful request publication.
    write_text(stem_path(work, key, L"", L".csn.corrupt"), "bad\n");
    report.check("writer: a valid request clears the quarantine",
                 x2w_native::publish_request(work, key, record, state, detail) &&
                     !x2w_native::path_exists(stem_path(work, key, L"", L".csn.corrupt")),
                 detail);

    // A record that does not hash to the filename key is never published.
    {
        const std::wstring mismatch = make_temp_dir("writer-mismatch");
        State refused = State::hit;
        report.check("writer: a record for another key is refused",
                     !x2w_native::publish_request(mismatch, key, other_record, refused, detail) &&
                         refused == State::corrupt &&
                         !x2w_native::path_exists(
                             stem_path(mismatch, key, L"", L".csn.src")),
                     detail);
        remove_tree(mismatch);
    }

    // Failed publication: a nonexistent directory is an error, not a claimed refusal.
    State failed = State::hit;
    const std::wstring missing = work + L"\\does-not-exist";
    report.check("writer: a failed publication is an error",
                 !x2w_native::publish_request(missing, key, record, failed, detail) &&
                     failed == State::error,
                 detail);

    remove_tree(work);
}

// ---------------------------------------------------------------- adapters

bool create_adapter(const char* backend, ComPtr<IDXGIAdapter>& adapter, DXGI_ADAPTER_DESC& desc)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (!std::strcmp(backend, "warp")) {
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        return SUCCEEDED(adapter->GetDesc(&desc));
    }
    ComPtr<IDXGIAdapter1> fallback;
    DXGI_ADAPTER_DESC1 fallback_desc{};
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
            desc = DXGI_ADAPTER_DESC{};
            adapter->GetDesc(&desc);
            return true;
        }
        if (!fallback) {
            fallback = candidate;
            fallback_desc = candidate_desc;
        }
    }
    if (fallback) {
        adapter = fallback;
        const HRESULT hr = adapter->GetDesc(&desc);
        return SUCCEEDED(hr);
    }
    return false;
}

bool create_desktop_device(IDXGIAdapter* adapter, bool debug, ComPtr<ID3D12Device>& device)
{
    if (debug) {
        ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
    }
    return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))) &&
           device;
}

unsigned shader_model(ID3D12Device* device)
{
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        return static_cast<unsigned>(model.HighestShaderModel);
    model.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))))
        return static_cast<unsigned>(model.HighestShaderModel);
    return 0;
}

void check_contract(Report& report, const std::string& tag,
                    const x2w_native::RootContract& contract)
{
    report.check(tag + " root contract version 1.1", contract.version == 2);
    report.check(tag + " root contract has three parameters", contract.parameters.size() == 3);
    report.check(tag + " root contract SRV t0", contract.parameters.size() > 0 &&
                                                     contract.parameters[0].type == 3 &&
                                                     contract.parameters[0].reg == 0 &&
                                                     contract.parameters[0].space == 0 &&
                                                     contract.parameters[0].visibility == 0);
    report.check(tag + " root contract UAV u0", contract.parameters.size() > 1 &&
                                                     contract.parameters[1].type == 4 &&
                                                     contract.parameters[1].reg == 0 &&
                                                     contract.parameters[1].space == 0 &&
                                                     contract.parameters[1].visibility == 0);
    report.check(tag + " root contract constants b0 (16)",
                 contract.parameters.size() > 2 && contract.parameters[2].type == 1 &&
                     contract.parameters[2].reg == 0 && contract.parameters[2].space == 0 &&
                     contract.parameters[2].has_count && contract.parameters[2].count == 16);
    report.check(tag + " root contract no samplers and no flags",
                 contract.samplers == 0 && contract.flags == 0);
}

void device_controls(const char* backend, bool debug, const std::wstring& cache, Report& report,
                     ComPtr<ID3D12Device>& device)
{
    for (const auto& entry : {std::make_pair("1d", x2w_native::golden_key_1d),
                              std::make_pair("2d", x2w_native::golden_key_2d)}) {
        const Acceptance acceptance =
            x2w_native::inspect(cache, entry.second, {"warp", "hardware"});
        if (!acceptance.accepted()) {
            report.check(std::string("device: ") + entry.first + " acceptance", false,
                         acceptance.detail);
            continue;
        }
        x2w_native::Pipeline pipeline;
        std::string detail;
        const HRESULT hr =
            x2w_native::create_pipeline(device.Get(), acceptance.record, acceptance.output,
                                        pipeline, detail);
        report.check(std::string("device: ") + entry.first + " CreateRootSignature/PSO S_OK",
                     hr == S_OK && pipeline.root && pipeline.pso,
                     std::string("hr=0x") + std::to_string(static_cast<unsigned long>(hr)) + " " +
                         detail);
        check_contract(report, std::string("device: ") + entry.first, pipeline.contract);

        // A malformed container never reaches the device.
        x2w_native::Pipeline bad;
        std::vector<unsigned char> broken(acceptance.output);
        if (broken.size() > 4) broken[0] = 'Y';
        const HRESULT bad_hr =
            x2w_native::create_pipeline(device.Get(), acceptance.record, broken, bad, detail);
        report.check(std::string("device: ") + entry.first + " rejects a malformed container",
                     FAILED(bad_hr) && !bad.root && !bad.pso, detail);

        // A structurally accepted container with a tampered signed DXIL payload is rejected by
        // D3D12's own container handling. (A container digest covers the whole blob, so this fails
        // at deserialization before PSO creation; re-signing is out of scope.)
        std::map<std::string, std::pair<std::size_t, std::size_t>> parts;
        std::string part_detail;
        if (x2w_native::dxbc_parts(acceptance.output.data(), acceptance.output.size(), parts,
                                   part_detail) &&
            parts.count("DXIL")) {
            const auto dxil = parts["DXIL"];
            std::vector<unsigned char> corrupt_dxil(acceptance.output);
            const std::size_t at = dxil.first + (dxil.second > 32 ? dxil.second / 2 : 0);
            corrupt_dxil[at] ^= 0xFF;
            x2w_native::Pipeline bad_shader;
            const HRESULT shader_hr =
                x2w_native::create_pipeline(device.Get(), acceptance.record, corrupt_dxil,
                                            bad_shader, detail);
            report.check(std::string("device: ") + entry.first +
                             " rejects a tampered signed container",
                         FAILED(shader_hr) && !bad_shader.root && !bad_shader.pso, detail);
        }

        // A root from another device is not usable: the consumer never caches roots across devices.
        ComPtr<ID3D12Device> second;
        DXGI_ADAPTER_DESC unused{};
        ComPtr<IDXGIAdapter> other_adapter;
        const char* other_backend = std::strcmp(backend, "warp") ? "warp" : "hardware";
        if (create_adapter(other_backend, other_adapter, unused) &&
            create_desktop_device(other_adapter.Get(), false, second)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = pipeline.root;
            desc.CS = {acceptance.output.data(), static_cast<UINT>(acceptance.output.size())};
            ComPtr<ID3D12PipelineState> cross;
            const HRESULT cross_hr =
                second->CreateComputePipelineState(&desc, IID_PPV_ARGS(&cross));
            report.check(std::string("device: ") + entry.first +
                             " rejects a root from another device",
                         FAILED(cross_hr), "hr=" + std::to_string(static_cast<unsigned long>(cross_hr)));
        } else {
            report.note("device: the cross-device control is skipped (no second adapter)");
        }
    }
    x2w_native::Pipeline null_pipeline;
    std::string detail;
    report.check("device: a null device is refused",
                 x2w_native::create_pipeline(nullptr, Record{}, {}, null_pipeline, detail) ==
                         E_POINTER &&
                     !null_pipeline.root && !null_pipeline.pso);
    report.note("device: shader model 0x" + std::to_string(shader_model(device.Get())));

    if (debug) {
        ComPtr<ID3D12Debug> layer;
        const bool available = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        report.check("debug: the D3D12 debug interface is available", available);
        ComPtr<ID3D12InfoQueue> queue;
        if (SUCCEEDED(device.As(&queue))) {
            const UINT64 stored = queue->GetNumStoredMessages();
            UINT64 errors = 0;
            for (UINT64 i = 0; i < stored; ++i) {
                SIZE_T length = 0;
                if (FAILED(queue->GetMessage(i, nullptr, &length))) continue;
                std::vector<unsigned char> buffer(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(queue->GetMessage(i, message, &length))) continue;
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                    ++errors;
            }
            report.check("debug: the debug layer reports no error/corruption messages",
                         errors == 0, std::to_string(errors) + " of " + std::to_string(stored));
        } else {
            report.note("debug: the info queue is unavailable");
        }
    }
}

void wrapper_controls(const std::wstring& cache, Report& report, IDXGIAdapter* adapter)
{
    SetEnvironmentVariableA("X2W_D3D12_PERMISSIVE", "1");
    const char* backend_flag = std::getenv("X2W_D3D12_DEBUG");
    if (!backend_flag) SetEnvironmentVariableA("X2W_D3D12_DEBUG", "0");
    std::array<unsigned char, 64> parameters{};
    void* guest = nullptr;
    const HRESULT device_hr =
        D3D12XboxCreateDevice(adapter, parameters.data(), __uuidof(ID3D12Device), &guest);
    if (FAILED(device_hr) || !guest) {
        report.check("fixture: a wrapped guest device is available", false,
                     "hr=" + std::to_string(static_cast<unsigned long>(device_hr)));
        return;
    }
    report.check("fixture: a wrapped guest device is available", true);

    SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
    for (const auto& entry : {std::make_pair("1d", x2w_native::golden_key_1d),
                              std::make_pair("2d", x2w_native::golden_key_2d)}) {
        void* root = nullptr;
        void* pso = nullptr;
        int state = -1;
        char detail[512] = "";
        const std::string key = x2w_native::key_text(entry.second);
        const HRESULT hr = x2w_native_fixture_create(guest, cache.c_str(), key.c_str(), &root, &pso,
                                                     &state, detail, sizeof(detail));
        report.check(std::string("fixture: ") + entry.first + " creates a real pipeline",
                     hr == S_OK && root && pso && state == x2w_native::state_code(State::hit),
                     std::string("hr=0x") + std::to_string(static_cast<unsigned long>(hr)) + " " +
                         detail);
        if (hr == S_OK && pso) {
            report.check(std::string("fixture: ") + entry.first + " pipeline is real",
                         x2w_d3d12_pipeline_real(pso) == 1);
            int path = -1, cls = -1;
            unsigned long long info_key = 0;
            const int info_ok = x2w_native_fixture_info(pso, &path, &cls, &info_key);
            report.check(std::string("fixture: ") + entry.first + " classification",
                         info_ok == 1 && path == 162 && cls == 2 && info_key == entry.second,
                         "info=" + std::to_string(info_ok) + " path=" + std::to_string(path) +
                             " cls=" + std::to_string(cls));
            unsigned int types[8]{}, registers[8]{}, spaces[8]{}, counts[8]{}, used = 0;
            const int root_ok =
                x2w_native_fixture_root(root, types, registers, spaces, counts, 8, &used);
            report.check(std::string("fixture: ") + entry.first + " recorded root metadata",
                         root_ok == 1 && used == 3 && types[0] == 3 && types[1] == 4 &&
                             types[2] == 1 && registers[0] == 0 && registers[1] == 0 &&
                             registers[2] == 0 && spaces[0] == 0 && spaces[1] == 0 &&
                             spaces[2] == 0 && counts[2] == 16,
                         "root=" + std::to_string(root_ok) + " used=" + std::to_string(used) +
                             " types=" + std::to_string(types[0]) + "," +
                             std::to_string(types[1]) + "," + std::to_string(types[2]));
            x2w_native_fixture_release(pso);
        }
        x2w_native_fixture_release_root(root);
    }

    // Disabled by default.
    SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "0");
    void* root = reinterpret_cast<void*>(1);
    void* pso = reinterpret_cast<void*>(1);
    int state = -1;
    char detail[128] = "";
    report.check("fixture: activation is disabled by default",
                 x2w_native_fixture_create(guest, cache.c_str(), "d55528b6b2cfa1f8", &root, &pso,
                                           &state, detail, sizeof(detail)) == E_ACCESSDENIED &&
                     !root && !pso);
    SetEnvironmentVariableA("X2W_NATIVE_COMPUTE_FIXTURE", "1");
    report.check("fixture: an unapproved key is refused",
                 x2w_native_fixture_create(guest, cache.c_str(), "0000000000000000", &root, &pso,
                                           &state, detail, sizeof(detail)) == E_INVALIDARG &&
                     !root && !pso);
    report.check("fixture: a missing device is refused",
                 x2w_native_fixture_create(nullptr, cache.c_str(), "d55528b6b2cfa1f8", &root, &pso,
                                           &state, detail, sizeof(detail)) == E_POINTER &&
                     !root && !pso);

    // A non-hit entry never yields a pipeline.
    Clone clone = clone_entry(cache, x2w_native::golden_key_2d, "fixture-negative");
    if (clone.ok) {
        std::vector<unsigned char> cso;
        if (read_all(stem_path(clone.directory, clone.key, L"", L".csn.cso"), cso) &&
            !cso.empty()) {
            cso.back() ^= 0xFF;
            write_all(stem_path(clone.directory, clone.key, L"", L".csn.cso"), cso.data(),
                      cso.size());
        }
        const HRESULT hr = x2w_native_fixture_create(
            guest, clone.directory.c_str(), "95c4c65e03a8b336", &root, &pso, &state, detail,
            sizeof(detail));
        report.check("fixture: a corrupt entry never creates a pipeline",
                     hr == S_FALSE && !root && !pso &&
                         state == x2w_native::state_code(State::corrupt),
                     detail);
        remove_tree(clone.directory);
    }

    // A desktop device is not a wrapped guest device.
    ComPtr<ID3D12Device> desktop;
    if (create_desktop_device(adapter, false, desktop)) {
        report.check("fixture: a desktop device is not a wrapped device",
                     x2w_native_fixture_create(desktop.Get(), cache.c_str(),
                                               "d55528b6b2cfa1f8", &root, &pso, &state, detail,
                                               sizeof(detail)) == E_INVALIDARG &&
                         !root && !pso);
    }

    // The production slot-67 placeholder is unchanged: it still returns a placeholder with its own
    // classification, and the fixture identity is distinct.
    std::array<unsigned char, 0x300> desc{};
    void* slot67 = nullptr;
    const GUID pipeline_iid = __uuidof(ID3D12PipelineState);
    const HRESULT slot_hr = static_cast<HRESULT>(
        invoke(guest, 67, arg(desc.data()), arg(&pipeline_iid), arg(&slot67)));
    int path = -1, cls = -1;
    unsigned long long slot_key = 0;
    const int slot_info = slot67 ? x2w_native_fixture_info(slot67, &path, &cls, &slot_key) : 0;
    report.check("slot 67: the placeholder still succeeds",
                 slot_hr == S_OK && slot67 && x2w_d3d12_pipeline_real(slot67) == 0,
                 "hr=" + std::to_string(static_cast<unsigned long>(slot_hr)));
    report.check("slot 67: the placeholder keeps its own classification",
                 slot_info == 1 && path == 67 && cls == 1 && slot_key == 0,
                 "info=" + std::to_string(slot_info) + " path=" + std::to_string(path) +
                     " cls=" + std::to_string(cls));
    if (slot67) x2w_native_fixture_release(slot67);
}

// ---------------------------------------------------------------- evidence

void write_evidence(const Report& report, const std::string& mode, const std::string& adapter,
                    const std::string& extra)
{
    wchar_t directory[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"X2W_NATIVE_EVIDENCE", directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    CreateDirectoryW(directory, nullptr);
    const std::wstring base = std::wstring(directory) + L"\\" + std::wstring(mode.begin(), mode.end());

    std::string log;
    for (const auto& check : report.checks)
        log += std::string(check.second ? "PASS " : "FAIL ") + check.first + "\n";
    for (const auto& note : report.notes) log += "NOTE " + note + "\n";
    log += "adapter " + adapter + "\n";
    if (!extra.empty()) log += extra + "\n";
    log += std::string("result ") + (report.failures ? "FAIL" : "PASS") + "\n";
    write_text(base + L".log", log);

    const auto escape = [](const std::string& text) {
        std::string out;
        for (char c : text) {
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
    write_text(base + L".json", json);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: startup_compute_native <controls|warp|hardware|debug> <cache-dir> "
                     "[legacy-dir]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::wstring cache = wide(argv[2]);
    const std::wstring legacy = argc > 3 ? wide(argv[3]) : std::wstring();
    if (GetFileAttributesW(cache.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "the accepted native compute cache is absent: %s\n", argv[2]);
        return 77;
    }

    Report report;
    report.mode = mode;
    const bool device_mode = mode != "controls";
    const bool debug = mode == "debug";
    const bool warp = mode == "warp" || debug;

    std::string adapter_text = "none";
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    ComPtr<ID3D12Device> device;
    if (device_mode) {
        if (!create_adapter(warp ? "warp" : "hardware", adapter, adapter_desc)) return 77;
        char text[512];
        std::snprintf(text, sizeof(text), "%ls vendor=%04x device=%04x",
                      adapter_desc.Description, adapter_desc.VendorId, adapter_desc.DeviceId);
        adapter_text = text;
        if (!create_desktop_device(adapter.Get(), debug, device)) return 77;
    }

    golden_vectors(cache, report);
    record_controls(cache, report);
    cache_controls(cache, report);
    acceptance_controls(cache, legacy, report);
    legacy_controls(legacy, report);
    writer_controls(cache, report);

    std::string extra;
    if (device_mode)
        extra = "shader_model=" + std::to_string(shader_model(device.Get()));

    if (device_mode) device_controls(warp ? "warp" : "hardware", debug, cache, report, device);
    if (device_mode) wrapper_controls(cache, report, adapter.Get());

    write_evidence(report, mode, adapter_text, extra);
    std::printf("%s %s: %zu checks, %d failed\n", report.failures ? "FAIL" : "PASS", mode.c_str(),
                report.checks.size(), report.failures);
    return report.failures ? 1 : 0;
}
