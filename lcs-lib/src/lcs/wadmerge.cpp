#include "wadmerge.hpp"
#include "error.hpp"
#include "progress.hpp"
#include "conflict.hpp"
#include <numeric>
#include <utility>
#include <unordered_set>
#include <picosha2.hpp>
#include <cstring>

using namespace LCS;

template<std::size_t S>
static void copyStream(InFile& source, std::int64_t offset, OutFile& dest,
                       std::size_t size, char (&buffer)[S],
                       Progress& progress) {
    source.seek(offset, SEEK_SET);
    for (std::size_t i = 0; i != size;) {
        std::size_t count = std::min(S, size - i);
        source.read(buffer, count);
        dest.write(buffer, count);
        // progress.consumeData(count);
        i += count;
    }
    progress.consumeData(size);
}

WadMerge::WadMerge(fs::path const& path, Wad const* original)
 : path_(fs::absolute(path)), original_(original) {
    lcs_trace_func(
                lcs_trace_var(this->path_),
                lcs_trace_var(this->original_->name())
                );
    lcs_assert_msg("Using league installation with 3.0 wads!", !original->is_oldchecksum());
    fs::create_directories(path_.parent_path());
    // TODO: potentially optimize by using the fact entries are sorted
    for(auto const& entry: original->entries()) {
        entries_.insert_or_assign(entry.xxhash, Entry { entry, original, EntryKind::Original });
        orgchecksum_.insert_or_assign(entry.xxhash, entry.checksum);
    }
}

void WadMerge::addWad(const Wad* source, Conflict conflict) {
    lcs_trace_func(
                lcs_trace_var(this->path_),
                lcs_trace_var(this->original_->name()),
                lcs_trace_var(source->path())
                );
    lcs_assert_msg("Mods using 3.0 wads need to be re-installed!", !source->is_oldchecksum());
    for(auto const& entry: source->entries()) {
        addWadEntry(entry, source, conflict, EntryKind::Full);
    }
}

void WadMerge::addExtraEntry(const Wad::Entry& entry, const Wad* source, Conflict conflict) {
    lcs_trace_func(
                lcs_trace_var(this->path_),
                lcs_trace_var(this->original_->name()),
                lcs_trace_var(source->path())
                );
    lcs_assert_msg("Mods using 3.0 wads need to be re-installed!", !source->is_oldchecksum());
    addWadEntry(entry, source, conflict, EntryKind::Extra);
}

void WadMerge::addWadEntry(Wad::Entry const& entry, Wad const* source,
                           Conflict conflict, EntryKind kind) {
    lcs_assert_msg("Mods using 3.0 wads need to be re-installed!", !source->is_oldchecksum());
    lcs_trace_func(
                lcs_trace_var(this->path_),
                lcs_trace_var(this->original_->name()),
                lcs_trace_var(source->path())
                );
    if (auto o = orgchecksum_.find(entry.xxhash); o != orgchecksum_.end() && o->second == entry.checksum) {
        return;
    }
    if (auto i = entries_.find(entry.xxhash); i != entries_.end()) {
        if (i->second.checksum != entry.checksum && i->second.kind_ != EntryKind::Original) {
            if (conflict == Conflict::Skip) {
                return;
            } else if(conflict == Conflict::Abort) {
                lcs_hint("Trying to modify same file from multiple mods/wads!");
                throw ConflictError(entry.xxhash, i->second.wad_->path(), source->path());
            }
        }
        i->second = Entry { entry, source, kind };
        sizeCalculated_ = false;
    } else {
        entries_.insert(i, std::pair {entry.xxhash, Entry { entry, source, kind } });
        sizeCalculated_ = false;
    }
}

std::uint64_t WadMerge::size() const noexcept {
    if(!sizeCalculated_) {
        size_ = std::accumulate(entries_.begin(), entries_.end(), std::uint64_t{0},
                    [](std::uint64_t old, auto const& kvp) -> std::uint64_t {
            return old + kvp.second.sizeCompressed;
        });
        sizeCalculated_ = true;
    }
    return size_;
}

void WadMerge::write(Progress& progress) const {
    lcs_trace_func(
                lcs_trace_var(this->path_),
                lcs_trace_var(this->original_->name())
                );
    auto const totalSize = size();
    progress.startItem(path_, totalSize);
    auto newHeader = original_->header();
    newHeader.filecount = static_cast<std::uint32_t>(entries_.size());
    {
        auto sha256_hasher = picosha2::hash256_one_by_one{};
        for(auto const& [xxhash, entry]: entries_) {
            auto const beg = (char const*)(&entry);
            auto const end = beg + sizeof(Wad::Entry);
            sha256_hasher.process(beg, end);
        }
        sha256_hasher.finish();
        sha256_hasher.get_hash_bytes(newHeader.signature.begin(), newHeader.signature.end());
    }
    if (fs::exists(path_)) {
        try {
            auto oldHeader = Wad::Header{};
            auto infile = InFile(path_);
            infile.read((char*)&oldHeader, sizeof(Wad::Header));
            if (std::memcmp((char const*)&oldHeader, (char const*)&newHeader, sizeof(Wad::Header)) == 0) {
                progress.consumeData(totalSize);
                progress.finishItem();
                return;
            }
        } catch(std::runtime_error const&) {
            // we don't care...
        }
    } else {
        fs::create_directories(path_.parent_path());
    }
    auto outfile = OutFile(path_);
    auto newEntries = std::vector<Wad::Entry>(entries_.size());
    auto dataOffset = (std::uint64_t)(sizeof(Wad::Header) + entries_.size() * sizeof(Wad::Entry));
    std::transform(entries_.begin(), entries_.end(), newEntries.begin(),
                   [&dataOffset](std::pair<uint64_t,Wad::Entry> kvp) {
        kvp.second.dataOffset = static_cast<uint32_t>(dataOffset);
        dataOffset += kvp.second.sizeCompressed;
        constexpr auto GB = static_cast<std::uint64_t>(1024 * 1024 * 1024);
        lcs_assert(dataOffset <= 2 * GB);
        return kvp.second;
    });
    outfile.write((char const*)&newHeader, sizeof(Wad::Header));
    outfile.write((char const*)newEntries.data(), newEntries.size() * sizeof(Wad::Entry));
    char buffer[64 * 1024];
    for(auto const& [xxhash, entry]: entries_) {
        InFile infile(entry.wad_->path());
        copyStream(infile, entry.dataOffset, outfile, entry.sizeCompressed, buffer, progress);
    }
    progress.finishItem();
}
