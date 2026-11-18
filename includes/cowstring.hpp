#pragma once
#ifndef COPY_ON_WRITE_STRING_H
#define COPY_ON_WRITE_STRING_H

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <type_traits>
#include <meta>
#include <climits>
#include <limits>
#include <utility>
#include <iterator>
#include <string>
#include <string_view>
#include <span>
#include <inplace_vector>
#include <memory>
#include <atomic>
#include <bit>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <ranges>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace cow {

namespace details {

inline constexpr std::meta::info valid_char_types[] = {
    ^^char, ^^wchar_t, ^^char8_t, ^^char16_t, ^^char32_t
};

template<typename CharT>
inline constexpr bool is_valid_char_type =
    std::ranges::find(valid_char_types, ^^CharT) != std::ranges::end(valid_char_types);

inline constexpr std::size_t max_char_alignment = alignment_of(
    std::ranges::max(valid_char_types, {}, &std::meta::alignment_of));

template<typename SizeT, typename CharT>
inline constexpr SizeT cow_max_size = std::numeric_limits<SizeT>::max() / (sizeof(CharT) * 2);

template<typename SizeT>
struct alignas(max_char_alignment) ctrl_blk {
    const SizeT capacity;
    std::atomic_size_t ref_count;

    template<typename CharT>
    CharT* data() noexcept { return reinterpret_cast<CharT*>(this + 1); }
};

template<typename Alloc,
         typename Traits = std::allocator_traits<Alloc>,
         typename SizeT = Traits::size_type>
ctrl_blk<SizeT>* allocate(const Alloc& source, std::type_identity_t<SizeT> capacity) {
    using size_type = SizeT;
    using header = ctrl_blk<size_type>;
    using allocator = Traits::template rebind_alloc<header>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    static constexpr std::size_t header_size = sizeof(header);
    static constexpr std::size_t value_size = sizeof(value_type);
    static_assert(std::is_same_v<size_type, typename traits::size_type>);
    static_assert(header_size >= value_size && alignof(header) >= alignof(value_type));
    allocator alloc(source);
    if (capacity > cow_max_size<size_type, value_type>) {
        throw std::length_error("basic_cow_string: capacity too large");
    }
    const std::size_t total_size = (value_size * capacity + header_size - 1) / header_size + 1;
    const std::size_t actual_capacity = (total_size - 1) * header_size / value_size;
    header* block = traits::allocate(alloc, total_size);
    traits::construct(alloc, block, actual_capacity, 1);
    return block;
}

template<typename Alloc,
         typename Traits = std::allocator_traits<Alloc>,
         typename SizeT = Traits::size_type>
void deallocate(const Alloc& source, std::type_identity_t<ctrl_blk<SizeT>>* block) noexcept {
    using header = ctrl_blk<SizeT>;
    using allocator = Traits::template rebind_alloc<header>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    static constexpr std::size_t header_size = sizeof(header);
    static constexpr std::size_t value_size = sizeof(value_type);
    allocator alloc(source);
    const std::size_t total_size = (value_size * block->capacity + header_size - 1) / header_size + 1;
    traits::destroy(alloc, block);
    traits::deallocate(alloc, block, total_size);
}

template<typename CharT, typename SizeT>
struct dyn_string {
    const CharT* data = nullptr;
    SizeT size = 0;
    std::uintptr_t control : (sizeof(SizeT) * CHAR_BIT - 2) = 0;
    mutable std::uintptr_t shared : 1 = 0;
    std::uintptr_t allocated : 1 = 1;
};

template<typename CharT, typename SizeT>
struct sta_string {
    const CharT* data = nullptr;
    SizeT size = 0;
    SizeT _ : (sizeof(SizeT) * CHAR_BIT - 2) = 0;
    SizeT shared : 1 = 1;
    SizeT allocated : 1 = 0;
    static constexpr std::size_t capacity = 0;
};

template<typename CharT, std::size_t Size>
struct sso_string {
    CharT data[Size] = {};
    unsigned char _[sizeof(CharT) - 1] = {};
    unsigned char size : (CHAR_BIT - 2) = 0;
    unsigned char shared : 1 = 0;
    unsigned char allocated : 1 = 0;
    static constexpr std::size_t capacity = Size;
};

template<typename CharT, std::size_t Size>
    requires (sizeof(CharT) == 1)
struct sso_string<CharT, Size> {
    CharT data[Size] = {};
    unsigned char size : (CHAR_BIT - 2) = 0;
    unsigned char shared : 1 = 0;
    unsigned char allocated : 1 = 0;
    static constexpr std::size_t capacity = Size;
};

struct status {
    bool shared, allocated;
    friend constexpr bool operator==(const status&, const status&) = default;
};

// sso buffer
inline constexpr status small   = { .shared = false, .allocated = false };
// bind to static string (literal or via std::define_static_string)
inline constexpr status literal = { .shared = true, .allocated = false };
// allocated but no one copy yet
inline constexpr status owned   = { .shared = false, .allocated = true };
// allocated and may shared with others (maybe they have already released it after copy)
inline constexpr status shared  = { .shared = true, .allocated = true };

template<typename CharT, typename SizeT>
union storage {
    using dyn_type = dyn_string<CharT, SizeT>;
    using sta_type = sta_string<CharT, SizeT>;
    static constexpr std::size_t bufsz = (sizeof(dyn_type) - 1) / sizeof(CharT) > 2
        ? (sizeof(dyn_type) - 1) / sizeof(CharT) : 2;
    using sso_type = sso_string<CharT, bufsz>;

    dyn_type dyn;
    sta_type sta;
    sso_type sso;

    static_assert(sizeof(dyn) == sizeof(sta) && sizeof(dyn) == sizeof(sso));
    static_assert(sizeof(SizeT) >= sizeof(void*));

    constexpr details::status status() const noexcept {
        if consteval {
            if (std::is_within_lifetime(&sso)) {
                return small;
            } else if (std::is_within_lifetime(&sta)) {
                return literal;
            } else {
                std::unreachable();
            }
        } else {
            const auto probe = std::bit_cast<sso_type>(*this);
            return { probe.shared, probe.allocated };
        }
    }

    template<typename OP>
    constexpr auto visit(OP op) const noexcept {
        const auto s = status();
        if (s == small) {
            return op(sso);
        } else if (s == literal) {
            return op(sta);
        } else if !consteval {
            return op(dyn);
        }
        std::unreachable();
    }

    constexpr const CharT* data() const noexcept {
        return visit([](const auto& s) -> const CharT* { return s.data; });
    }

    constexpr SizeT size() const noexcept {
        return visit([](const auto& s) -> SizeT { return s.size; });
    }

    constexpr std::basic_string_view<CharT> view() const noexcept {
        return visit([](const auto& s) -> std::basic_string_view<CharT> {
            return { s.data, s.size };
        });
    }

    ctrl_blk<SizeT>* control() const noexcept
        pre (status().allocated)
    {
        return reinterpret_cast<ctrl_blk<SizeT>*>(dyn.control << 2);
    }

    std::span<CharT> allocation() const noexcept
        pre (status().allocated)
    {
        auto* block = control();
        return { block->template data<CharT>(), block->capacity };
    }

    std::atomic_size_t& ref_count() const noexcept
        pre (status().allocated)
    {
        return control()->ref_count;
    }

    bool self_alias(const void* ptr) const noexcept {
        static constexpr std::less_equal<void> le = {};
        const auto s = status();
        if (s == literal) {
            return false;
        }
        if (s == small) {
            return le(sso.data, ptr) && le(ptr, sso.data + sso.size);
        }
        auto alloc = allocation();
        return le(alloc.data(), ptr) && le(ptr, alloc.data() + alloc.size());
    }

    // There is no writable string during compile time
    CharT* data() noexcept
        pre (!status().shared)
    {
        const auto s = status();
        if (s == small) {
            return sso.data;
        } else {
            return const_cast<CharT*>(dyn.data);
        }
    }

    constexpr SizeT capacity() const noexcept {
        const auto s = status();
        if (s == small) {
            return sso.capacity;
        } else if (s == literal) {
            return sta.capacity;
        } else if !consteval {
            // Capacity of dynamic string is size plus space
            // after current tail, which means for substring
            // scenario prefix won't count. They can still
            // be reused when actual recap happens.
            auto alloc = allocation();
            return static_cast<SizeT>((alloc.data() + alloc.size()) - dyn.data);
        }
        std::unreachable();
    }

    constexpr SizeT real_capacity() const noexcept {
        const auto s = status();
        if (s == small) {
            return sso.capacity;
        } else if (s == literal) {
            return sta.capacity;
        } else if !consteval {
            return static_cast<SizeT>(allocation().size());
        }
        std::unreachable();
    }
};

template<typename Iter, typename CharT>
concept string_like_iter = std::contiguous_iterator<Iter>
    && std::convertible_to<typename std::iterator_traits<Iter>::pointer, const CharT*>;

template<typename Iter, typename CharT>
consteval bool can_direct_replace() noexcept {
    if constexpr (std::contiguous_iterator<Iter>) {
        using pointer = std::iterator_traits<Iter>::pointer;
        using pointee = std::remove_cv_t<std::remove_pointer_t<pointer>>;
        // Exclude magic types that can alias with anything.
        return !std::is_class_v<pointee> && !std::is_same_v<pointee, CharT>
            && !std::is_same_v<pointee, char> && !std::is_same_v<pointee, unsigned char>
            && !std::is_same_v<pointee, std::byte>;
    } else {
        return false;
    }
}

// In std::string, some interfaces take null-terminated string
// as argument. This facility only accepts string literals
// known at compile time for corresponding APIs.
template<typename CharT>
class literal_helper
{
public:
    literal_helper() = delete;
    literal_helper(const literal_helper&) = delete;
    literal_helper& operator=(const literal_helper&) = delete;

    consteval literal_helper(const CharT* s)
        : str(s), length(std::char_traits<CharT>::length(s))
    {
        if (!std::is_string_literal(s)) {
            std::unreachable();
        }
    }

    constexpr const CharT* data() const noexcept { return str; }
    constexpr std::size_t size() const noexcept { return length; }

private:
    const CharT* str;
    std::size_t length;
};

template<typename CharT, bool IsConst>
class string_iterator
{
public:
    using iterator_concept = std::contiguous_iterator_tag;
    using value_type = CharT;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<IsConst, const CharT*, CharT*>;
    using reference = std::conditional_t<IsConst, const CharT&, CharT&>;

    constexpr string_iterator() = default;
    constexpr string_iterator(const string_iterator&) = default;
    constexpr string_iterator& operator=(const string_iterator&) = default;
    explicit constexpr string_iterator(pointer ptr) noexcept : cur(ptr) {}

    constexpr operator string_iterator<CharT, true>() const noexcept requires(!IsConst) {
        return string_iterator<CharT, true>(cur);
    }

    constexpr reference operator*() const noexcept pre (cur != nullptr) { return *cur; }
    constexpr string_iterator& operator++() noexcept pre (cur != nullptr) { ++cur; return *this; }
    constexpr string_iterator operator++(int) noexcept pre (cur != nullptr) { auto tmp = *this; ++cur; return tmp; }
    constexpr string_iterator& operator--() noexcept pre (cur != nullptr) { --cur; return *this; }
    constexpr string_iterator operator--(int) noexcept pre (cur != nullptr) { auto tmp = *this; --cur; return tmp; }
    constexpr string_iterator& operator+=(difference_type n) noexcept pre (cur != nullptr) { cur += n; return *this; }
    constexpr string_iterator& operator-=(difference_type n) noexcept pre (cur != nullptr) { cur -= n; return *this; }
    constexpr string_iterator operator+(difference_type n) const noexcept pre (cur != nullptr) { return auto(*this) += n; }
    // There is a GCC bug makes contracts on hidden friend of template class cause ICE
    friend constexpr string_iterator operator+(difference_type n, string_iterator it) noexcept { return it += n; }
    constexpr string_iterator operator-(difference_type n) const noexcept pre (cur != nullptr) { return auto(*this) -= n; }
    constexpr difference_type operator-(string_iterator other) const noexcept pre (cur != nullptr && other.cur != nullptr) { return cur - other.cur; }
    constexpr reference operator[](difference_type n) const noexcept pre (cur != nullptr) { return *(cur + n); }
    constexpr pointer operator->() const noexcept pre (cur != nullptr) { return cur; }

    friend constexpr bool operator==(string_iterator lhs, string_iterator rhs) = default;
    friend constexpr auto operator<=>(string_iterator lhs, string_iterator rhs) = default;

private:
    pointer cur = nullptr;
};

template<typename S>
concept string_like = requires(const S& s) {
    std::basic_string_view<typename S::value_type>(s);
};

template<string_like S>
using char_type = S::value_type;

inline bool overlap(std::span<const std::byte> x, std::span<const std::byte> y) noexcept {
    const std::byte* const xb = x.data();
    const std::byte* const xe = xb + x.size();
    const std::byte* const yb = y.data();
    const std::byte* const ye = yb + y.size();
    static constexpr std::less<> less{};
    contract_assert(!less(xe, xb) && !less(ye, yb));
    return less(xb, ye) && less(yb, xe);
}

inline bool enclose(std::span<const std::byte> small,
                    std::span<const std::byte> large) noexcept
{
    const std::byte* const sb = small.data();
    const std::byte* const se = sb + small.size();
    const std::byte* const lb = large.data();
    const std::byte* const le = lb + large.size();
    static constexpr std::less<> less{};
    contract_assert(!less(se, sb) && !less(le, lb));
    return !less(sb, lb) && !less(le, se);
}

inline bool prefix_or_suffix(std::span<const std::byte> small,
                             std::span<const std::byte> large) noexcept
{
    const std::byte* const sb = small.data();
    const std::byte* const se = sb + small.size();
    const std::byte* const lb = large.data();
    const std::byte* const le = lb + large.size();
    static constexpr std::less_equal<> less_equal{};
    static constexpr std::equal_to<> equal{};
    return (equal(sb, lb) && less_equal(se, le))
        || (equal(se, le) && less_equal(lb, sb));
}

inline std::span<const std::byte> overlap_region(std::span<const std::byte> x,
                                                 std::span<const std::byte> y) noexcept
{
    contract_assert(details::overlap(x, y));
    const std::byte* const xb = x.data();
    const std::byte* const xe = xb + x.size();
    const std::byte* const yb = y.data();
    const std::byte* const ye = yb + y.size();
    static constexpr std::less<> less{};
    const std::byte* const begin = std::ranges::max(xb, yb, less);
    const std::byte* const end = std::ranges::min(xe, ye, less);
    return { begin, end };
}

template<typename T>
std::span<T> rebase_region(std::span<const std::byte> sub,
                           std::span<const std::byte> base,
                           std::span<T> rebase) noexcept
{
    contract_assert(details::enclose(sub, base));
    contract_assert(base.size() == rebase.size());
    const std::byte* const sub_data = sub.data();
    const std::byte* const base_data = base.data();
    return rebase.subspan(sub_data - base_data, sub.size());
}

inline std::span<const std::byte> complement(std::span<const std::byte> sub,
                                             std::span<const std::byte> base) noexcept
{
    contract_assert(details::prefix_or_suffix(sub, base));
    const std::byte* const sb = sub.data();
    const std::byte* const se = sb + sub.size();
    const std::byte* const bb = base.data();
    const std::byte* const br = bb + base.size();
    if (sb == bb) {
        return { se, br };
    } else if (se == br) {
        return { bb, sb };
    } else {
        std::unreachable();
    }
}

inline void spanmov(std::span<const std::byte> src, std::span<std::byte> dst) noexcept
    pre (dst.size() >= src.size())
{
    std::memmove(dst.data(), src.data(), src.size());
}

inline bool try_inplace_self_replace(std::span<const std::byte> head,
                                     std::span<const std::byte> tail,
                                     std::span<const std::byte> source,
                                     std::span<std::byte> head_dest,
                                     std::span<std::byte> tail_dest,
                                     std::span<std::byte> source_dest) noexcept
{
    // Tail never overlaps with head destination
    // Head may overlap with tail destination, but just copy head first
    const bool hs = details::overlap(head_dest, source);
    const bool sh = details::overlap(source_dest, head);
    const bool ts = details::overlap(tail_dest, source);
    const bool st = details::overlap(source_dest, tail);
    if (!hs && !st) {
        spanmov(head, head_dest);
        spanmov(source, source_dest);
        spanmov(tail, tail_dest);
        return true;
    }
    if (!sh && !st) {
        spanmov(source, source_dest);
        spanmov(head, head_dest);
        spanmov(tail, tail_dest);
        return true;
    }
    if (!ts && !hs) {
        spanmov(head, head_dest);
        spanmov(tail, tail_dest);
        spanmov(source, source_dest);
        return true;
    }
    if (sh && hs && !st) {
        // Check if:
        // 1. The clobbered region of source can be recovered from the moved head, or
        // 2. The clobbered region of head can be recovered from the moved source.
        // If doable, move tail after recovering the clobbered region.
        // Note that if clobbered region can be recovered, it is always head or tail of
        // the total region of source or head.
        std::span<const std::byte> clob_hs = details::overlap_region(head_dest, source);
        if (details::enclose(clob_hs, head)) {
            // Clobbered region of source by copying head
            // can be recovered from the moved head
            std::span<const std::byte> recover =
                details::rebase_region(clob_hs, head, head_dest);
            std::span<const std::byte> unaffected = details::complement(clob_hs, source);
            std::span<std::byte> recover_dest =
                details::rebase_region(clob_hs, source, source_dest);
            std::span<std::byte> unaffected_dest =
                details::rebase_region(unaffected, source, source_dest);
            spanmov(head, head_dest);
            spanmov(unaffected, unaffected_dest);
            spanmov(recover, recover_dest);
            spanmov(tail, tail_dest);
            return true;
        }
        std::span<const std::byte> clob_sh = details::overlap_region(source_dest, head);
        if (details::enclose(clob_sh, source)) {
            // Clobbered region of head by copying source
            // can be recovered from the moved source
            std::span<const std::byte> recover =
                details::rebase_region(clob_sh, source, source_dest);
            std::span<const std::byte> unaffected = details::complement(clob_sh, head);
            std::span<std::byte> recover_dest =
                details::rebase_region(clob_sh, head, head_dest);
            std::span<std::byte> unaffected_dest =
                details::rebase_region(unaffected, head, head_dest);
            spanmov(source, source_dest);
            spanmov(unaffected, unaffected_dest);
            spanmov(recover, recover_dest);
            spanmov(tail, tail_dest);
            return true;
        }
    }
    if (st && ts && !hs) {
        // Similar as previous case, but for source-tail overlap and move head first
        std::span<const std::byte> clob_st = details::overlap_region(source_dest, tail);
        if (details::enclose(clob_st, source)) {
            std::span<const std::byte> recover =
                details::rebase_region(clob_st, source, source_dest);
            std::span<const std::byte> unaffected = details::complement(clob_st, tail);
            std::span<std::byte> recover_dest =
                details::rebase_region(clob_st, tail, tail_dest);
            std::span<std::byte> unaffected_dest =
                details::rebase_region(unaffected, tail, tail_dest);
            spanmov(head, head_dest);
            spanmov(source, source_dest);
            spanmov(unaffected, unaffected_dest);
            spanmov(recover, recover_dest);
            return true;
        }
        std::span<const std::byte> clob_ts = details::overlap_region(tail_dest, source);
        if (details::enclose(clob_ts, tail)) {
            std::span<const std::byte> recover =
                details::rebase_region(clob_ts, tail, tail_dest);
            std::span<const std::byte> unaffected = details::complement(clob_ts, source);
            std::span<std::byte> recover_dest =
                details::rebase_region(clob_ts, source, source_dest);
            std::span<std::byte> unaffected_dest =
                details::rebase_region(unaffected, source, source_dest);
            spanmov(head, head_dest);
            spanmov(tail, tail_dest);
            spanmov(unaffected, unaffected_dest);
            spanmov(recover, recover_dest);
            return true;
        }
    }
    return false;
}

} // namespace cow::details

template<typename CharT, typename Allocator = std::allocator<CharT>>
class basic_cow_string : private Allocator
{
public:
    using value_type = CharT;
    using char_type = CharT;
    using allocator_type = Allocator;
    using allocator_traits_type = std::allocator_traits<allocator_type>;
    using size_type = allocator_traits_type::size_type;
    using difference_type = allocator_traits_type::difference_type;
    using const_reference = const value_type&;
    using reference = value_type&;
    using const_pointer = allocator_traits_type::const_pointer;
    using pointer = allocator_traits_type::pointer;
    using const_iterator = details::string_iterator<value_type, true>;
    using iterator = details::string_iterator<value_type, false>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator = std::reverse_iterator<iterator>;

    using string_view_type = std::basic_string_view<char_type>;
    using string_type = std::basic_string<char_type, std::char_traits<char_type>, allocator_type>;

    static constexpr size_type npos = static_cast<size_type>(-1);

private:
    static_assert(std::is_same_v<value_type, typename allocator_traits_type::value_type>);
    static_assert(details::is_valid_char_type<CharT>);
    static_assert(std::is_pointer_v<pointer> && std::is_pointer_v<const_pointer>
        && std::is_same_v<std::remove_const_t<std::remove_pointer_t<const_pointer>>, char_type>
        && std::is_same_v<std::remove_pointer_t<pointer>, char_type>);
    using storage_type = details::storage<char_type, size_type>;
    static_assert(sizeof(storage_type) == sizeof(storage_type::dyn));
    static_assert(std::contiguous_iterator<iterator>);
    static_assert(std::contiguous_iterator<const_iterator>);
    static constexpr size_type growing_factor = 2;

    // Allocator strategy of cow_string
    // On copy:
    //  If allocator_traits::is_always_equal is true, life is easy;
    //  If not but propagate_on_container_copy_assignment is true, just copy assign
    //  the allocator, then here is this->get_allocator() == other.get_allocator(), and
    //  life is still easy;
    //  If both is false, we have to do deep copy if allocators are not equal, and
    //  the copy ctor has to be marked as throwing anyway.
    // On move:
    //  Things are basically the same as copy.
    // On swap:
    //  Things are basically the same as copy, but deep copy twice.
    static constexpr bool always_equal_alloc = allocator_traits_type::is_always_equal::value;
    static constexpr bool POCCA = allocator_traits_type::propagate_on_container_copy_assignment::value;
    static constexpr bool POCMA = allocator_traits_type::propagate_on_container_move_assignment::value;
    static constexpr bool POCS = allocator_traits_type::propagate_on_container_swap::value;
    static_assert(std::is_nothrow_swappable_v<allocator_type>
        && std::is_nothrow_copy_constructible_v<allocator_type>
        && std::is_nothrow_copy_assignable_v<allocator_type>
        && std::is_nothrow_move_constructible_v<allocator_type>
        && std::is_nothrow_move_assignable_v<allocator_type>);

    constexpr details::status status() const noexcept { return storage.status(); }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    consteval void write_static_string(R&& r)
        pre (!status().allocated)
        post (!status().allocated)
    {
        // Do not touch customized allocator here
        std::basic_string<char_type> test(std::from_range, std::forward<R>(r));
        if (test.size() > storage.sso.capacity) {
            std::start_lifetime(storage.sta);
            storage.sta = { .data = std::define_static_string(test), .size = test.size() };
        } else {
            write_sso(test);
        }
    }

    constexpr bool can_write_sso(size_type size) const noexcept {
        return size <= storage.sso.capacity;
    }

    constexpr char_type* write_sso(string_view_type view) noexcept
        pre (!status().allocated)
        pre (can_write_sso(view.size()))
        post (status() == details::small)
    {
        std::start_lifetime(storage.sso);
        storage.sso = {};
        char_type* const data = storage.sso.data;
        std::ranges::copy_n(view.data(), view.size(), data);
        storage.sso.size = static_cast<unsigned char>(view.size());
        return storage.sso.data;
    }

    // Copy content of view into SSO buffer, view is referencing current object.
    // Then release the old dynamic storage if needed.
    constexpr char_type* write_sso_release(string_view_type view) noexcept
        pre (can_write_sso(view.size()))
        post (status() == details::small)
    {
        // Use a temporary buffer to move the content to SSO storage
        // SSO capacity is small, should be fine
        const std::size_t size = view.size();
        char_type buffer[storage.sso.capacity] = {};
        std::ranges::copy_n(view.data(), size, buffer);
        deallocate_if_needed();
        return write_sso(string_view_type(buffer, size));
    }

    constexpr void assign_static_string(string_view_type view) noexcept
        pre (!status().allocated)
        post (status() == details::literal)
    {
        std::start_lifetime(storage.sta);
        storage.sta = { .data = view.data(), .size = view.size() };
    }

    std::atomic_size_t& ref_count() const noexcept
        pre (status().allocated)
    {
        return storage.ref_count();
    }

    constexpr allocator_type allocator_copy() const noexcept(always_equal_alloc) {
        if constexpr (always_equal_alloc) {
            return allocator_type();
        } else {
            return allocator_traits_type::select_on_container_copy_construction(
                get_allocator()
            );
        }
    }

    constexpr allocator_type allocator_move() && noexcept {
        if constexpr (always_equal_alloc) {
            return allocator_type();
        } else {
            return static_cast<allocator_type&&>(*this);
        }
    }

    constexpr bool can_share_with(const basic_cow_string& other) const noexcept {
        return !other.status().allocated || this->get_allocator() == other.get_allocator();
    }

    constexpr void set_states_on_copy(const basic_cow_string& other,
                                      size_type pos = 0, size_type count = npos)
        pre (can_share_with(other))
    {
        const auto s = other.status();
        const string_view_type subview = other.subview(pos, count);
        if (can_write_sso(subview.size())) {
            write_sso(subview);
        } else if (s.allocated) {
            // Share with source
            other.storage.dyn.shared = 1;
            storage = other.storage;
            storage.dyn.data = subview.data();
            storage.dyn.size = subview.size();
            ref_count().fetch_add(1, std::memory_order_relaxed);
        } else {
            contract_assert(s == details::literal);
            assign_static_string(subview);
        }
    }

    constexpr void set_states_on_move(basic_cow_string&& other,
                                      size_type pos = 0, size_type count = npos)
    {
        const auto s = other.status();
        const string_view_type subview = other.subview(pos, count);
        if (can_write_sso(subview.size())) {
            write_sso(subview);
        } else if (s.allocated) {
            // Steal from source and set it to sso empty string
            storage = other.storage;
            storage.dyn.data = subview.data();
            storage.dyn.size = subview.size();
            std::start_lifetime(other.storage.sso);
            other.storage.sso = {};
        } else {
            contract_assert(s == details::literal);
            assign_static_string(subview);
        }
    }

    constexpr void deallocate_if_needed() noexcept
        post (!status().allocated)
    {
        const auto s = status();
        if (s.allocated) {
            if (!s.shared || ref_count().fetch_sub(1, std::memory_order_acq_rel) == 1) {
                details::deallocate(get_allocator(), storage.control());
            }
            std::start_lifetime(storage.sso);
            storage.sso = {};
        }
    }

    // If capacity < str.size(), exceeded contents are discarded
    char_type* allocate_exclusive_buffer(string_view_type str, std::size_t capacity) {
        const char_type* const str_data = str.data();
        const std::size_t size = std::ranges::min(str.size(), capacity);
        auto* const control = details::allocate(get_allocator(), capacity);
        char_type* const data = control->template data<char_type>();
        std::ranges::copy_n(str_data, size, data);
        deallocate_if_needed();
        std::start_lifetime(storage.dyn);
        storage.dyn = {
            .data = data, .size = size,
            .control = reinterpret_cast<std::uintptr_t>(control) >> 2
        };
        return data;
    }

    char_type* allocate_exclusive_buffer(string_view_type str) {
        return allocate_exclusive_buffer(str, str.size());
    }

    char_type* reserve_or_exclusive_shrink(size_type new_cap)
        post (exclusive())
    {
        // Get new buffer if is shared with other strings or holds literal.
        // In this case, new buffer always have capacity of new_cap regardless of current capacity.
        // Tail contents are discarded if new_cap < size() here. (The 'exclusive shrink' branch)
        if (!exclusive()) {
            if (can_write_sso(new_cap)) {
                return write_sso_release(subview(0, new_cap));
            } else {
                return allocate_exclusive_buffer(view(), new_cap);
            }
        } else {
            const size_type old_real_cap = real_capacity();
            if (new_cap > old_real_cap) {
                // The auto growing branch
                const size_type grow = std::saturating_mul(old_real_cap, growing_factor);
                const size_type ceil_grow = std::ranges::min(grow, max_size());
                const size_type final_cap = std::ranges::max(new_cap, ceil_grow);
                return allocate_exclusive_buffer(view(), final_cap);
            } else if (new_cap > capacity()) {
                // Can use prefix to get enough capacity without reallocation.
                const auto alloc = storage.allocation();
                std::ranges::copy(view(), alloc.data());
                storage.dyn.data = alloc.data();
                return alloc.data();
            } else {
                // Already exclusive and having enough capacity
                // Nothing to do here.
                return storage.data();
            }
        }
    }

    char_type* exclusive_data() post(exclusive()) { return reserve_or_exclusive_shrink(size()); }

    // at(index) throw when index pass the end.
    constexpr void check_index(size_type index) const {
        if (index >= size()) {
            throw std::out_of_range("basic_cow_string: out of range");
        }
    }

    // insert/erase/replace allow pass-the-end position
    constexpr void check_pos(size_type index) const {
        if (index > size()) {
            throw std::out_of_range("basic_cow_string: out of range");
        }
    }

    bool is_my_iter(const_iterator it) const noexcept {
        static constexpr std::less_equal<void> le = {};
        const auto ptr = std::to_address(it);
        const auto v = view();
        return le(v.data(), ptr) && le(ptr, v.data() + v.size());
    }

    void adjust_old_for_replace(size_type pos, size_type count, size_type add_size)
        pre (pos <= size())
        post (exclusive())
    {
        const size_type old_size = size();
        const size_type rcount = std::ranges::min(count, old_size - pos);
        const size_type new_size = old_size - rcount + add_size;
        const auto allocate_for_replace = [&, this]<bool is_exclusive>(std::bool_constant<is_exclusive>) {
            size_type new_capacity = new_size;
            if constexpr (is_exclusive) {
                const size_type old_real_cap = real_capacity();
                contract_assert(new_size > old_real_cap);
                const size_type grow = std::saturating_mul(old_real_cap, growing_factor);
                const size_type ceil_grow = std::ranges::min(grow, max_size());
                const size_type final_cap = std::ranges::max(new_size, ceil_grow);
                new_capacity = final_cap;
            }
            auto* const control = details::allocate(get_allocator(), new_capacity);
            char_type* const data = control->template data<char_type>();
            // Copy head and tail of original string, and leave a hole for new content
            const char_type* const old_data = std::as_const(*this).data();
            std::ranges::copy_n(old_data, pos, data);
            std::ranges::copy_n(old_data + pos + rcount, old_size - pos - rcount, data + pos + add_size);
            deallocate_if_needed();
            std::start_lifetime(storage.dyn);
            storage.dyn = {
                .data = data, .size = new_size,
                .control = reinterpret_cast<std::uintptr_t>(control) >> 2
            };
        };
        if (!exclusive()) {
            allocate_for_replace(std::false_type{});
        } else if (new_size <= capacity()) {
            // If tail capacity is enough, move tail only
            char_type* const data = storage.data();
            std::ranges::copy_n(data + pos + rcount, old_size - pos - rcount, data + pos + add_size);
            if (status().allocated) {
                storage.dyn.size = new_size;
            } else {
                storage.sso.size = static_cast<unsigned char>(new_size);
            }
        } else if (new_size <= real_capacity()) {
            // If real capacity is enough, move head and tail to front
            contract_assert(status() == details::owned);
            const char_type* const data = storage.data();
            std::span<char_type> alloc = storage.allocation();
            std::ranges::copy_n(data, pos, alloc.data());
            std::ranges::copy_n(data + pos + rcount, old_size - pos - rcount, alloc.data() + pos + add_size);
            storage.dyn.data = alloc.data();
            storage.dyn.size = new_size;
        } else {
            // Need reallocation
            allocate_for_replace(std::true_type{});
        }
    }

    bool try_inplace_self_replace(size_type pos, size_type count,
                                  const char_type* src, size_type add_size)
    {
        if (!exclusive()) {
            return false;
        }
        const size_type old_size = size();
        const size_type rcount = std::ranges::min(count, old_size - pos);
        const size_type new_size = old_size - rcount + add_size;
        if (new_size > real_capacity()) {
            return false;
        }
        std::span<char_type> alloc = status().allocated
            ? storage.allocation()
            : std::span<char_type>(storage.sso.data);
        char_type* const data = alloc.data();
        const char_type* const old_data = std::as_const(*this).data();
        std::span<const char_type> head(old_data, pos);
        std::span<const char_type> tail(old_data + pos + rcount, old_data + old_size);
        std::span<const char_type> source(src, src + add_size);
        std::span<char_type> head_dest(data, pos);
        std::span<char_type> tail_dest(data + pos + add_size, data + new_size);
        std::span<char_type> source_dest(data + pos, data + pos + add_size);
        if (details::try_inplace_self_replace(
                std::as_bytes(head),
                std::as_bytes(tail),
                std::as_bytes(source),
                std::as_writable_bytes(head_dest),
                std::as_writable_bytes(tail_dest),
                std::as_writable_bytes(source_dest)
            )) {
            if (status().allocated) {
                storage.dyn.data = data;
                storage.dyn.size = new_size;
            } else {
                storage.sso.size = static_cast<unsigned char>(new_size);
            }
            return true;
        }
        return false;
    }

public:
    constexpr explicit basic_cow_string(const allocator_type& alloc) noexcept
        : allocator_type(alloc)
    {
        std::start_lifetime(storage.sso);
        storage.sso = {};
    }

    constexpr basic_cow_string()
        noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
        : basic_cow_string(allocator_type())
    {}

    constexpr basic_cow_string(size_type count, char_type ch,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        if consteval {
            write_static_string(std::views::repeat(ch, count));
        } else {
            append(count, ch);
        }
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, CharT>
    constexpr basic_cow_string(Iter iter, Sent sent,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        if consteval {
            write_static_string(std::ranges::subrange(std::move(iter), std::move(sent)));
        } else {
            append(std::move(iter), std::move(sent));
        }
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    constexpr basic_cow_string(std::from_range_t, R&& r,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        if consteval {
            write_static_string(std::forward<R>(r));
        } else {
            append_range(std::forward<R>(r));
        }
    }

    constexpr basic_cow_string(const char_type* s, size_type count,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(string_view_type(s, count), alloc)
    {}

    constexpr basic_cow_string(std::nullptr_t) = delete;

    constexpr basic_cow_string(const basic_cow_string& other) noexcept(always_equal_alloc)
        : basic_cow_string(other, other.allocator_copy())
    {}

    template<typename COWString>
        requires std::same_as<std::remove_cvref_t<COWString>, basic_cow_string>
    constexpr basic_cow_string(COWString&& other, const allocator_type& alloc)
        noexcept(always_equal_alloc)
        : basic_cow_string(std::forward<COWString>(other), 0, npos, alloc)
    {}

    constexpr basic_cow_string(const basic_cow_string& other, size_type pos,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(other, pos, npos, alloc)
    {}

    constexpr basic_cow_string(const basic_cow_string& other,
                               size_type pos, size_type count,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        const string_view_type view = other.subview(pos, count);
        if (can_write_sso(view.size()) || can_share_with(other)) {
            set_states_on_copy(other, pos, count);
        } else {
            basic_cow_string(view, get_allocator()).swap(*this);
        }
    }

    constexpr basic_cow_string(basic_cow_string&& other) noexcept
        : basic_cow_string(std::move(other).allocator_move())
    {
        // This ctor seems not able to delegate to another ctor safely,
        // so we handle the move manually here.
        set_states_on_move(std::move(other));
    }

    constexpr basic_cow_string(basic_cow_string&& other, size_type pos,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(std::move(other), pos, npos, alloc)
    {}

    constexpr basic_cow_string(basic_cow_string&& other,
                               size_type pos, size_type count,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        const string_view_type view = other.subview(pos, count);
        if (can_write_sso(view.size()) || can_share_with(other)) {
            set_states_on_move(std::move(other), pos, count);
        } else {
            basic_cow_string(view, get_allocator()).swap(*this);
        }
    }

    constexpr basic_cow_string(std::initializer_list<char_type> il,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(string_view_type(il), alloc)
    {}

    consteval basic_cow_string(const char_type* literal)
        noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
        : basic_cow_string(literal, allocator_type())
    {}

    constexpr basic_cow_string(details::literal_helper<char_type> literal,
                               const allocator_type& alloc) 
        : basic_cow_string(alloc)
    {
        std::start_lifetime(storage.sta);
        storage.sta = { .data = literal.data(), .size = literal.size() };
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    explicit constexpr basic_cow_string(const StringViewLike& sv) 
        : basic_cow_string(string_view_type(sv), allocator_type())
    {}

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
              && (!std::same_as<StringViewLike, basic_cow_string>)
    constexpr basic_cow_string(const StringViewLike& sv, const allocator_type& alloc)
        : basic_cow_string(alloc)
    {
        const string_view_type view = sv;
        const std::size_t size = view.size();
        if (size > storage.sso.capacity) {
            if consteval {
                write_static_string(view);
            } else {
                allocate_exclusive_buffer(view);
            }
        } else {
            write_sso(view);
        }
    }

    constexpr basic_cow_string(string_view_type sv,
                               size_type pos, size_type count,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(string_view_type(sv).subview(pos, count), alloc)
    {}

    constexpr basic_cow_string(string_view_type sv, size_type pos,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(sv, pos, npos, alloc)
    {}

    constexpr ~basic_cow_string() { deallocate_if_needed(); }

    // Non-modifying methods

#ifdef COW_STRING_TEST_STATUS

    // For testing purposes only
    constexpr bool is_small() const noexcept { return status() == details::small; }
    // For testing purposes only
    constexpr bool is_literal() const noexcept { return status() == details::literal; }
    // For testing purposes only
    constexpr bool is_owned() const noexcept { return status() == details::owned; }
    // For testing purposes only
    // Note that this only checks local status, not reference counter
    constexpr bool is_shared() const noexcept { return status() == details::shared; }
    // For testing purposes only
    // Maybe inaccurate in concurrent scenarios
    std::size_t cur_count() const noexcept {
        if (!status().allocated) {
            return 0;
        }
        return ref_count().load(std::memory_order_relaxed);
    }

#endif // COW_STRING_TEST_STATUS

    // Small buffer is exclusive, literal is not.
    // Allocation need further checking for shared references.
    constexpr bool exclusive() const noexcept
        post (r : r || status().shared)
    {
        const auto s = status();
        if (s == details::small || s == details::owned) {
            return true;
        }
        if (s == details::literal) {
            return false;
        }
        if (ref_count().load(std::memory_order_acquire) == 1) {
            storage.dyn.shared = 0;
            return true;
        }
        return false;
    }

    static constexpr size_type max_size() noexcept { return details::cow_max_size<size_type, char_type>; }
    constexpr size_type size() const noexcept { return storage.size(); }
    constexpr size_type length() const noexcept { return size(); }
    constexpr bool empty() const noexcept { return size() == 0; }
    constexpr string_view_type view() const noexcept { return storage.view(); }
    constexpr string_type string() const { return string_type(view(), get_allocator()); }
    constexpr operator string_view_type() const noexcept { return view(); }
    constexpr basic_cow_string clone() const { return { view(), allocator_copy() }; }
    constexpr const_pointer data() const noexcept { return storage.data(); }
    constexpr const_iterator begin() const noexcept { return const_iterator(storage.data()); }
    constexpr const_iterator end() const noexcept { return const_iterator(storage.data() + size()); }
    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() const noexcept { return end(); }
    constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    constexpr const_reverse_iterator crend() const noexcept { return rend(); }        
    constexpr const_reference front() const noexcept pre (!empty()) { return view().front(); }
    constexpr const_reference back() const noexcept pre (!empty()) { return view().back(); }
    constexpr const_reference operator[](size_type pos) const noexcept pre (pos < size()) { return view()[pos]; }

    // Capacity that append does not make reallocation or content relocation.
    constexpr size_type capacity() const noexcept { return storage.capacity(); }

    // Capacity that append does not make reallocation, but may cause content relocation.
    // Pointers and iterators are invalidated if such thing occurs.
    constexpr size_type real_capacity() const noexcept { return storage.real_capacity(); }

    constexpr const_reference at(size_type pos) const {
        check_index(pos);
        return view()[pos];
    }

    constexpr size_type find(string_view_type str, size_type pos = 0) const noexcept {
        return view().find(str, pos);
    }

    constexpr size_type find(char_type ch, size_type pos = 0) const noexcept {
        return view().find(ch, pos);
    }

    constexpr size_type find(const char_type* s, size_type pos, size_type count) const {
        return view().find(s, pos, count);
    }

    constexpr size_type rfind(string_view_type str, size_type pos = npos) const noexcept {
        return view().rfind(str, pos);
    }

    constexpr size_type rfind(char_type ch, size_type pos = npos) const noexcept {
        return view().rfind(ch, pos);
    }

    constexpr size_type rfind(const char_type* s, size_type pos, size_type count) const {
        return view().rfind(s, pos, count);
    }

    constexpr size_type find_first_of(string_view_type str, size_type pos = 0) const noexcept {
        return view().find_first_of(str, pos);
    }

    constexpr size_type find_first_of(char_type ch, size_type pos = 0) const noexcept {
        return view().find_first_of(ch, pos);
    }

    constexpr size_type find_first_of(const char_type* s, size_type pos, size_type count) const {
        return view().find_first_of(s, pos, count);
    }

    constexpr size_type find_first_not_of(string_view_type str, size_type pos = 0) const noexcept {
        return view().find_first_not_of(str, pos);
    }

    constexpr size_type find_first_not_of(char_type ch, size_type pos = 0) const noexcept {
        return view().find_first_not_of(ch, pos);
    }

    constexpr size_type find_first_not_of(const char_type* s, size_type pos, size_type count) const {
        return view().find_first_not_of(s, pos, count);
    }

    constexpr size_type find_last_of(string_view_type str, size_type pos = npos) const noexcept {
        return view().find_last_of(str, pos);
    }

    constexpr size_type find_last_of(char_type ch, size_type pos = npos) const noexcept {
        return view().find_last_of(ch, pos);
    }
    constexpr size_type find_last_of(const char_type* s, size_type pos, size_type count) const {
        return view().find_last_of(s, pos, count);
    }

    constexpr size_type find_last_not_of(string_view_type str, size_type pos = npos) const noexcept {
        return view().find_last_not_of(str, pos);
    }

    constexpr size_type find_last_not_of(char_type ch, size_type pos = npos) const noexcept {
        return view().find_last_not_of(ch, pos);
    }

    constexpr size_type find_last_not_of(const char_type* s, size_type pos, size_type count) const {
        return view().find_last_not_of(s, pos, count);
    }

    constexpr bool starts_with(string_view_type str) const noexcept {
        return view().starts_with(str);
    }

    constexpr bool starts_with(char_type ch) const noexcept {
        return view().starts_with(ch);
    }

    constexpr bool ends_with(string_view_type str) const noexcept {
        return view().ends_with(str);
    }

    constexpr bool ends_with(char_type ch) const noexcept {
        return view().ends_with(ch);
    }

    constexpr bool contains(string_view_type str) const noexcept {
        return view().contains(str);
    }

    constexpr bool contains(char_type ch) const noexcept {
        return view().contains(ch);
    }

    constexpr const allocator_type& get_allocator() const noexcept {
        return static_cast<const allocator_type&>(*this);
    }

    constexpr string_view_type subview(size_type pos = 0, size_type count = npos) const {
        return view().substr(pos, count);
    }

    constexpr basic_cow_string substr(size_type pos = 0, size_type count = npos) const {
        return { *this, pos, count, get_allocator() };
    }

    constexpr basic_cow_string substr(size_type pos = 0, size_type count = npos) && {
        return { std::move(*this), pos, count, get_allocator() };
    }

    constexpr bool operator==(const basic_cow_string& other) const noexcept { return view() == other.view(); }
    constexpr auto operator<=>(const basic_cow_string& other) const noexcept { return view() <=> other.view(); }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr bool operator==(const StringViewLike& other) const
        noexcept(std::is_nothrow_convertible_v<const StringViewLike&, string_view_type>)
    {
        return view() == string_view_type(other);
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr auto operator<=>(const StringViewLike& other) const
        noexcept(std::is_nothrow_convertible_v<const StringViewLike&, string_view_type>)
    {
        return view() <=> string_view_type(other);
    }

    constexpr int compare(details::literal_helper<char_type> literal) const noexcept {
        return view().compare(string_view_type(literal.data(), literal.size()));
    }

    constexpr int compare(size_type pos, size_type count,
                          details::literal_helper<char_type> literal) const
    {
        return view().compare(pos, count, string_view_type(literal.data(), literal.size()));
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr int compare(const StringViewLike& other) const
        noexcept(std::is_nothrow_convertible_v<const StringViewLike&, string_view_type>)
    {
        return view().compare(string_view_type(other));
    }

    template<std::convertible_to<string_view_type> StringViewLike>
    constexpr int compare(size_type pos, size_type count,
                          const StringViewLike& other) const
    {
        return view().compare(pos, count, string_view_type(other));
    }

    constexpr int compare(size_type pos1, size_type count1, string_view_type v,
                          size_type pos2, size_type count2) const
    {
        return view().compare(pos1, count1, v, pos2, count2);
    }

    constexpr int compare(size_type pos1, size_type count1,
                          const char_type* s, size_type count2) const
    {
        return view().compare(pos1, count1, s, count2);
    }

    constexpr size_type copy(char_type* dest, size_type count, size_type pos = 0) const {
        return view().copy(dest, count, pos);
    }

    // Modifying methods

    // Make a exclusive writable string if it not already is
    void ensure_exclusive() post(exclusive()) { (void) exclusive_data(); }

    constexpr void swap(basic_cow_string& other) noexcept(always_equal_alloc || POCS) {
        if (this == &other) {
            return;
        }
        if constexpr (always_equal_alloc) {
            std::ranges::swap(this->storage, other.storage);
        } else if constexpr (POCS) {
            std::ranges::swap(
                static_cast<allocator_type&>(*this),
                static_cast<allocator_type&>(other)
            );
            std::ranges::swap(this->storage, other.storage);
        } else if (this->get_allocator() == other.get_allocator()) {
            std::ranges::swap(this->storage, other.storage);
        } else {
            // According to standard container requirement, this is undefined behavior.
            // But we will make it well-defined for cow_string.
            auto copy_for_swap = [](const basic_cow_string& source, const allocator_type& alloc) {
                if (source.status().allocated) {
                    return basic_cow_string(source.view(), alloc);
                } else {
                    basic_cow_string rt(alloc);
                    rt.storage = source.storage;
                    return rt;
                }
            };
            basic_cow_string o_copy = copy_for_swap(other, this->get_allocator());
            basic_cow_string t_copy = copy_for_swap(*this, other.get_allocator());
            std::ranges::swap(this->storage, o_copy.storage);
            std::ranges::swap(other.storage, t_copy.storage);
        }
    }

    friend constexpr void swap(basic_cow_string& lhs, basic_cow_string& rhs)
        noexcept(noexcept(lhs.swap(rhs)))
    {
        lhs.swap(rhs);
    }

    constexpr basic_cow_string& assign(std::nullptr_t) = delete;
    constexpr basic_cow_string& operator=(std::nullptr_t) = delete;

    constexpr basic_cow_string& assign(const basic_cow_string& other)
        noexcept(always_equal_alloc || POCCA)
    {
        if (this == &other) {
            return *this;
        }
        if constexpr (always_equal_alloc) {
            deallocate_if_needed();
            set_states_on_copy(other);
        } else if constexpr (POCCA) {
            deallocate_if_needed();
            static_cast<allocator_type&>(*this) = other.get_allocator();
            set_states_on_copy(other);
        } else if (can_share_with(other)) {
            deallocate_if_needed();
            set_states_on_copy(other);
        } else {
            // Can not share with source, do deep copy
            *this = other.view();
        }
        return *this;
    }

    constexpr basic_cow_string& operator=(const basic_cow_string& other)
        noexcept(noexcept(this->assign(other)))
    {
        return assign(other);
    }

    constexpr basic_cow_string& assign(basic_cow_string&& other)
        noexcept(always_equal_alloc || POCMA)
    {
        if (this == &other) {
            return *this;
        }
        if constexpr (always_equal_alloc) {
            deallocate_if_needed();
            set_states_on_move(std::move(other));
        } else if constexpr (POCMA) {
            deallocate_if_needed();
            static_cast<allocator_type&>(*this) = static_cast<allocator_type&&>(other);
            set_states_on_move(std::move(other));
        } else if (can_share_with(other)) {
            deallocate_if_needed();
            set_states_on_move(std::move(other));
        } else {
            // Can not share with source, do deep copy
            *this = other.view();
        }
        return *this;
    }

    constexpr basic_cow_string& operator=(basic_cow_string&& other)
        noexcept(noexcept(this->assign(std::move(other))))
    {
        return assign(std::move(other));
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr basic_cow_string& assign(const StringViewLike& sv) {
        const string_view_type view = sv;
        return replace(0, npos, view.data(), view.data() + view.size());
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr basic_cow_string& operator=(const StringViewLike& sv) {
        return assign(sv);
    }

    constexpr basic_cow_string& assign(char_type ch) {
        return replace(0, npos, &ch, &ch + 1);
    }

    constexpr basic_cow_string& operator=(char_type ch) {
        return assign(ch);
    }

    constexpr basic_cow_string& assign(std::initializer_list<char_type> il) {
        const string_view_type view(il);
        return replace(0, npos, view.data(), view.data() + view.size());
    }

    constexpr basic_cow_string& operator=(std::initializer_list<char_type> il) {
        return assign(il);
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    constexpr basic_cow_string& assign(Iter iter, Sent sent) {
        if consteval {
            std::ranges::subrange r(std::move(iter), std::move(sent));
            write_static_string(std::move(r));
            return *this;
        } else {
            if constexpr (details::string_like_iter<Iter, char_type>) {
                const char_type* const data = std::to_address(iter);
                const auto length = std::ranges::distance(iter, sent);
                return replace(0, npos, data, data + length);
            } else {
                return replace(0, npos, std::move(iter), std::move(sent));
            }
        }
    }

    constexpr basic_cow_string& assign(size_type count, char_type ch) {
        if consteval {
            write_static_string(std::views::repeat(ch, count));
            return *this;
        } else {
            return replace(0, npos, count, ch);
        }
    }

    constexpr basic_cow_string& assign(const char_type* s, size_type count) {
        return replace(0, npos, s, s + count);
    }

    constexpr basic_cow_string& assign(const basic_cow_string& str,
                                       size_type pos, size_type count = npos)
    {
        basic_cow_string(str, pos, count).swap(*this);
        return *this;
    }

    constexpr basic_cow_string& assign(basic_cow_string&& str,
                                       size_type pos, size_type count = npos)
    {
        basic_cow_string(std::move(str), pos, count).swap(*this);
        return *this;
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr basic_cow_string& assign(const StringViewLike& sv,
                                       size_type pos, size_type count = npos)
    {
        const auto view = string_view_type(sv).subview(pos, count);
        return replace(0, npos, view.data(), view.data() + view.size());
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    constexpr basic_cow_string& assign_range(R&& r) {
        return replace_with_range(0, npos, std::forward<R>(r));
    }

    constexpr void clear() noexcept {
        if consteval {
            contract_assert(!status().allocated);
            std::start_lifetime(storage.sso);
            storage.sso = {};
        } else {
            resize(0);
        }
    }

    constexpr void shrink_to_fit() {
        // reserve_or_exclusive_shrink won't shrink
        // so we have to do it ourselves
        if (status().allocated) {
            if (can_write_sso(size())) {
                // In most part of cow_string, mechanisms eagerly write to SSO when possible.
                // The only possible way to reach here right now is reserve() then shrink.
                write_sso_release(view());
            } else if (!exclusive() || size() < capacity()) {
                allocate_exclusive_buffer(view());
            }
        }
    }

    constexpr void reserve(const size_type new_cap) {
        if !consteval {
            const size_type cap = std::ranges::max({ new_cap, size(), capacity() });
            reserve_or_exclusive_shrink(cap);
        }
    }

    pointer data() post (exclusive()) { return exclusive_data(); }
    iterator begin() post (exclusive()) { return iterator(data()); }
    iterator end() post (exclusive()) { return iterator(data() + size()); }
    reverse_iterator rbegin() post (exclusive()) { return reverse_iterator(end()); }
    reverse_iterator rend() post (exclusive()) { return reverse_iterator(begin()); }
    reference front() pre (!empty()) post (exclusive()) { return exclusive_data()[0]; }
    reference back() pre (!empty()) post (exclusive()) { return exclusive_data()[size() - 1]; }

    reference operator[](size_type pos)
        pre (pos < size())
        post (exclusive())
    {
        return exclusive_data()[pos];
    }
    
    reference at(size_type pos)
        post (exclusive())
    {
        check_index(pos);
        return exclusive_data()[pos];
    }

    template<std::invocable<char_type*, size_type> Operation>
    void resize_and_overwrite(size_type count, Operation op)
        post (exclusive())
    {
        // get necessary storage
        char_type* const p = reserve_or_exclusive_shrink(count);
        // now exclusive
        size_type r = std::move(op)(p, count);
        contract_assert(r <= count);
        if (status().allocated) {
            storage.dyn.size = r;
        } else {
            storage.sso.size = static_cast<unsigned char>(r);
        }
    }

    void resize(size_type new_size, char_type ch = char_type())
        post (exclusive())
    {
        const size_type old_size = size();
        resize_and_overwrite(new_size,
            [ch, old_size](char_type* p, size_type count) noexcept {
            if (count > old_size) {
                std::ranges::fill(p + old_size, p + count, ch);
            }
            return count;
        });
    }

    void push_back(char_type ch)
        post (exclusive())
    {
        const size_type old_size = size();
        resize_and_overwrite(old_size + 1,
            [ch, old_size](char_type* p, size_type count) noexcept {
            p[old_size] = ch;
            return count;
        });
    }

    void pop_back() pre (!empty()) post (exclusive()) { resize(size() - 1); }
    
    basic_cow_string& append(size_type count, char_type ch)
        post (exclusive())
    {
        resize(size() + count, ch);
        return *this;
    }

    basic_cow_string& append(string_view_type sv, size_type pos, size_type count = npos)
        post (exclusive())
    {
        const auto view = sv.subview(pos, count);
        return replace(size(), 0, view.data(), view.data() + view.size());
    }

    basic_cow_string& append(string_view_type sv)
        post (exclusive())
    {
        return replace(size(), 0, sv.data(), sv.data() + sv.size());
    }

    basic_cow_string& append(const char_type* s, size_type count)
        post (exclusive())
    {
        return replace(size(), 0, s, s + count);
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    basic_cow_string& append(Iter iter, Sent sent)
        post (exclusive())
    {
        if constexpr (details::string_like_iter<Iter, char_type>) {
            const char_type* const data = std::to_address(iter);
            const auto length = std::ranges::distance(iter, sent);
            return replace(size(), 0, data, data + length);
        } else {
            return replace(size(), 0, std::move(iter), std::move(sent));
        }
    }

    basic_cow_string& append(std::initializer_list<char_type> il)
        post (exclusive())
    {
        const string_view_type view(il);
        return replace(size(), 0, view.data(), view.data() + view.size());
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    basic_cow_string& append_range(R&& r)
        post (exclusive())
    {
        return replace_with_range(size(), 0, std::forward<R>(r));
    }

    basic_cow_string& operator+=(string_view_type sv)
        post (exclusive())
    {
        return append(sv);
    }

    basic_cow_string& operator+=(char_type ch)
        post (exclusive())
    {
        push_back(ch);
        return *this;
    }

    basic_cow_string& operator+=(std::initializer_list<char_type> il)
        post (exclusive())
    {
        return append(il);
    }

    basic_cow_string& insert(size_type index, size_type count, char_type ch)
        post (exclusive())
    {
        return replace(index, 0, count, ch);
    }

    basic_cow_string& insert(size_type index, char_type ch)
        post (exclusive())
    {
        return replace(index, 0, 1, ch);
    }

    basic_cow_string& insert(size_type index, string_view_type str)
        post (exclusive())
    {
        return replace(index, 0, str.data(), str.data() + str.size());
    }

    basic_cow_string& insert(size_type index, string_view_type str,
                             size_type s_index, size_type count = npos)
        post (exclusive())
    {
        const auto view = str.subview(s_index, count);
        return replace(index, 0, view.data(), view.data() + view.size());
    }

    basic_cow_string& insert(size_type index, const char_type* s, size_type count)
        post (exclusive())
    {
        return replace(index, 0, s, s + count);
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    basic_cow_string& insert(size_type index, Iter iter, Sent sent)
        post (exclusive())
    {
        if constexpr (details::string_like_iter<Iter, char_type>) {
            const char_type* const data = std::to_address(iter);
            const auto length = std::ranges::distance(iter, sent);
            return replace(index, 0, data, data + length);
        } else {
            return replace(index, 0, std::move(iter), std::move(sent));
        }
    }

    basic_cow_string& insert(size_type index, std::initializer_list<char_type> il)
        post (exclusive())
    {
        const string_view_type view(il);
        return replace(index, 0, view.data(), view.data() + view.size());
    }

    iterator insert(const_iterator pos, size_type count, char_type ch)
        pre (is_my_iter(pos))
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        replace(index, 0, count, ch);
        return begin() + index;
    }

    iterator insert(const_iterator pos, char_type ch)
        pre (is_my_iter(pos))
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        replace(index, 0, 1, ch);
        return begin() + index;
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    iterator insert(const_iterator pos, Iter iter, Sent sent)
        pre (is_my_iter(pos))
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        if constexpr (details::string_like_iter<Iter, char_type>) {
            const char_type* const data = std::to_address(iter);
            const auto length = std::ranges::distance(iter, sent);
            replace(index, 0, data, data + length);
        } else {
            replace(index, 0, std::move(iter), std::move(sent));
        }
        return begin() + index;
    }

    iterator insert(const_iterator pos, std::initializer_list<char_type> il)
        pre (is_my_iter(pos))
        post (exclusive())
    {
        const string_view_type sv(il);
        const size_type index = pos - cbegin();
        replace(index, 0, sv.data(), sv.data() + sv.size());
        return begin() + index;
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    iterator insert_range(const_iterator pos, R&& r)
        pre (is_my_iter(pos))
        post (exclusive())
    {
        // Transfer iterator to index eagerly to avoid issues if r is the current object.
        const size_type index = pos - cbegin();
        replace_with_range(index, 0, std::forward<R>(r));
        return begin() + index;
    }

    basic_cow_string& erase(size_type index = 0, size_type count = npos)
        post (exclusive())
    {
        return replace(index, count, 0, char_type());
    }

    iterator erase(const_iterator pos)
        pre (is_my_iter(pos))
        pre (pos != cend())
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        erase(index, 1);
        return begin() + index;
    }

    iterator erase(const_iterator first, const_iterator last)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        erase(index, count);
        return begin() + index;
    }

    basic_cow_string& replace(size_type pos, size_type count, string_view_type str,
                              size_type pos2, size_type count2 = npos)
        post (exclusive())
    {
        const auto view = str.subview(pos2, count2);
        return replace(pos, count, view.data(), view.data() + view.size());
    }

    basic_cow_string& replace(size_type pos, size_type count, string_view_type str)
        post (exclusive())
    {
        return replace(pos, count, str.data(), str.data() + str.size());
    }

    basic_cow_string& replace(const_iterator first, const_iterator last, string_view_type str)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, str.data(), str.data() + str.size());
    }

    basic_cow_string& replace(size_type pos, size_type count,
                              const char_type* cstr, size_type count2)
        post (exclusive())
    {
        return replace(pos, count, cstr, cstr + count2);
    }

    basic_cow_string& replace(const_iterator first, const_iterator last,
                              const char_type* cstr, size_type count2)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, cstr, cstr + count2);
    }

    basic_cow_string& replace(size_type pos, size_type count, size_type count2, char_type ch)
        post (exclusive())
    {
        check_pos(pos);
        adjust_old_for_replace(pos, count, count2);
        std::ranges::fill_n(storage.data() + pos, count2, ch);
        return *this;
    }
    
    basic_cow_string& replace(const_iterator first, const_iterator last,
                              size_type count2, char_type ch)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, count2, ch);
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    basic_cow_string& replace(size_type pos, size_type count, Iter iter, Sent sent)
        post (exclusive())
    {
        check_pos(pos);
        if constexpr (details::string_like_iter<Iter, char_type>) {
            const size_type add_size = std::ranges::distance(iter, sent);
            const char_type* const src = std::to_address(iter);
            if (!storage.self_alias(src)) {
                adjust_old_for_replace(pos, count, add_size);
                std::ranges::copy_n(src, add_size, storage.data() + pos);
                return *this;
            }
            if (try_inplace_self_replace(pos, count, src, add_size)) {
                // May fallthrough to the next branch if self-replacement is not possible.
                return *this;
            }
        } else if constexpr (details::can_direct_replace<Iter, char_type>()) {
            const size_type add_size = std::ranges::distance(iter, sent);
            const auto* const src = std::to_address(iter);
            // If someone break strick aliasing then we are cooked here.
            contract_assert(!storage.self_alias(src));
            adjust_old_for_replace(pos, count, add_size);
            std::ranges::copy_n(src, add_size, storage.data() + pos);
            return *this;
        }
        if constexpr (std::sized_sentinel_for<Sent, Iter> || std::forward_iterator<Iter>) {
            const size_type add_size = std::ranges::distance(iter, sent);
            if (status() == details::small && empty()) {
                // No one can alias with a empty string.
                // Just resize and write into the storage.
                adjust_old_for_replace(0, 0, add_size);
                try {
                    std::ranges::copy_n(iter, add_size, storage.data());
                } catch (...) {
                    resize(0);
                    shrink_to_fit();
                    throw;
                }
                return *this;
            }
            auto r = [&iter, add_size] {
                if constexpr (std::random_access_iterator<Iter>) {
                    return std::ranges::subrange(iter, iter + add_size);
                } else {
                    return std::ranges::subrange(
                        std::counted_iterator(std::move(iter), add_size),
                        std::default_sentinel
                    );
                }
            }();
            static constexpr std::size_t static_buffer_max = (1024 + 512) / sizeof(char_type);
            if (add_size <= static_buffer_max) {
                // Make stack buffer separate stack frame.
                return [this](this auto, size_type pos, size_type count, auto r) -> auto& {
                    using buffer_type = std::inplace_vector<char_type, static_buffer_max>;
                    buffer_type buffer(std::from_range, std::move(r));
                    const char_type* const data = buffer.data();
                    return replace(pos, count, data, data + buffer.size());
                }(pos, count, std::move(r));
            }
            string_type cp(std::from_range, std::move(r), get_allocator());
            const char_type* const data = cp.data();
            return replace(pos, count, data, data + cp.size());
        } else {
            if (status() == details::small && empty()) {
                // Although we know nothing about source range,
                // no one can alias with a empty string anyway.
                // This branch is mostly for constructors.
                try {
                    // Smaller buffer to inline
                    static constexpr std::size_t input_cache_max = 128 / sizeof(char_type);
                    std::inplace_vector<char_type, input_cache_max> buffer;
                    while (iter != sent) {
                        buffer.push_back(*iter);
                        if (buffer.size() == buffer.capacity()) {
                            append(buffer.data(), buffer.size());
                            buffer.clear();
                        }
                        ++iter;
                    }
                    if (!buffer.empty()) {
                        append(buffer.data(), buffer.size());
                    }
                } catch (...) {
                    clear();
                    shrink_to_fit();
                    throw;
                }
                return *this;
            } else {
                std::ranges::subrange r(std::move(iter), std::move(sent));
                string_type cp(std::from_range, std::move(r), get_allocator());
                const char_type* const data = cp.data();
                return replace(pos, count, data, data + cp.size());
            }
        }
    }

    template<std::input_iterator Iter, std::sentinel_for<Iter> Sent>
        requires std::convertible_to<std::iter_reference_t<Iter>, char_type>
    basic_cow_string& replace(const_iterator first, const_iterator last, Iter iter, Sent sent)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        if constexpr (details::string_like_iter<Iter, char_type>) {
            const char_type* const data = std::to_address(iter);
            const auto length = std::ranges::distance(iter, sent);
            return replace(index, count, data, data + length);
        } else {
            return replace(index, count, std::move(iter), std::move(sent));
        }
    }

    basic_cow_string& replace(size_type pos, size_type count,
                              std::initializer_list<char_type> il)
        post (exclusive())
    {
        const string_view_type view(il);
        return replace(pos, count, view.data(), view.data() + view.size());
    }

    basic_cow_string& replace(const_iterator first, const_iterator last,
                              std::initializer_list<char_type> il)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        const string_view_type view(il);
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, view.data(), view.data() + view.size());
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    basic_cow_string& replace_with_range(size_type pos, size_type count, R&& r)
        post (exclusive())
    {
        if constexpr (std::ranges::contiguous_range<R>) {
            const auto* const data = std::ranges::data(r);
            const auto length = std::ranges::distance(r);
            return replace(pos, count, data, data + length);
        } else if constexpr (std::ranges::sized_range<R>) {
            const auto size = std::ranges::size(r);
            if constexpr (std::ranges::random_access_range<R>) {
                const auto iter = std::ranges::begin(r);
                return replace(pos, count, iter, iter + size);
            } else {
                std::counted_iterator iter(std::ranges::begin(r), size);
                return replace(pos, count, std::move(iter), std::default_sentinel);
            }
        } else {
            return replace(pos, count, std::ranges::begin(r), std::ranges::end(r));
        }
    }

    template<std::ranges::input_range R>
        requires std::convertible_to<std::ranges::range_reference_t<R>, char_type>
    basic_cow_string& replace_with_range(const_iterator first, const_iterator last, R&& r)
        pre (is_my_iter(first) && is_my_iter(last))
        pre (first <= last)
        post (exclusive())
    {
        // Transfer iterators to index eagerly to avoid issues if r is the current object.
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace_with_range(index, count, std::forward<R>(r));
    }

private:
    storage_type storage = {};
};

template<std::input_iterator Iter, std::sentinel_for<Iter> Sent,
         typename Allocator = std::allocator<std::remove_cv_t<std::iter_value_t<Iter>>>>
basic_cow_string(Iter, Sent, Allocator = Allocator())
    -> basic_cow_string<std::remove_cv_t<std::iter_value_t<Iter>>, Allocator>;

template<details::string_like S, typename Allocator = std::allocator<details::char_type<S>>>
explicit basic_cow_string(const S&, Allocator = Allocator())
    -> basic_cow_string<details::char_type<S>, Allocator>;

template<details::string_like S, typename Allocator = std::allocator<details::char_type<S>>>
basic_cow_string(const S&,
                 typename basic_cow_string<details::char_type<S>, Allocator>::size_type,
                 typename basic_cow_string<details::char_type<S>, Allocator>::size_type,
                 Allocator = Allocator())
    -> basic_cow_string<details::char_type<S>, Allocator>;

template<std::ranges::input_range R,
         class Alloc = std::allocator<std::remove_cv_t<std::ranges::range_value_t<R>>>>
basic_cow_string(std::from_range_t, R&&, Alloc = Alloc())
    -> basic_cow_string<std::remove_cv_t<std::ranges::range_value_t<R>>, Alloc>;

template<typename CharT, typename Allocator, typename COWString = basic_cow_string<CharT, Allocator>>
constexpr COWString operator+(basic_cow_string<CharT, Allocator> lhs,
                              basic_cow_string<CharT, Allocator> rhs)
{
    if consteval {
        return COWString(std::basic_string<CharT>(lhs) + std::basic_string<CharT>(rhs));
    } else {
        const auto total_size = lhs.size() + rhs.size();
        const auto lhs_capacity = lhs.real_capacity();
        const auto rhs_capacity = rhs.real_capacity();
        if (lhs.exclusive() && lhs_capacity >= total_size) {
            lhs.append(rhs);
            return lhs;
        }
        if (rhs.exclusive() && rhs_capacity >= total_size) {
            rhs.insert(0, lhs);
            return rhs;
        }
        lhs.append(rhs);
        return lhs;
    }
}

template<typename CharT, typename Allocator, typename StringViewLike,
         typename COWString = basic_cow_string<CharT, Allocator>>
    requires std::convertible_to<const StringViewLike&, typename COWString::string_view_type>
          && (!std::convertible_to<const StringViewLike&, const CharT*>)
          && (!std::same_as<StringViewLike, COWString>)
constexpr COWString operator+(basic_cow_string<CharT, Allocator> lhs,
                              const StringViewLike& rhs)
{
    if consteval {
        return COWString(std::basic_string<CharT>(lhs) + std::basic_string<CharT>(rhs));
    } else {
        lhs.append(rhs);
        return lhs;
    }
}

template<typename CharT, typename Allocator, typename StringViewLike,
         typename COWString = basic_cow_string<CharT, Allocator>>
    requires std::convertible_to<const StringViewLike&, typename COWString::string_view_type>
          && (!std::convertible_to<const StringViewLike&, const CharT*>)
          && (!std::same_as<StringViewLike, CharT>)
constexpr COWString operator+(const StringViewLike& lhs,
                              basic_cow_string<CharT, Allocator> rhs)
{
    if consteval {
        return COWString(std::basic_string<CharT>(lhs) + std::basic_string<CharT>(rhs));
    } else {
        rhs.insert(0, lhs);
        return rhs;
    }
}

template<typename CharT, typename Allocator, typename COWString = basic_cow_string<CharT, Allocator>>
constexpr COWString operator+(basic_cow_string<CharT, Allocator> lhs, CharT rhs) {
    if consteval {
        return COWString(std::basic_string<CharT>(lhs) + rhs);
    } else {
        lhs.push_back(rhs);
        return lhs;
    }
}

template<typename CharT, typename Allocator, typename COWString = basic_cow_string<CharT, Allocator>>
constexpr COWString operator+(CharT lhs, basic_cow_string<CharT, Allocator> rhs) {
    if consteval {
        return COWString(lhs + std::basic_string<CharT>(rhs));
    } else {
        rhs.insert(0, lhs);
        return rhs;
    }
}

using cow_string = basic_cow_string<char>;
using cow_wstring = basic_cow_string<wchar_t>;
using cow_u8string = basic_cow_string<char8_t>;
using cow_u16string = basic_cow_string<char16_t>;
using cow_u32string = basic_cow_string<char32_t>;

namespace literals::inline cow_string_literals {

consteval cow_string operator""_cows(const char* str, std::size_t) { return str; }
consteval cow_wstring operator""_cows(const wchar_t* str, std::size_t) { return str; }
consteval cow_u8string operator""_cows(const char8_t* str, std::size_t) { return str; }
consteval cow_u16string operator""_cows(const char16_t* str, std::size_t) { return str; }
consteval cow_u32string operator""_cows(const char32_t* str, std::size_t) { return str; }

} // namespace cow::literals::cow_string_literals

} // namespace cow

namespace std {

template<typename CharT, typename Allocator, typename TargetCharT>
struct formatter<cow::basic_cow_string<CharT, Allocator>, TargetCharT>
    : private formatter<basic_string_view<CharT>, TargetCharT>
{
    using cow_string = cow::basic_cow_string<CharT, Allocator>;
    using base = std::formatter<basic_string_view<CharT>, TargetCharT>;
    using base::parse;

    template<typename FormatContext>
    auto format(const cow_string& str, FormatContext& ctx) const {
        return base::format(str.view(), ctx);
    }
};

template<typename CharT, typename Allocator>
struct hash<cow::basic_cow_string<CharT, Allocator>> {
    using cow_string = cow::basic_cow_string<CharT, Allocator>;
    using delegate = std::hash<typename cow_string::string_view_type>;

    static auto operator()(const cow_string& str) noexcept {
        return delegate{}(str.view());
    }
};

} // namespace std

#endif // COPY_ON_WRITE_STRING_H
