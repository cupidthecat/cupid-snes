#pragma once

#include "snes/core/FilePath.hpp"
#include "storage/ArchiveLocale.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace snes::tests::archives {
struct File {
    std::string name;
    std::vector<uint8_t> bytes;
    bool symlink = false;
};

inline void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file)
        throw std::runtime_error("Write archive fixture");
}

inline void Archive(const std::filesystem::path& path, std::string_view format,
                    const std::vector<File>& files) {
    snes::frontend::detail::ArchiveLocale locale;
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(),
                                                                   archive_write_free);
    const auto check = [&](int result) {
        if (result != ARCHIVE_OK)
            throw std::runtime_error(archive_error_string(writer.get()));
    };
    if (format == "zip") {
        check(archive_write_set_format_zip(writer.get()));
        check(archive_write_set_format_option(writer.get(), "zip", "hdrcharset", "UTF-8"));
        check(archive_write_set_format_option(writer.get(), "zip", "compression", "store"));
    } else if (format == "7z")
        check(archive_write_set_format_7zip(writer.get()));
    else if (format.starts_with("raw"))
        check(archive_write_set_format_raw(writer.get()));
    else
        check(archive_write_set_format_pax_restricted(writer.get()));
    if (format.ends_with(".gz"))
        check(archive_write_add_filter_gzip(writer.get()));
    if (format.ends_with(".bz2"))
        check(archive_write_add_filter_bzip2(writer.get()));
    if (format.ends_with(".xz"))
        check(archive_write_add_filter_xz(writer.get()));
    if (format.ends_with(".zst"))
        check(archive_write_add_filter_zstd(writer.get()));
    if (format.ends_with(".lz4"))
        check(archive_write_add_filter_lz4(writer.get()));
#ifdef _WIN32
    check(archive_write_open_filename_w(writer.get(), path.c_str()));
#else
    check(archive_write_open_filename(writer.get(), path.c_str()));
#endif
    for (const auto& file : files) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(),
                                                                            archive_entry_free);
        archive_entry_set_pathname_utf8(entry.get(), file.name.c_str());
        archive_entry_set_filetype(entry.get(), file.symlink ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), file.symlink ? 0 : la_int64_t(file.bytes.size()));
        if (file.symlink)
            archive_entry_set_symlink(entry.get(), "../outside.sfc");
        check(archive_write_header(writer.get(), entry.get()));
        if (!file.symlink && !file.bytes.empty()) {
            const auto count =
                archive_write_data(writer.get(), file.bytes.data(), file.bytes.size());
            if (count != la_ssize_t(file.bytes.size()))
                throw std::runtime_error("Write complete archive fixture payload");
        }
    }
    check(archive_write_close(writer.get()));
}

// A stored RAR 4 fixture exercises a ROM payload without requiring an archiver executable.
inline void Rar(const std::filesystem::path& path, const File& file) {
    std::vector<uint8_t> bytes{'R', 'a', 'r', '!', 0x1a, 7, 0};
    const auto put = [](std::vector<uint8_t>& out, uint64_t value, unsigned count) {
        for (unsigned index = 0; index < count; ++index)
            out.push_back(uint8_t(value >> (index * 8)));
    };
    const auto header = [&](uint8_t type, uint16_t flags, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> block{type};
        put(block, flags, 2);
        put(block, body.size() + 7, 2);
        block.insert(block.end(), body.begin(), body.end());
        put(bytes, crc32(0, block.data(), uInt(block.size())), 2);
        bytes.insert(bytes.end(), block.begin(), block.end());
    };
    header(0x73, 0, std::vector<uint8_t>(6));
    std::vector<uint8_t> body;
    put(body, file.bytes.size(), 4);
    put(body, file.bytes.size(), 4);
    body.push_back(3);
    put(body, crc32(0, file.bytes.data(), uInt(file.bytes.size())), 4);
    put(body, 0, 4);
    body.push_back(20);
    body.push_back(0x30);
    put(body, file.name.size(), 2);
    put(body, 0100644, 4);
    body.insert(body.end(), file.name.begin(), file.name.end());
    header(0x74, 0x8000, body);
    bytes.insert(bytes.end(), file.bytes.begin(), file.bytes.end());
    header(0x7b, 0, {});
    Write(path, bytes);
}
} // namespace snes::tests::archives
