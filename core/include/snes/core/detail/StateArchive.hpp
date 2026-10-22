#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace snes::core::detail {

// Encodes fields without native padding or pointers. Reads stage all changes;
// Commit requires a complete, validated input before assigning any live state.
class StateArchive {
public:
  static constexpr size_t MaximumBytes = 128 * 1024 * 1024;
  StateArchive();
  explicit StateArchive(std::span<const uint8_t> bytes);
  StateArchive(const StateArchive &) = delete;
  StateArchive &operator=(const StateArchive &) = delete;

  bool Reading() const noexcept {
    return mode_ != Mode::Write;
  }
  const std::vector<uint8_t> &Bytes() const noexcept {
    return output_;
  }
  size_t Remaining() const noexcept {
    return input_.size() - position_;
  }
  void Commit();
  // Release suspended host execution only after the entire input validates.
  // These callbacks run before any staged field is assigned.
  template <class Prepare> void BeforeRestore(Prepare prepare) {
    Operation operation(*this);
    static_assert(std::is_nothrow_invocable_v<Prepare>);
    if (!Reading())
      return;
    Require(mode_ == Mode::Stage, "Restore preparation requires a live hardware object");
    plan_->preparations.emplace_back(std::move(prepare));
  }
  void Validate(bool condition, const char *message) {
    Require(condition, message);
  }

  template <class T>
    requires(std::is_arithmetic_v<T>)
  T Number(T &value) {
    Operation operation(*this);
    if (!Reading()) {
      WriteNumber(value);
      return value;
    }
    const auto next = ReadNumber<T>();
    Assign(value, next);
    return next;
  }

  template <class T>
    requires(std::is_arithmetic_v<T>)
  T Bounded(T &value, T minimum, T maximum) {
    Operation operation(*this);
    const auto next = Number(value);
    Require(next >= minimum && next <= maximum, "Machine state value is out of range");
    return next;
  }

  template <class T>
    requires(std::is_enum_v<T>)
  T Enumeration(T &value, T minimum, T maximum) {
    Operation operation(*this);
    using U = std::underlying_type_t<T>;
    U next = static_cast<U>(value);
    if (Reading())
      next = ReadNumber<U>();
    Require(next >= static_cast<U>(minimum) && next <= static_cast<U>(maximum),
            "Machine state enumeration is out of range");
    if (!Reading())
      WriteNumber(next);
    else
      Assign(value, static_cast<T>(next));
    return static_cast<T>(next);
  }

  template <class T>
    requires(std::is_arithmetic_v<T>)
  void Expect(T expected) {
    Operation operation(*this);
    if (!Reading())
      WriteNumber(expected);
    else
      Require(ReadNumber<T>() == expected, "Machine state does not match the loaded hardware");
  }

  // Keep the allocation and addresses of mapped memory intact during restore.
  template <class T>
    requires(std::is_arithmetic_v<T>)
  void Fixed(std::span<T> values) {
    Operation operation(*this);
    if (!Reading()) {
      for (const auto value : values)
        WriteNumber(value);
      return;
    }
    Require(values.size() <= Remaining() / WireSize<T>(), "Truncated machine state array");
    if (mode_ == Mode::Scratch) {
      for (auto &value : values)
        value = ReadNumber<T>();
      return;
    }
    ReserveStaging(values.size(), sizeof(T));
    auto next = std::make_shared<std::vector<T>>(values.size());
    for (size_t index = 0; index < values.size(); ++index)
      (*next)[index] = ReadNumber<T>();
    plan_->changes.emplace_back(
        [values, next]() noexcept { std::copy(next->begin(), next->end(), values.begin()); });
  }

  template <class T, size_t N>
    requires(std::is_arithmetic_v<T>)
  void Array(std::array<T, N> &values) {
    Fixed(std::span<T>(values));
  }

  template <class T>
    requires(std::is_arithmetic_v<T>)
  void Vector(std::vector<T> &values, size_t maximumElements) {
    Operation operation(*this);
    if (!Reading()) {
      WriteCount(values.size(), maximumElements);
      for (const T value : values)
        WriteNumber(value);
      return;
    }
    const auto count = ReadCount(maximumElements, WireSize<T>());
    ReserveStaging(count, sizeof(T));
    auto next = std::make_shared<std::vector<T>>(count);
    for (size_t index = 0; index < count; ++index)
      (*next)[index] = ReadNumber<T>();
    Replace(values, std::move(next));
  }

  // The record must own the fields visited here. Borrowed spans and pointers
  // require Fixed on the live object so a scratch copy cannot mutate its owner.
  template <class T, class Visit> void Object(T &value, Visit visit) {
    Operation operation(*this);
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_move_assignable_v<T>);
    if (mode_ != Mode::Stage) {
      VisitObject(visit, value);
      return;
    }
    ReserveStaging(1, sizeof(T));
    auto next = std::make_shared<T>(value);
    StateArchive scratch(Mode::Scratch, input_.subspan(position_), plan_);
    scratch.VisitObject(visit, *next);
    position_ += scratch.position_;
    Replace(value, std::move(next));
  }

  template <class T, size_t N, class Visit> void Array(std::array<T, N> &values, Visit visit) {
    Object(values, [&](auto &archive, auto &items) {
      for (auto &item : items)
        visit(archive, item);
    });
  }

  template <class T, class Visit>
  void Vector(std::vector<T> &values, size_t maximumElements, Visit visit) {
    Operation operation(*this);
    static_assert(std::is_trivially_copyable_v<T>);
    if (!Reading()) {
      WriteCount(values.size(), maximumElements);
      for (auto &value : values)
        VisitObject(visit, value);
      return;
    }
    const auto count = ReadCount(maximumElements, 1);
    ReserveStaging(count, sizeof(T));
    auto next = std::make_shared<std::vector<T>>(count);
    StateArchive scratch(Mode::Scratch, input_.subspan(position_), plan_);
    for (auto &item : *next)
      scratch.VisitObject(visit, item);
    position_ += scratch.position_;
    Replace(values, std::move(next));
  }

  template <class Visit> void Section(uint32_t tag, Visit visit) {
    Operation operation(*this);
    Expect(tag);
    if (!Reading()) {
      const auto lengthAt = output_.size();
      WriteUnsigned(0, 4);
      const auto start = output_.size();
      VisitSection(visit);
      PatchLength(lengthAt, output_.size() - start);
    } else {
      const auto length = ReadUnsigned(4);
      Require(length <= Remaining(), "Truncated machine state section");
      StateArchive section(mode_, input_.subspan(position_, size_t(length)), plan_);
      section.VisitSection(visit);
      Require(section.Remaining() == 0, "Unexpected data in machine state section");
      position_ += size_t(length);
    }
  }

private:
  struct Operation {
    StateArchive &archive;
    int exceptions = std::uncaught_exceptions();
    explicit Operation(StateArchive &owner) : archive(owner) {}
    ~Operation() {
      if (std::uncaught_exceptions() > exceptions)
        archive.Invalidate();
    }
  };
  enum class Mode { Write, Stage, Scratch };
  struct RestorePlan {
    std::vector<std::function<void()>> preparations;
    std::vector<std::function<void()>> changes;
    size_t bytes = 0;
    bool committed = false;
    bool failed = false;
  };
  StateArchive(Mode mode, std::span<const uint8_t> bytes, std::shared_ptr<RestorePlan> plan);
  void Require(bool condition, const char *message);
  void Invalidate() noexcept;
  void WriteUnsigned(uint64_t value, size_t bytes);
  uint64_t ReadUnsigned(size_t bytes);
  void PatchLength(size_t position, size_t length);
  void WriteCount(size_t count, size_t maximumElements);
  size_t ReadCount(size_t maximumElements, size_t minimumBytes);
  void ReserveStaging(size_t count, size_t bytes);
  template <class Visit, class T> void VisitObject(Visit &visit, T &value) {
    try {
      visit(*this, value);
    } catch (...) {
      Invalidate();
      throw;
    }
  }
  template <class Visit> void VisitSection(Visit &visit) {
    try {
      visit(*this);
    } catch (...) {
      Invalidate();
      throw;
    }
  }

  template <class T> static constexpr size_t WireSize() {
    return std::is_same_v<T, bool> ? 1 : sizeof(T);
  }
  template <class T> void WriteNumber(T value) {
    if constexpr (std::is_same_v<T, bool>)
      WriteUnsigned(value ? 1 : 0, 1);
    else if constexpr (std::is_floating_point_v<T>) {
      static_assert(std::numeric_limits<T>::is_iec559 && (sizeof(T) == 4 || sizeof(T) == 8));
      Require(std::isfinite(value), "Machine state contains a non-finite value");
      if constexpr (sizeof(T) == 4)
        WriteUnsigned(std::bit_cast<uint32_t>(value), 4);
      else
        WriteUnsigned(std::bit_cast<uint64_t>(value), 8);
    } else {
      static_assert(sizeof(T) <= 8);
      using U = std::make_unsigned_t<T>;
      WriteUnsigned(std::bit_cast<U>(value), sizeof(T));
    }
  }
  template <class T> T ReadNumber() {
    if constexpr (std::is_same_v<T, bool>) {
      const auto value = ReadUnsigned(1);
      Require(value <= 1, "Invalid boolean in machine state");
      return value != 0;
    } else if constexpr (std::is_floating_point_v<T>) {
      static_assert(std::numeric_limits<T>::is_iec559 && (sizeof(T) == 4 || sizeof(T) == 8));
      T value;
      if constexpr (sizeof(T) == 4)
        value = std::bit_cast<T>(uint32_t(ReadUnsigned(4)));
      else
        value = std::bit_cast<T>(ReadUnsigned(8));
      Require(std::isfinite(value), "Machine state contains a non-finite value");
      return value;
    } else {
      static_assert(sizeof(T) <= 8);
      using U = std::make_unsigned_t<T>;
      return std::bit_cast<T>(U(ReadUnsigned(sizeof(T))));
    }
  }
  template <class T> void Assign(T &value, T next) {
    if (mode_ == Mode::Scratch)
      value = next;
    else
      plan_->changes.emplace_back([&value, next]() noexcept { value = next; });
  }
  template <class T> void Replace(T &value, std::shared_ptr<T> next) {
    static_assert(std::is_nothrow_move_assignable_v<T>);
    if (mode_ == Mode::Scratch)
      value = std::move(*next);
    else
      plan_->changes.emplace_back([&value, next]() noexcept { value = std::move(*next); });
  }

  Mode mode_ = Mode::Write;
  bool root_ = true;
  std::vector<uint8_t> output_;
  std::span<const uint8_t> input_;
  size_t position_ = 0;
  std::shared_ptr<RestorePlan> plan_;
};
} // namespace snes::core::detail
