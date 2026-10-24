#include "RomArchive.hpp"
#include "ArchiveLocale.hpp"
#include "snes/core/FilePath.hpp"
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <memory>
#include <stdexcept>

namespace snes::frontend {
namespace {
constexpr uint64_t maximumArchiveBytes = uint64_t(8) * 1024 * 1024 * 1024;
constexpr uint64_t maximumExpandedBytes = uint64_t(4) * 1024 * 1024 * 1024;
constexpr size_t maximumEntries = 16384;

std::string Extension(const std::filesystem::path& path) {
    auto text = core::PathText(path.extension());
    for (auto& byte : text)
        if (byte >= 'A' && byte <= 'Z')
            byte += 'a' - 'A';
    return text;
}

bool RawStream(const std::filesystem::path& path) {
    const auto extension = Extension(path);
    return extension == ".gz" || extension == ".bz2" || extension == ".xz" ||
           extension == ".lzma" || extension == ".zst" || extension == ".lz4";
}

class Reader {
  public:
    Reader(const std::filesystem::path& path, const core::StateOperation& operation)
        : path_(path), operation_(operation), reader_(archive_read_new(), archive_read_free) {
        if (!reader_)
            throw std::runtime_error("Cannot allocate an archive reader");
        const auto bytes = std::filesystem::file_size(path);
        if (bytes > maximumArchiveBytes)
            throw std::runtime_error("Archive exceeds the 8 GiB input limit");
        operation_.Report(core::StatePhase::ReadFile, 0, bytes, core::PathText(path.filename()));
        archive_read_support_format_all(reader_.get());
        if (RawStream(path))
            archive_read_support_format_raw(reader_.get());
        archive_read_support_filter_none(reader_.get());
        archive_read_support_filter_gzip(reader_.get());
        archive_read_support_filter_bzip2(reader_.get());
        archive_read_support_filter_xz(reader_.get());
        archive_read_support_filter_lzma(reader_.get());
        archive_read_support_filter_zstd(reader_.get());
        archive_read_support_filter_lz4(reader_.get());
#ifdef _WIN32
        Check(archive_read_open_filename_w(reader_.get(), path.c_str(), 65536));
#else
        Check(archive_read_open_filename(reader_.get(), path.c_str(), 65536));
#endif
    }

    archive* Get() const noexcept {
        return reader_.get();
    }

    void Check(int result) const {
        if (result != ARCHIVE_OK) {
            const char* error = archive_error_string(Get());
            throw std::runtime_error("Cannot read archive " + core::PathText(path_) + ": " +
                                     (error ? error : "invalid archive data"));
        }
    }

    bool Next(archive_entry*& entry) {
        operation_.Report(core::StatePhase::ReadFile, count_, maximumEntries, "Archive entries");
        const int result = archive_read_next_header(Get(), &entry);
        if (result == ARCHIVE_EOF)
            return false;
        Check(result);
        if (++count_ > maximumEntries)
            throw std::runtime_error("Archive exceeds the 16384-entry limit");
        if (archive_entry_size_is_set(entry)) {
            const auto size = archive_entry_size(entry);
            if (size < 0 || uint64_t(size) > maximumExpandedBytes - expanded_)
                throw std::runtime_error("Archive exceeds the 4 GiB expanded-size limit");
            expanded_ += uint64_t(size);
        }
        return true;
    }

    std::string Name(archive_entry* entry) const {
        if (archive_format(Get()) == ARCHIVE_FORMAT_RAW)
            return core::PathText(path_.filename().stem());
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name || !*name)
            throw std::runtime_error("Archive entry has no readable UTF-8 filename");
        const std::string text(name);
        if (text.size() > 4096 || std::any_of(text.begin(), text.end(),
                                              [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::runtime_error("Archive entry contains an unsupported filename");
        return text;
    }

    void Finish() {
        Check(archive_read_close(Get()));
    }

  private:
    std::filesystem::path path_;
    const core::StateOperation& operation_;
    detail::ArchiveLocale locale_;
    std::unique_ptr<archive, decltype(&archive_read_free)> reader_;
    size_t count_ = 0;
    uint64_t expanded_ = 0;
};

bool Regular(archive_entry* entry) {
    return archive_entry_filetype(entry) == AE_IFREG && !archive_entry_symlink(entry) &&
           !archive_entry_hardlink(entry);
}
} // namespace

bool RomArchive::Recognizes(const std::filesystem::path& path) {
    static constexpr std::array extensions{".7z",   ".zip", ".tar",  ".rar", ".tgz",  ".tbz",
                                           ".tbz2", ".txz", ".tzst", ".gz",  ".bz2",  ".xz",
                                           ".lzma", ".zst", ".lz4",  ".cab", ".cpio", ".pax",
                                           ".iso",  ".xar", ".lha",  ".lzh", ".ar"};
    const auto extension = Extension(path);
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

std::vector<RomArchive::Entry> RomArchive::List(const std::filesystem::path& path,
                                                const core::StateOperation& operation) {
    Reader reader(path, operation);
    std::vector<Entry> entries;
    archive_entry* entry;
    while (reader.Next(entry)) {
        if (Regular(entry))
            entries.push_back({reader.Name(entry), archive_entry_size_is_set(entry)
                                                       ? uint64_t(archive_entry_size(entry))
                                                       : 0});
        reader.Check(archive_read_data_skip(reader.Get()));
    }
    reader.Finish();
    return entries;
}

std::vector<uint8_t> RomArchive::Read(const std::filesystem::path& path, const std::string& member,
                                      const core::StateOperation& operation) {
    Reader reader(path, operation);
    archive_entry* entry;
    while (reader.Next(entry)) {
        if (!Regular(entry) || reader.Name(entry) != member) {
            reader.Check(archive_read_data_skip(reader.Get()));
            continue;
        }
        if (archive_entry_is_encrypted(entry) == 1)
            throw std::runtime_error("Encrypted ROM entries are not supported");
        const auto size =
            archive_entry_size_is_set(entry) ? uint64_t(archive_entry_size(entry)) : 0;
        if (size > maximumMemberBytes)
            throw std::runtime_error("Archived ROM exceeds the 128 MiB limit");
        std::vector<uint8_t> bytes;
        bytes.reserve(size_t(size));
        std::array<uint8_t, 65536> chunk;
        while (true) {
            operation.Report(core::StatePhase::ReadFile, bytes.size(), size, member);
            const auto count = archive_read_data(reader.Get(), chunk.data(), chunk.size());
            if (count < 0)
                reader.Check(int(count));
            if (!count)
                break;
            if (uint64_t(count) > maximumMemberBytes - bytes.size())
                throw std::runtime_error("Archived ROM exceeds the 128 MiB limit");
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        }
        if (size && bytes.size() != size)
            throw std::runtime_error("Archived ROM has an incomplete payload");
        reader.Finish();
        operation.Report(core::StatePhase::ReadFile, bytes.size(), bytes.size(), member);
        return bytes;
    }
    throw std::runtime_error("Selected ROM is no longer in the archive: " + member);
}
} // namespace snes::frontend
