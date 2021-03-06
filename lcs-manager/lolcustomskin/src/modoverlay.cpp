#define _CRT_SECURE_NO_WARNINGS
#include "modoverlay.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>
#include "ppp.hpp"

using namespace LCS;

constexpr auto const find_open = &ppp::any<
        "6A 03 68 00 00 00 00 C0 68 ?? ?? ?? ?? FF 15 o[u[?? ?? ?? ??]]"_pattern,
        "6A 00 56 55 50 FF 15 o[u[?? ?? ?? ??]] 8B F8"_pattern>;
constexpr auto const find_ret = &ppp::any<
        "57 8B CB E8 ?? ?? ?? ?? o[84] C0 75 12"_pattern>;
constexpr auto const find_wopen = &ppp::any<
        "6A 00 6A ?? 68 ?? ?? 12 00 ?? FF 15 u[?? ?? ?? ??]"_pattern>;
constexpr auto const find_free = &ppp::any<
        "A1 u[?? ?? ?? ??] 85 C0 74 09 3D ?? ?? ?? ?? 74 02 FF E0 o[FF 74 24 04] E8 ?? ?? ??"_pattern>;

void ModOverlay::save(std::filesystem::path const& filename) const noexcept {
    FILE* file = {};
#ifdef WIN32
    _wfopen_s(&file, filename.c_str(),  L"wb");
#else
    file = fopen(filename.c_str(), "wb");
#endif
    if (file) {
        auto str = to_string();
        fwrite(str.data(), 1, str.size(), file);
        fclose(file);
    }
}

void ModOverlay::load(std::filesystem::path const& filename) noexcept {
    FILE* file = {};
#ifdef WIN32
    _wfopen_s(&file, filename.c_str(),  L"rb");
#else
    file = fopen(filename.c_str(), "rb");
#endif
    if (file) {
        auto buffer = std::string();
        fseek(file, 0, SEEK_END);
        auto end = ftell(file);
        fseek(file, 0, SEEK_SET);
        buffer.resize((size_t)end);
        fread(buffer.data(), 1, buffer.size(), file);
        from_string(buffer);
    }
}

char const ModOverlay::SCHEMA[] = "lcs-overlay v5 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X";
char const ModOverlay::INFO[] = "lcs-overlay v5 checksum off_open off_open_ref off_wopen off_ret off_free_ptr off_free_fn";

std::string ModOverlay::to_string() const noexcept {
    char buffer[sizeof(SCHEMA) * 4] = {};
    int size = sprintf(buffer, SCHEMA, checksum, off_open, off_open_ref, off_wopen, off_ret, off_free_ptr, off_free_fn);
    return std::string(buffer, (size_t)size);
}

void ModOverlay::from_string(std::string const &buffer) noexcept {
    if (sscanf(buffer.c_str(), SCHEMA, &checksum, &off_open, &off_open_ref, &off_wopen, &off_ret, &off_free_ptr, &off_free_fn) != 7) {
        checksum = 0;
        off_open = 0;
        off_open_ref = 0;
        off_wopen = 0;
        off_ret = 0;
        off_free_ptr = 0;
        off_free_fn = 0;
    }
}

bool ModOverlay::check(LCS::Process const &process) const {
    return checksum && off_open && off_open_ref && off_wopen && off_ret && off_free_ptr && off_free_fn && process.Checksum() == checksum;
}

void ModOverlay::scan(LCS::Process const &process) {
    auto const base = process.Base();
    auto const data = process.Dump();

    auto const open_match = find_open(data, base);
    if (!open_match) {
        throw std::runtime_error("Failed to find fopen!");
    }
    auto const wopen_match = find_wopen(data, base);
    if (!wopen_match) {
        throw std::runtime_error("Failed to find wfopen!");
    }
    auto const ret_match = find_ret(data, base);
    if (!ret_match) {
        throw std::runtime_error("Failed to find ret!");
    }
    auto const free_match = find_free(data, base);
    if (!free_match) {
        throw std::runtime_error("Failed to find free!");
    }

    off_open_ref = process.Debase((PtrStorage)std::get<1>(*open_match));
    off_open = process.Debase((PtrStorage)std::get<2>(*open_match));
    off_wopen = process.Debase((PtrStorage)std::get<1>(*wopen_match));
    off_ret = process.Debase((PtrStorage)std::get<1>(*ret_match));
    off_free_ptr = process.Debase((PtrStorage)std::get<1>(*free_match));
    off_free_fn = process.Debase((PtrStorage)std::get<2>(*free_match));

    checksum = process.Checksum();
}

namespace {
    struct ImportTrampoline {
        uint8_t data[64] = {};
        static ImportTrampoline make(Ptr<uint8_t> where) {
            ImportTrampoline result = {
                { 0xB8u, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE0, }
            };
            memcpy(result.data + 1, &where.storage, sizeof(where.storage));
            return result;
        }
    };

    struct CodePayload {
        // Pointers to be consumed by shellcode
        Ptr<uint8_t> org_open_ptr = {};
        Ptr<char16_t> prefix_open_ptr = {};
        Ptr<uint8_t> wopen_ptr = {};
        Ptr<uint8_t> org_free_ptr = {};
        PtrStorage find_ret_addr = {};
        PtrStorage hook_ret_addr = {};

        // Actual data and shellcode storage
        uint8_t hook_open_data[0x80] = {
            0xc8, 0x00, 0x10, 0x00, 0x53, 0x57, 0x56, 0xe8,
            0x00, 0x00, 0x00, 0x00, 0x5b, 0x81, 0xe3, 0x00,
            0xf0, 0xff, 0xff, 0x8d, 0xbd, 0x00, 0xf0, 0xff,
            0xff, 0x8b, 0x73, 0x04, 0x66, 0xad, 0x66, 0xab,
            0x66, 0x85, 0xc0, 0x75, 0xf7, 0x83, 0xef, 0x02,
            0x8b, 0x75, 0x08, 0xb4, 0x00, 0xac, 0x3c, 0x2f,
            0x75, 0x02, 0xb0, 0x5c, 0x66, 0xab, 0x84, 0xc0,
            0x75, 0xf3, 0x8d, 0x85, 0x00, 0xf0, 0xff, 0xff,
            0xff, 0x75, 0x20, 0xff, 0x75, 0x1c, 0xff, 0x75,
            0x18, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10, 0xff,
            0x75, 0x0c, 0x50, 0xff, 0x53, 0x08, 0x83, 0xf8,
            0xff, 0x75, 0x17, 0xff, 0x75, 0x20, 0xff, 0x75,
            0x1c, 0xff, 0x75, 0x18, 0xff, 0x75, 0x14, 0xff,
            0x75, 0x10, 0xff, 0x75, 0x0c, 0xff, 0x75, 0x08,
            0xff, 0x13, 0x5e, 0x5f, 0x5b, 0xc9, 0xc2, 0x1c,
            0x00
        };
        uint8_t hook_free_data[0x80] = {
            0xc8, 0x00, 0x00, 0x00, 0x53, 0x56, 0xe8, 0x00,
            0x00, 0x00, 0x00, 0x5b, 0x81, 0xe3, 0x00, 0xf0,
            0xff, 0xff, 0x8b, 0x73, 0x10, 0x89, 0xe8, 0x05,
            0x80, 0x01, 0x00, 0x00, 0x83, 0xe8, 0x04, 0x39,
            0xe8, 0x74, 0x09, 0x3b, 0x30, 0x75, 0xf5, 0x8b,
            0x73, 0x14, 0x89, 0x30, 0x8b, 0x43, 0x0c, 0x5e,
            0x5b, 0xc9, 0xff, 0xe0
        };
        ImportTrampoline org_open_data = {};
        char16_t prefix_open_data[0x400] = {};
    };
}

void ModOverlay::wait_patchable(const Process &process, uint32_t timeout) {
    auto ptr_open = process.Rebase(off_open);
    auto ptr_open_ref = process.Rebase(off_open_ref);
    process.WaitPtrEq(ptr_open_ref, PtrStorage(ptr_open), 1, timeout);
    auto ptr_free = process.Rebase(off_free_ptr);
    process.WaitPtrNotEq(ptr_free, PtrStorage(0), 1, timeout);
    auto ptr_wopen = process.Rebase(off_wopen);
    process.WaitPtrNotEq(ptr_wopen, PtrStorage(0), 1, timeout);
}

void ModOverlay::patch(LCS::Process const &process, std::filesystem::path const& prefix) const {
    if (!check(process)) {
        throw std::runtime_error("Config invalid to patch this executable!");
    }
    std::u16string prefix_str = prefix.generic_u16string();
    for (auto& c: prefix_str) {
        if (c == u'/') {
            c = u'\\';
        }
    }
    prefix_str = u"\\\\?\\" + prefix_str;
    if (!prefix_str.ends_with(u"\\")) {
        prefix_str.push_back(u'\\');
    }
    if (prefix_str.size() * sizeof(char16_t) >= sizeof(CodePayload::prefix_open_data)) {
        throw std::runtime_error("Prefix too big!");
    }

    // Prepare pointers
    auto mod_code = process.Allocate<CodePayload>();
    auto ptr_open = Ptr<Ptr<ImportTrampoline>>(process.Rebase(off_open));
    auto org_open = Ptr<ImportTrampoline>{};
    process.Read(ptr_open, org_open);
    auto ptr_wopen = Ptr<Ptr<uint8_t>>(process.Rebase(off_wopen));
    auto wopen = Ptr<uint8_t>{};
    process.Read(ptr_wopen, wopen);
    auto find_ret_addr = process.Rebase(off_ret);
    auto ptr_free = Ptr<Ptr<uint8_t>>(process.Rebase(off_free_ptr));
    auto org_free_ptr = Ptr<uint8_t>(process.Rebase(off_free_fn));

    // Prepare payload
    auto payload = CodePayload {};
    payload.org_open_ptr = Ptr(mod_code->org_open_data.data);
    payload.prefix_open_ptr = Ptr(mod_code->prefix_open_data);
    payload.wopen_ptr = wopen;
    payload.org_free_ptr = org_free_ptr;
    payload.find_ret_addr = find_ret_addr;
    payload.hook_ret_addr = find_ret_addr + 0x16;

    process.Read(org_open, payload.org_open_data);
    std::copy_n(prefix_str.data(), prefix_str.size(), payload.prefix_open_data);

    // Write payload
    process.Write(mod_code, payload);
    process.MarkExecutable(mod_code);

    // Write hooks
    process.Write(ptr_free, Ptr(mod_code->hook_free_data));
    process.Write(org_open, ImportTrampoline::make(mod_code->hook_open_data));
}
