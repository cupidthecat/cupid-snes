#pragma once

#ifndef _WIN32
#include <locale.h>
#include <stdexcept>
#endif

namespace snes::frontend::detail {
// Keep the decoder's filename conversions independent of the process locale.
class ArchiveLocale {
  public:
#ifndef _WIN32
    ArchiveLocale() {
        locale_ = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        if (!locale_)
            locale_ = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
        if (!locale_)
            throw std::runtime_error("Cannot initialize UTF-8 archive filenames");
        previous_ = uselocale(locale_);
        if (!previous_) {
            freelocale(locale_);
            throw std::runtime_error("Cannot select UTF-8 archive filenames");
        }
    }

    ~ArchiveLocale() {
        uselocale(previous_);
        freelocale(locale_);
    }
#else
    ArchiveLocale() = default;
#endif
    ArchiveLocale(const ArchiveLocale&) = delete;
    ArchiveLocale& operator=(const ArchiveLocale&) = delete;

  private:
#ifndef _WIN32
    locale_t locale_ = nullptr, previous_ = nullptr;
#endif
};
} // namespace snes::frontend::detail
