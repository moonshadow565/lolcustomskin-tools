#pragma once
#include "process.hpp"
#include <cinttypes>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <filesystem>

namespace LCS {
    struct Process;

    struct ModOverlay {
        static char const SCHEMA[];
        static char const INFO[];
        uint32_t checksum = {};
        PtrStorage off_open = {};
        PtrStorage off_open_ref = {};
        PtrStorage off_wopen = {};
        PtrStorage off_ret = {};
        PtrStorage off_free_ptr = {};
        PtrStorage off_free_fn = {};

        void save(std::filesystem::path const& filename) const noexcept;
        void load(std::filesystem::path const& filename) noexcept;
        std::string to_string() const noexcept;
        void from_string(std::string const &) noexcept;
        bool check(Process const &process) const;
        void scan(Process const &process);
        void wait_patchable(Process const &process, uint32_t timeout = 60 * 1000);
        void patch(Process const &process, std::filesystem::path const& prefix) const;
    };
}
