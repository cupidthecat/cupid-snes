#include "GameAssets.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "storage/AtomicFile.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace snes::frontend {
namespace {
std::filesystem::path MemberPath(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return core::FilePath(name);
}

bool Matches(const LaunchRequest& request, unsigned asset, const std::string& name) {
    auto extension = core::PathText(MemberPath(name).extension());
    for (auto& c : extension)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    if (request.kind == LaunchRequest::Kind::Handheld && asset == 1)
        return extension == ".gb" || extension == ".gbc" || extension == ".sgb";
    if (request.kind == LaunchRequest::Kind::Handheld && asset == 2)
        return extension == ".bin" || extension == ".rom";
    if (asset && (request.kind == LaunchRequest::Kind::SufamiTurbo ||
                  request.kind == LaunchRequest::Kind::Broadcast))
        if (extension == ".st" || extension == ".bs")
            return true;
    return extension == ".sfc" || extension == ".smc" || extension == ".bin" ||
           extension == ".rom" || extension == ".fig" || extension == ".swc";
}
} // namespace

ArchivePlan GameAssets::Resolve(LaunchRequest request, const core::StateOperation& operation) {
    for (unsigned index = 0; index < request.paths.size(); ++index) {
        const auto& path = request.paths[index];
        auto& selected = request.archiveMembers[index];
        if (path.empty() || path == "-" || !RomArchive::Recognizes(core::FilePath(path))) {
            if (!selected.empty())
                throw std::runtime_error("An archive member requires an archive source");
            continue;
        }
        const auto entries = RomArchive::List(core::FilePath(path), operation);
        std::vector<RomArchive::Entry> choices;
        std::set<std::string> names;
        bool found = false;
        for (const auto& entry : entries) {
            if (!Matches(request, index, entry.name))
                continue;
            if (!names.insert(entry.name).second)
                throw std::runtime_error("Archive contains duplicate ROM names: " + entry.name);
            if (entry.name == selected)
                found = true;
            choices.push_back(entry);
        }
        if (!selected.empty()) {
            if (!found)
                throw std::runtime_error("Selected ROM is no longer in the archive: " + selected);
            continue;
        }
        if (choices.empty())
            throw std::runtime_error("Archive contains no compatible ROM: " + path);
        if (choices.size() > 1)
            return {std::move(request), index, std::move(choices)};
        selected = choices.front().name;
    }
    return {std::move(request), 0, {}};
}

bool GameAssets::Archived(const LaunchRequest& request) noexcept {
    return std::any_of(request.archiveMembers.begin(), request.archiveMembers.end(),
                       [](const auto& member) { return !member.empty(); });
}

std::vector<uint8_t> GameAssets::Read(const LaunchRequest& request, unsigned asset,
                                      const core::StateOperation& operation) {
    const auto& path = request.paths.at(asset);
    if (path.empty() || path == "-")
        return {};
    const auto& member = request.archiveMembers[asset];
    if (!std::filesystem::is_regular_file(core::FilePath(path)))
        throw std::runtime_error("Cannot read game asset: " + path);
    return member.empty()
               ? ReadFile(core::FilePath(path), RomArchive::maximumMemberBytes, operation)
               : RomArchive::Read(core::FilePath(path), member, operation);
}

std::filesystem::path GameAssets::Origin(const LaunchRequest& request, unsigned asset) {
    auto path = core::FilePath(request.paths.at(asset));
    if (!request.archiveMembers[asset].empty())
        path = path.parent_path() / MemberPath(request.archiveMembers[asset]).filename();
    return path;
}

std::filesystem::path GameAssets::SaveBase(const LaunchRequest& request, unsigned asset) {
    const auto path = core::FilePath(request.paths.at(asset));
    const auto& member = request.archiveMembers[asset];
    if (member.empty())
        return path;
    std::string label;
    for (unsigned char c : core::PathText(MemberPath(member).stem())) {
        if (label.size() == 32)
            break;
        label +=
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'
                ? char(c)
                : '_';
    }
    const auto canonical =
        core::PathText(std::filesystem::weakly_canonical(std::filesystem::absolute(path)));
    std::vector<uint8_t> identity(canonical.begin(), canonical.end());
    identity.push_back(0);
    identity.insert(identity.end(), member.begin(), member.end());
    const auto digest = core::detail::Sha256(identity);
    constexpr char hex[] = "0123456789abcdef";
    label += '-';
    for (unsigned index = 0; index < 16; ++index) {
        label += hex[digest[index] >> 4];
        label += hex[digest[index] & 15];
    }
    return path.parent_path() / "archive-saves" / label;
}

std::string GameAssets::Label(const LaunchRequest& request, unsigned asset) {
    auto name = core::PathText(core::FilePath(request.paths.at(asset)).filename());
    if (!request.archiveMembers[asset].empty())
        name += " > " + request.archiveMembers[asset];
    return name;
}
} // namespace snes::frontend
