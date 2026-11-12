#pragma once
#ifndef MYLIB_COW_STRING_H
#define MYLIB_COW_STRING_H 1

#include <cstddef>
#include <concepts>
#include <type_traits>
#include <meta>
#include <climits>
#include <utility>
#include <iterator>
#include <string>
#include <string_view>
#include <memory>
#include <atomic>
#include <bit>
#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <functional>
#include <numeric>
#include <cstring>
#include <stdexcept>

namespace cow {

namespace details {

template<typename CharT>
inline constexpr bool is_valid_char_type = 
    std::is_same_v<CharT, char>
 || std::is_same_v<CharT, wchar_t>
 || std::is_same_v<CharT, char8_t>
 || std::is_same_v<CharT, char16_t>
 || std::is_same_v<CharT, char32_t>;

template<typename CharT, typename SizeT>
struct dyn_string {
    CharT* data = nullptr;
    SizeT size = 0;
    SizeT capacity : (sizeof(SizeT) * CHAR_BIT - 2) = 0;
    mutable SizeT shared : 1 = 0;
    SizeT allocated : 1 = 1;
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
    static constexpr std::size_t capacity = Size - 1;
};

template<typename CharT, std::size_t Size>
    requires (sizeof(CharT) == 1)
struct sso_string<CharT, Size> {
    CharT data[Size] = {};
    unsigned char size : (CHAR_BIT - 2) = 0;
    unsigned char shared : 1 = 0;
    unsigned char allocated : 1 = 0;
    static constexpr std::size_t capacity = Size - 1;
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

    template<typename Self, typename OP>
    constexpr auto visit(this Self& self, OP op) noexcept {
        const auto s = self.status();
        if (s == small) {
            return op(self.sso);
        } else if (s == literal) {
            if constexpr (std::is_const_v<Self>) {
                return op(self.sta);
            }
        } else if !consteval {
            return op(self.dyn);
        }
        std::unreachable();
    }

    // There is no writable string during compile time
    CharT* data() noexcept
        pre (!status().shared)
    {
        return visit([](auto& s) -> CharT* { return s.data; });
    }

    constexpr const CharT* data() const noexcept {
        return visit([](const auto& s) -> const CharT* { return s.data; });
    }

    constexpr SizeT size() const noexcept {
        return visit([](const auto& s) -> SizeT { return s.size; });
    }

    constexpr SizeT capacity() const noexcept {
        return visit([](const auto& s) -> SizeT { return s.capacity; });
    }

    using string_view_type = std::basic_string_view<CharT>;

    constexpr string_view_type view() const noexcept {
        return visit([](const auto& s) -> string_view_type { return { s.data, s.size }; });
    }
};

// shall never allocate through the allocator during constant evaluation
template<typename CharT>
struct allocation {
    CharT* data = nullptr;
    std::size_t capacity = 0;
};

template<typename SizeT>
inline constexpr SizeT cow_max_size = SizeT(1) << (sizeof(SizeT) * CHAR_BIT - 2);

template<typename Alloc, typename Traits = std::allocator_traits<Alloc>>
allocation<typename Traits::value_type> allocate(const Alloc& source, std::size_t capacity) {
    using allocator = Traits::template rebind_alloc<std::size_t>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    using size_type = traits::size_type;
    static_assert(sizeof(std::size_t) >= sizeof(value_type)
               && alignof(std::size_t) >= alignof(value_type));
    allocator alloc(source);
    if (capacity > cow_max_size<size_type>) {
        throw std::length_error("basic_cow_string: capacity too large");
    }
    const std::size_t total_size =
        (sizeof(value_type) * (capacity + 1) + sizeof(std::size_t) - 1) / sizeof(std::size_t) + 1;
    std::size_t* block = traits::allocate(alloc, total_size);
    traits::construct(alloc, block, 1);
    const std::size_t actual_capacity =
        (total_size - 1) * sizeof(std::size_t) / sizeof(value_type) - 1;
    return { reinterpret_cast<value_type*>(block + 1), actual_capacity };
}

template<typename Alloc, typename Traits = std::allocator_traits<Alloc>>
void deallocate(const Alloc& source, allocation<typename Traits::value_type> mem) noexcept {
    using allocator = Traits::template rebind_alloc<std::size_t>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    allocator alloc(source);
    std::size_t* block = reinterpret_cast<std::size_t*>(mem.data) - 1;
    const std::size_t total_size =
        (mem.capacity + 1) * sizeof(value_type) / sizeof(std::size_t) + 1;
    traits::destroy(alloc, block);
    traits::deallocate(alloc, block, total_size);
}

template<typename Iter, typename Sent, typename CharT>
concept eligible_iter_and_sent = std::input_iterator<std::decay_t<Iter>>
    && std::sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>
    && std::convertible_to<std::iter_reference_t<std::decay_t<Iter>>, CharT>;

template<typename R, typename CharT>
concept container_compatible_range = std::ranges::input_range<R>
    && std::convertible_to<std::ranges::range_reference_t<R>, CharT>;

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
    friend constexpr string_iterator operator+(difference_type n, string_iterator it) noexcept pre (it.cur != nullptr) { return it += n; }
    constexpr string_iterator operator-(difference_type n) const noexcept pre (cur != nullptr) { return auto(*this) -= n; }
    constexpr difference_type operator-(string_iterator other) const noexcept pre (cur != nullptr && other.cur != nullptr) { return cur - other.cur; }
    constexpr reference operator[](difference_type n) const noexcept pre (cur != nullptr) { return *(cur + n); }
    constexpr pointer operator->() const noexcept pre (cur != nullptr) { return cur; }

    friend constexpr bool operator==(string_iterator lhs, string_iterator rhs) = default;
    friend constexpr auto operator<=>(string_iterator lhs, string_iterator rhs) = default;

private:
    pointer cur = nullptr;
};

template<typename CharT>
void strmov(const CharT* src, const CharT* src_end, CharT* dst) noexcept {
    std::memmove(dst, src, (src_end - src) * sizeof(CharT));
}

template<typename S>
concept string_like = requires(const S& s) {
    std::basic_string_view<typename S::value_type>(s);
};

template<string_like S>
using char_type = S::value_type;

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
        && std::is_nothrow_move_constructible_v<allocator_type>
        && std::is_nothrow_move_assignable_v<allocator_type>);

    constexpr details::status status() const noexcept { return storage.status(); }

    template<details::container_compatible_range<char_type> R>
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

    constexpr char_type* write_sso(string_view_type view) noexcept
        pre (!status().allocated)
        pre (view.size() <= storage.sso.capacity)
        post (status() == details::small)
    {
        std::start_lifetime(storage.sso);
        storage.sso = {};
        char_type* const data = storage.sso.data;
        if (!view.empty()) {
            std::ranges::copy_n(view.data(), view.size(), data);
        }
        data[view.size()] = char_type();
        storage.sso.size = static_cast<unsigned char>(view.size());
        return storage.sso.data;
    }

    // Copy content of view into SSO buffer, view is referencing current object.
    // Then release the old dynamic storage if needed.
    constexpr char_type* write_sso_release(string_view_type view) noexcept
        pre (view.size() <= storage.sso.capacity)
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

    std::atomic_ref<std::size_t> ref_count() const noexcept
        pre (status().allocated)
    {
        return std::atomic_ref<std::size_t>(
            *(reinterpret_cast<std::size_t*>(storage.dyn.data) - 1)
        );
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

    constexpr void set_states_on_copy(const basic_cow_string& other) noexcept
        pre (!other.status().allocated || this->get_allocator() == other.get_allocator())
    {
        if (other.status().allocated) {
            // owned or shared string
            // share with source
            other.storage.dyn.shared = 1;
            storage = other.storage;
            ref_count().fetch_add(1, std::memory_order_relaxed);
        } else {
            storage = other.storage;
        }
    }

    constexpr void set_states_on_move(basic_cow_string&& other) noexcept {
        if (other.status().allocated) {
            // owned or shared string
            // steal from source and set it to sso empty string
            storage = other.storage;
            std::start_lifetime(other.storage.sso);
            other.storage.sso = {};
        } else {
            storage = other.storage;
        }
    }

    constexpr void deallocate_if_needed() noexcept
        post (!status().allocated)
    {
        const auto s = status();
        if (s.allocated) {
            if (!s.shared || ref_count().fetch_sub(1, std::memory_order_acq_rel) == 1) {
                details::deallocate(get_allocator(), { storage.dyn.data, storage.dyn.capacity });
            }
            std::start_lifetime(storage.sso);
            storage.sso = {};
        }
    }

    // If capacity < str.size(), exceeded contents are discarded
    char_type* allocate_exclusive_buffer(string_view_type str, std::size_t capacity) {
        const char_type* const str_data = str.data();
        const std::size_t size = std::ranges::min(str.size(), capacity);
        auto [data, actual_capacity] = details::allocate(get_allocator(), capacity);
        std::ranges::copy_n(str_data, size, data);
        data[size] = char_type();
        deallocate_if_needed();
        std::start_lifetime(storage.dyn);
        storage.dyn = { .data = data, .size = size, .capacity = actual_capacity };
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
            if (new_cap > storage.sso.capacity) {
                return allocate_exclusive_buffer(view(), new_cap);
            } else {
                return write_sso_release(subview(0, new_cap));
            }
        } else {
            const size_type old_cap = capacity();
            if (new_cap > old_cap) {
                // The auto growing branch
                const size_type grow = std::saturating_mul(old_cap, growing_factor);
                const size_type ceil_grow = std::ranges::min(grow, max_size());
                const size_type final_cap = std::ranges::max(new_cap, ceil_grow);
                return allocate_exclusive_buffer(view(), final_cap);
            } else {
                // Already exclusive and having enough capacity
                // Nothing to do here.
                return storage.data();
            }
        }
    }

    char_type* exclusive_data() { return reserve_or_exclusive_shrink(size()); }

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

    // Modification in-place only requires source range is valid,
    // so that it may have self-reference.
    template<std::contiguous_iterator It>
    bool is_my_pointer(const It& it) const noexcept {
        static constexpr std::less_equal<void> le = {};
        const auto ptr = std::to_address(it);
        const auto v = view();
        const bool within = le(v.data(), ptr) && le(ptr, v.data() + v.size());
        if (within) {
            // Check if some devil break strict aliasing rules.
            contract_assert(std::is_convertible_v<decltype(ptr), const char_type*>);
        }
        return within;
    }

    bool inplace_self_replace(size_type pos, size_type count,
                              const char_type* const src,
                              const size_type srclen) noexcept
        pre (pos <= size())
        pre (src != nullptr)
        post (r : !r || exclusive())
    {
        if (!exclusive()) {
            return false;
        }
        // Some non-owning users are aliasing us, but we can
        // try to do modification in-place if possible.
        // They are invalidated anyway.
        const size_type old_size = size();
        const size_type rcount = std::ranges::min(count, old_size - pos);
        const size_type new_size = old_size - rcount + srclen;
        const size_type cap = capacity();
        if (new_size > cap) {
            return false;
        }
        // Capacity is enough, so no reallocation happens and
        // the source range stays valid during the operation.
        char_type* const data = storage.data();
        char_type* const old_end = data + old_size;
        char_type* const new_end = data + new_size;
        const char_type* const src_end = src + srclen;
        char_type* const dst = data + pos;
        char_type* const dst_end = dst + rcount;
        char_type* const tail = new_end - (old_end - dst_end);
        if (new_size == old_size) {
            details::strmov(src, src_end, dst);
            return true;
        }
        if (new_size < old_size) {
            // Move the source and tail before shrinking.
            // resize() writes a null terminator at the new end,
            // which used to clobber data that was still needed
            // by the tail move.
            details::strmov(src, src_end, dst);
            details::strmov(dst_end, old_end, tail);
            resize(new_size);
            return true;
        }
        // Growing
        resize(new_size);
        if (src <= dst) {
            // Source lies before (or at) the destination,
            // so moving the tail cannot clobber it.
            details::strmov(dst_end, old_end, tail);
            details::strmov(src, src_end, dst);
            return true;
        }
        // Source starts right of the destination.
        // The part before the old tail is copied in place,
        // the rest is relocated with the tail and then
        // copied from its new location.
        const difference_type prefix = std::ranges::max(dst_end - src, 0z);
        if (prefix != 0) {
            details::strmov(src, dst_end, dst);
        }
        details::strmov(dst_end, old_end, tail);
        const char_type* const moved =
            (src < dst_end ? dst_end : src) + srclen - rcount;
        details::strmov(moved, moved + (srclen - prefix),
                        dst + prefix);
        return true;
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

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    constexpr basic_cow_string(Iter&& iter, Sent&& sent,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(alloc)
    {
        if consteval {
            write_static_string(
                std::ranges::subrange(std::forward<Iter>(iter), std::forward<Sent>(sent))
            );
        } else {
            append(std::forward<Iter>(iter), std::forward<Sent>(sent));
        }
    }

    template<details::container_compatible_range<char_type> R>
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
        if (pos == 0 && count >= other.size() &&
            (!other.status().allocated || this->get_allocator() == other.get_allocator())) {
            set_states_on_copy(other);
        } else {
            basic_cow_string(other.subview(pos, count), get_allocator()).swap(*this);
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
        other.check_pos(pos);
        if consteval {
            if (pos == 0 && count >= other.size()) {
                set_states_on_move(std::move(other));
            } else {
                write_static_string(other.subview(pos, count));
            }
        } else {
            const auto shrink_in_place = [this](size_type pos, size_type count) noexcept {
                contract_assert(exclusive());
                char_type* const data = storage.data();
                const size_type original_size = size();
                const size_type final_size = std::ranges::min(count, original_size - pos);
                std::ranges::copy_n(data + pos, final_size, data);
                resize(final_size);
            };
            if (other.status() == details::small) {
                set_states_on_move(std::move(other));
                shrink_in_place(pos, count);
                return;
            }
            if (other.status() == details::literal && pos == 0 && count >= other.size()) {
                set_states_on_move(std::move(other));
                return;
            }
            if (this->get_allocator() == other.get_allocator()) {
                if (pos == 0 && count >= other.size()) {
                    set_states_on_move(std::move(other));
                    return;
                }
                if (other.exclusive()) {
                    set_states_on_move(std::move(other));
                    shrink_in_place(pos, count);
                    return;
                }
            }
            basic_cow_string(other.subview(pos, count), get_allocator()).swap(*this);
        }
    }

    constexpr basic_cow_string(std::initializer_list<char_type> il,
                               const allocator_type& alloc = allocator_type())
        : basic_cow_string(il.begin(), il.end(), alloc)
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
        : basic_cow_string(string_view_type(sv).substr(pos, count), alloc)
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

    // Check if writing ops would require sso-filling/allocation first
    constexpr bool exclusive() const noexcept {
        const auto s = status();
        if (s == details::small) {
            return true;
        }
        if (s == details::shared || s == details::literal) {
            return false;
        }
        if (ref_count().load(std::memory_order_acquire) == 1) {
            storage.dyn.shared = 0;
            return true;
        }
        return false;
    }

    static constexpr size_type max_size() noexcept { return details::cow_max_size<size_type>; }
    constexpr size_type size() const noexcept { return storage.size(); }
    constexpr size_type capacity() const noexcept { return storage.capacity(); }
    constexpr size_type length() const noexcept { return size(); }
    constexpr bool empty() const noexcept { return size() == 0; }
    constexpr string_view_type view() const noexcept { return storage.view(); }
    constexpr string_type string() const { return string_type(view(), get_allocator()); }
    constexpr operator string_view_type() const noexcept { return view(); }
    constexpr basic_cow_string clone() const { return { view(), allocator_copy() }; }
    constexpr const_pointer data() const noexcept { return storage.data(); }
    constexpr const_pointer c_str() const noexcept { return storage.data(); }
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
    void ensure_exclusive() { (void) exclusive_data(); }

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
        } else if (!other.status().allocated || this->get_allocator() == other.get_allocator()) {
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
        if (always_equal_alloc) {
            deallocate_if_needed();
            set_states_on_move(std::move(other));
        } else if constexpr (POCMA) {
            deallocate_if_needed();
            static_cast<allocator_type&>(*this) = static_cast<allocator_type&&>(other);
            set_states_on_move(std::move(other));
        } else if (!other.status().allocated || this->get_allocator() == other.get_allocator()) {
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
        return assign(view.begin(), view.end());
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<const StringViewLike&, const char_type*>)
    constexpr basic_cow_string& operator=(const StringViewLike& sv) {
        return assign(sv);
    }

    constexpr basic_cow_string& assign(char_type ch) {
        return assign(string_view_type(&ch, 1));
    }

    constexpr basic_cow_string& operator=(char_type ch) {
        return assign(ch);
    }

    constexpr basic_cow_string& assign(std::initializer_list<char_type> il) {
        return assign(string_view_type(il));
    }

    constexpr basic_cow_string& operator=(std::initializer_list<char_type> il) {
        return assign(il);
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    constexpr basic_cow_string& assign(Iter&& iter, Sent&& sent) {
        if consteval {
            std::ranges::subrange r(std::forward<Iter>(iter), std::forward<Sent>(sent));
            write_static_string(std::move(r));
            return *this;
        } else {
            return replace(0, npos, std::forward<Iter>(iter), std::forward<Sent>(sent));
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
        return assign(string_view_type(s, count));
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

    template<details::container_compatible_range<char_type> R>
    constexpr basic_cow_string& assign_range(R&& r) {
        return assign(std::ranges::begin(r), std::ranges::end(r));
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
            if (size() <= storage.sso.capacity) {
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
            storage.sso.size = r;
        }
        p[r] = char_type();
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
        return append(sv.substr(pos, count));
    }

    basic_cow_string& append(string_view_type sv)
        post (exclusive())
    {
        return append(sv.begin(), sv.end());
    }

    basic_cow_string& append(const char_type* s, size_type count)
        post (exclusive())
    {
        return append(string_view_type(s, count));
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    basic_cow_string& append(Iter&& iter, Sent&& sent)
        post (exclusive())
    {
        return replace(size(), npos, std::forward<Iter>(iter), std::forward<Sent>(sent));
    }

    basic_cow_string& append(std::initializer_list<char_type> il)
        post (exclusive())
    {
        return append(string_view_type(il));
    }

    template<details::container_compatible_range<char_type> R>
    basic_cow_string& append_range(R&& r)
        post (exclusive())
    {
        // TODO: add approximately_sized_range branch when supported
        return append(std::ranges::begin(r), std::ranges::end(r));
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
        return insert(index, 1, ch);
    }

    basic_cow_string& insert(size_type index, string_view_type str)
        post (exclusive())
    {
        return insert(index, str.begin(), str.end());
    }

    basic_cow_string& insert(size_type index, string_view_type str,
                             size_type s_index, size_type count = npos)
        post (exclusive())
    {
        return insert(index, str.substr(s_index, count));
    }

    basic_cow_string& insert(size_type index, const char_type* s, size_type count)
        post (exclusive())
    {
        return insert(index, string_view_type(s, count));
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    basic_cow_string& insert(size_type index, Iter&& iter, Sent&& sent)
        post (exclusive())
    {
        return replace(index, 0, std::forward<Iter>(iter), std::forward<Sent>(sent));
    }

    basic_cow_string& insert(size_type index, std::initializer_list<char_type> il)
        post (exclusive())
    {
        return insert(index, il.begin(), il.end());
    }

    iterator insert(const_iterator pos, size_type count, char_type ch)
        pre (is_my_pointer(pos))
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        insert(index, count, ch);
        return begin() + index;
    }

    iterator insert(const_iterator pos, char_type ch)
        pre (is_my_pointer(pos))
        post (exclusive())
    {
        return insert(pos, 1, ch);
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    iterator insert(const_iterator pos, Iter&& iter, Sent&& sent)
        pre (is_my_pointer(pos))
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        insert(index, std::forward<Iter>(iter), std::forward<Sent>(sent));
        return begin() + index;
    }

    iterator insert(const_iterator pos, std::initializer_list<char_type> il)
        pre (is_my_pointer(pos))
        post (exclusive())
    {
        return insert(pos, il.begin(), il.end());
    }

    template<details::container_compatible_range<char_type> R>
    iterator insert_range(const_iterator pos, R&& r)
        pre (is_my_pointer(pos))
        post (exclusive())
    {
        // Transfer iterator to index eagerly to avoid issues if r is the current object.
        const size_type index = pos - cbegin();
        insert(index, std::ranges::begin(r), std::ranges::end(r));
        return begin() + index;
    }

    basic_cow_string& erase(size_type index = 0, size_type count = npos)
        post (exclusive())
    {
        return replace(index, count, 0, char_type());
    }

    iterator erase(const_iterator pos)
        pre (is_my_pointer(pos))
        pre (pos != cend())
        post (exclusive())
    {
        const size_type index = pos - cbegin();
        erase(index, 1);
        return begin() + index;
    }

    iterator erase(const_iterator first, const_iterator last)
        pre (is_my_pointer(first) && is_my_pointer(last))
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
        return replace(pos, count, str.substr(pos2, count2));
    }

    basic_cow_string& replace(size_type pos, size_type count, string_view_type str)
        post (exclusive())
    {
        return replace(pos, count, str.begin(), str.end());
    }

    basic_cow_string& replace(const_iterator first, const_iterator last, string_view_type str)
        pre (is_my_pointer(first) && is_my_pointer(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, str);
    }

    basic_cow_string& replace(size_type pos, size_type count,
                              const char_type* cstr, size_type count2)
        post (exclusive())
    {
        return replace(pos, count, string_view_type(cstr, count2));
    }

    basic_cow_string& replace(const_iterator first, const_iterator last,
                              const char_type* cstr, size_type count2)
        post (exclusive())
    {
        return replace(first, last, string_view_type(cstr, count2));
    }

    basic_cow_string& replace(size_type pos, size_type count, size_type count2, char_type ch)
        post (exclusive())
    {
        check_pos(pos);
        const size_type old_size = size();
        const size_type rcount = std::ranges::min(count, old_size - pos);
        const size_type add_size = count2;
        const size_type new_size = old_size - rcount + add_size;
        if (new_size == old_size) {
            char_type* const data = exclusive_data();
            std::ranges::fill_n(data + pos, add_size, ch);
        } else if (new_size > old_size) {
            resize(new_size);
            char_type* const data = storage.data();
            details::strmov(data + pos + rcount, data + old_size,
                            data + pos + add_size);
            std::ranges::fill_n(data + pos, add_size, ch);
        } else {
            char_type* const data = exclusive_data();
            std::ranges::fill_n(data + pos, add_size, ch);
            if (add_size != rcount) {
                details::strmov(data + pos + rcount, data + old_size,
                                data + pos + add_size);
            }
            resize(new_size);
        }
        return *this;
    }
    
    basic_cow_string& replace(const_iterator first, const_iterator last,
                              size_type count2, char_type ch)
        pre (is_my_pointer(first) && is_my_pointer(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, count2, ch);
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    basic_cow_string& replace(size_type pos, size_type count, Iter&& iter, Sent&& sent)
        post (exclusive())
    {
        check_pos(pos);
        if constexpr (std::contiguous_iterator<std::decay_t<Iter>>) {
            const size_type add_size = std::ranges::distance(iter, sent);
            const char_type* const src = std::to_address(iter);
            if (!is_my_pointer(iter)) {
                // The fast path for when iter is not alias with current string data
                const char_type* const src_end = src + add_size;
                const size_type old_size = size();
                const size_type rcount = std::ranges::min(count, old_size - pos);
                const size_type new_size = old_size - rcount + add_size;
                if (new_size == old_size) {
                    char_type* const data = exclusive_data();
                    details::strmov(src, src_end, data + pos);
                } else if (new_size > old_size) {
                    resize(new_size);
                    char_type* const data = storage.data();
                    details::strmov(data + pos + rcount, data + old_size,
                                    data + pos + add_size);
                    details::strmov(src, src_end, data + pos);
                } else {
                    char_type* const data = exclusive_data();
                    details::strmov(src, src_end, data + pos);
                    if (add_size != rcount) {
                        details::strmov(data + pos + rcount, data + old_size,
                                        data + pos + add_size);
                    }
                    resize(new_size);
                }
                return *this;
            }
            if (inplace_self_replace(pos, count, src, add_size)) {
                return *this;
            }
            // Fallback, but we already know length
            string_type cp(src, add_size, get_allocator());
            return replace(pos, count, cp);
        } else {
            // Fallback
            std::ranges::subrange r(std::forward<Iter>(iter), std::forward<Sent>(sent));
            string_type cp(std::from_range, std::move(r), get_allocator());
            return replace(pos, count, cp);
        }
    }

    template<typename Iter, typename Sent>
        requires details::eligible_iter_and_sent<Iter, Sent, char_type>
    basic_cow_string& replace(const_iterator first, const_iterator last, Iter&& iter, Sent&& sent)
        pre (is_my_pointer(first) && is_my_pointer(last))
        pre (first <= last)
        post (exclusive())
    {
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, std::forward<Iter>(iter), std::forward<Sent>(sent));
    }

    basic_cow_string& replace(size_type pos, size_type count,
                              std::initializer_list<char_type> il)
        post (exclusive())
    {
        return replace(pos, count, il.begin(), il.end());
    }

    basic_cow_string& replace(const_iterator first, const_iterator last,
                              std::initializer_list<char_type> il)
        pre (is_my_pointer(first) && is_my_pointer(last))
        pre (first <= last)
        post (exclusive())
    {
        return replace(first, last, il.begin(), il.end());
    }

    template<details::container_compatible_range<char_type> R>
    basic_cow_string& replace_with_range(size_type pos, size_type count, R&& r)
        post (exclusive())
    {
        return replace(pos, count, std::ranges::begin(r), std::ranges::end(r));
    }

    template<details::container_compatible_range<char_type> R>
    basic_cow_string& replace_with_range(const_iterator first, const_iterator last, R&& r)
        pre (is_my_pointer(first) && is_my_pointer(last))
        pre (first <= last)
        post (exclusive())
    {
        // Transfer iterators to index eagerly to avoid issues if r is the current object.
        const size_type index = first - cbegin();
        const size_type count = last - first;
        return replace(index, count, std::ranges::begin(r), std::ranges::end(r));
    }

private:
    storage_type storage = {};
};

template<typename Iter, std::sentinel_for<Iter> Sent,
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
    -> basic_cow_string<std::ranges::range_value_t<R>, Alloc>;

template<typename CharT, typename Allocator, typename COWString = basic_cow_string<CharT, Allocator>>
constexpr COWString operator+(basic_cow_string<CharT, Allocator> lhs,
                              basic_cow_string<CharT, Allocator> rhs)
{
    if consteval {
        return COWString(std::basic_string<CharT>(lhs) + std::basic_string<CharT>(rhs));
    } else {
        const auto total_size = lhs.size() + rhs.size();
        const auto lhs_capacity = lhs.capacity();
        const auto rhs_capacity = rhs.capacity();
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
    using result_type = std::size_t;

    result_type operator()(const cow_string& str) const noexcept {
        return std::hash<std::basic_string_view<CharT>>{}(str.view());
    }
};

} // namespace std

#endif // MYLIB_COW_STRING_H
