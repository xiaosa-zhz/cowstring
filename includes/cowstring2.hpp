#pragma once
#ifndef MYLIB_COW_STRING2_H
#define MYLIB_COW_STRING2_H 1

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <type_traits>
#include <meta>
#include <climits>
#include <new>
#include <utility>
#include <iterator>
#include <string>
#include <string_view>
#include <memory>
#include <format>
#include <atomic>
#include <bit>
#include <algorithm>
#include <initializer_list>
#include <ranges>

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
    SizeT capacity : (CHAR_BIT - 2) = 0;
    mutable SizeT shared : 1 = 0;
    SizeT allocated : 1 = 1;
};

template<typename CharT, typename SizeT>
struct sta_string {
    const CharT* data = nullptr;
    SizeT size = 0;
    SizeT _ : (CHAR_BIT - 2) = 0;
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

    constexpr status status() const noexcept {
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
    CharT* data() noexcept {
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

template<typename Alloc, typename Traits = std::allocator_traits<Alloc>>
allocation<typename Traits::value_type> allocate(const Alloc& source, std::size_t capacity) {
    using allocator = Traits::template rebind_alloc<std::size_t>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    static_assert(sizeof(std::size_t) >= sizeof(value_type) && alignof(std::size_t) >= alignof(value_type));
    allocator alloc(source);
    const std::size_t total_size = (sizeof(value_type) * (capacity + 1) + sizeof(std::size_t) - 1) / sizeof(std::size_t) + 1;
    std::size_t* block = traits::allocate(alloc, total_size);
    traits::construct(alloc, block, 1);
    const std::size_t actual_capacity = (total_size - 1) * sizeof(std::size_t) / sizeof(value_type) - 1;
    return { reinterpret_cast<value_type*>(block + 1), actual_capacity };
}

template<typename Alloc, typename Traits = std::allocator_traits<Alloc>>
void deallocate(const Alloc& source, allocation<typename Traits::value_type> mem) noexcept {
    using allocator = Traits::template rebind_alloc<std::size_t>;
    using traits = std::allocator_traits<allocator>;
    using value_type = Traits::value_type;
    allocator alloc(source);
    std::size_t* block = reinterpret_cast<std::size_t*>(mem.data) - 1;
    const std::size_t total_size = (mem.capacity + 1) * sizeof(value_type) / sizeof(std::size_t) + 1;
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

    consteval const CharT* data() const noexcept { return str; }
    consteval std::size_t size() const noexcept { return length; }

private:
    const CharT* str;
    std::size_t length;
};

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
    using const_iterator = const value_type*;
    using iterator = value_type*;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator = std::reverse_iterator<iterator>;

    using string_view_type = std::basic_string_view<value_type>;
    using string_type = std::basic_string<value_type>;

    static constexpr size_type npos = static_cast<size_type>(-1);

private:
    static_assert(std::is_same_v<value_type, typename allocator_traits_type::value_type>);
    static_assert(details::is_valid_char_type<CharT>);
    static_assert(std::is_pointer_v<pointer> && std::is_pointer_v<const_pointer>
        && std::is_same_v<std::remove_const_t<std::remove_pointer_t<const_pointer>>, char_type>
        && std::is_same_v<std::remove_pointer_t<pointer>, char_type>);
    using storage_type = details::storage<char_type, size_type>;
    static_assert(sizeof(storage_type) == sizeof(storage_type::dyn));

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
    static constexpr bool always_equal_alloc = allocator_traits_type::is_always_equal;
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
        string_type test(std::from_range, std::forward<R>(r));
        if (test.size() <= storage.sso.capacity) {
            write_sso(test);
        } else {
            storage.sta = { .data = std::define_static_string(test), .size = test.size() };
        }
    }

    constexpr char_type* write_sso(string_view_type view) noexcept
        pre (view.size() <= storage.sso.capacity)
        post (status() == details::small)
    {
        const char_type* data = storage.sso.data;
        if (!view.empty()) {
            std::ranges::copy_n(view.data(), view.size(), data);
        }
        data[view.size()] = char_type();
        storage.sso.size = static_cast<unsigned char>(view.size());
        return storage.sso.data;
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
        pre (this->get_allocator() == other.get_allocator() || !other.status().allocated)
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
            other.storage.sso = {};
        } else {
            storage = other.storage;
        }
    }

    constexpr void deallocate_if_needed() noexcept {
        const auto s = status();
        if (s.allocated && (!s.shared || ref_count().fetch_sub(1, std::memory_order_acq_rel) == 1)) {
            details::deallocate(get_allocator(), { storage.dyn.data, storage.dyn.capacity });
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
                return write_sso(subview(0, new_cap));
            }
        } else {
            const size_type old_cap = capacity();
            if (new_cap > old_cap) {
                // The auto growing branch
                return allocate_exclusive_buffer(view(), std::ranges::max(new_cap, old_cap * 2));
            } else {
                // Already exclusive and having enough capacity
                // Nothing to do here.
                return data();
            }
        }
    }

    char_type* exclusive_data() { return reserve_or_exclusive_shrink(size()); }

public:
    constexpr explicit basic_cow_string(const allocator_type& alloc) noexcept
        : allocator_type(alloc)
    {}

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
        : allocator_type(other.allocator_copy())
    {
        if (this->get_allocator() == other.get_allocator() || !other.status().allocated) {
            set_states_on_copy(other);
        } else {
            basic_cow_string(other.view(), get_allocator()).swap(*this);
        }
    }

    constexpr basic_cow_string(basic_cow_string&& other) noexcept
        : allocator_type(std::move(other).allocator_move())
    {
        set_states_on_move(std::move(other));
    }

    constexpr basic_cow_string(const basic_cow_string& other,
                               size_type pos, size_type count)
        : basic_cow_string(other.substr(pos, count))
    {}

    constexpr basic_cow_string(const basic_cow_string& other, size_type pos)
        : basic_cow_string(other, pos, npos)
    {}

    constexpr basic_cow_string(basic_cow_string&& other,
                               size_type pos, size_type count)
        : basic_cow_string(std::move(other).substr(pos, count))
    {}

    constexpr basic_cow_string(basic_cow_string&& other, size_type pos)
        : basic_cow_string(std::move(other), pos, npos)
    {}

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
        storage.sta = { .data = literal.data(), .size = literal.size() };
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<std::decay_t<StringViewLike>, const char_type*>)
    constexpr basic_cow_string(const StringViewLike& sv,
                               const allocator_type& alloc = allocator_type()) 
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

    constexpr size_type size() const noexcept { return storage.size(); }
    constexpr size_type capacity() const noexcept { return storage.capacity(); }
    constexpr size_type length() const noexcept { return size(); }
    constexpr bool empty() const noexcept { return size() == 0; }
    constexpr string_view_type view() const noexcept { return storage.view(); }
    constexpr string_type string() const { return string_type(view()); }
    constexpr operator string_view_type() const noexcept { return view(); }
    constexpr basic_cow_string clone() const { return { view(), allocator_copy() }; }
    constexpr const_pointer data() const noexcept { return storage.data(); }
    constexpr const_pointer c_str() const noexcept { return storage.data(); }
    constexpr const_iterator begin() const noexcept { return storage.data(); }
    constexpr const_iterator end() const noexcept { return storage.data() + size(); }
    constexpr const_iterator cbegin() const noexcept { return begin(); }
    constexpr const_iterator cend() const noexcept { return end(); }
    constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    constexpr const_reverse_iterator crend() const noexcept { return rend(); }        
    constexpr const_reference front() const noexcept pre (size() > 0) { return view().front(); }
    constexpr const_reference back() const noexcept pre (size() > 0) { return view().back(); }
    constexpr const_reference operator[](size_type pos) const noexcept pre (pos < size()) { return view()[pos]; }

    constexpr const_reference at(size_type pos) const {
        if (pos < size()) {
            return view()[pos];
        }
        throw std::out_of_range("basic_cow_string::at: position out of range");
    }

    constexpr size_type find(string_view_type str, size_type pos = 0) const noexcept {
        return view().find(str, pos);
    }

    constexpr size_type find(char_type ch, size_type pos = 0) const noexcept {
        return view().find(ch, pos);
    }

    constexpr size_type rfind(string_view_type str, size_type pos = npos) const noexcept {
        return view().rfind(str, pos);
    }

    constexpr size_type rfind(char_type ch, size_type pos = npos) const noexcept {
        return view().rfind(ch, pos);
    }

    constexpr size_type find_first_of(string_view_type str, size_type pos = 0) const noexcept {
        return view().find_first_of(str, pos);
    }

    constexpr size_type find_first_of(char_type ch, size_type pos = 0) const noexcept {
        return view().find_first_of(ch, pos);
    }

    constexpr size_type find_first_not_of(string_view_type str, size_type pos = 0) const noexcept {
        return view().find_first_not_of(str, pos);
    }

    constexpr size_type find_first_not_of(char_type ch, size_type pos = 0) const noexcept {
        return view().find_first_not_of(ch, pos);
    }

    constexpr size_type find_last_of(string_view_type str, size_type pos = npos) const noexcept {
        return view().find_last_of(str, pos);
    }

    constexpr size_type find_last_of(char_type ch, size_type pos = npos) const noexcept {
        return view().find_last_of(ch, pos);
    }

    constexpr size_type find_last_not_of(string_view_type str, size_type pos = npos) const noexcept {
        return view().find_last_not_of(str, pos);
    }

    constexpr size_type find_last_not_of(char_type ch, size_type pos = npos) const noexcept {
        return view().find_last_not_of(ch, pos);
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

    constexpr string_view_type subview(size_type pos = 0, size_type count = npos) const noexcept {
        return view().substr(pos, count);
    }

    constexpr basic_cow_string substr(size_type pos = 0, size_type count = npos) const {
        if (pos == 0 && count >= size()) {
            return auto(*this);
        } else {
            return basic_cow_string(view(), pos, count, allocator_copy());
        }
    }

    constexpr basic_cow_string substr(size_type pos = 0, size_type count = npos) && {
        if consteval {
            return std::as_const(*this).substr(pos, count);
        } else {
            const size_type original_size = size();
            if (pos == 0 && count >= original_size) {
                return std::move(*this);
            } else if (exclusive()) {
                if (pos > 0) {
                    const string_view_type sub = subview(pos, count);
                    if (status().shared) {
                        write_allocated_buffer(sub);
                    } else {
                        write_sso(sub);
                    }
                } else if (count < original_size) {
                    resize(count);
                }
                return std::move(*this);
            } else {
                return std::as_const(*this).substr(pos, count);
            }
        }
    }

    // Modifying methods

    // Make a exlusive writable string if it not already is
    void ensure_exclusive() { (void) exclusive_data(); }

    constexpr void swap(basic_cow_string& other) noexcept(always_equal_alloc || POCS) {
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
        } else if (this->get_allocator() == other.get_allocator() || !other.status().allocated) {
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
        } else if (this->get_allocator() == other.get_allocator() || !other.status().allocated) {
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

    constexpr basic_cow_string& assign(details::literal_helper<char_type> literal) noexcept {
        deallocate_if_needed();
        storage.sta = { .data = literal.data, .size = literal.size };
        return *this;
    }

    constexpr basic_cow_string& operator=(details::literal_helper<char_type> literal) noexcept {
        return assign(literal);
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<std::decay_t<StringViewLike>, const char_type*>)
    constexpr basic_cow_string& assign(const StringViewLike& sv) {
        const string_view_type view = sv;
        if consteval {
            write_static_string(view);
        } else {
            const auto* const data = view.data();
            const size_type size = view.size();
            resize_and_overwrite(view.size(), 
                [data, size](char_type* p, size_type count) constexpr noexcept {
                std::ranges::copy_n(data, size, p);
                return count;
            });
        }
        return *this;
    }

    template<std::convertible_to<string_view_type> StringViewLike>
        requires (!std::convertible_to<std::decay_t<StringViewLike>, const char_type*>)
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
            write_static_string(std::forward<Iter>(iter), std::forward<Sent>(sent));
        } else {
            clear();
            append(std::forward<Iter>(iter), std::forward<Sent>(sent));
        }
        return *this;
    }

    constexpr basic_cow_string& assign(size_type count, char_type ch) {
        if consteval {
            write_static_string(std::views::repeat(ch, count));
        } else {
            clear();
            append_range(std::views::repeat(ch, count));
        }
        return *this;
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
        auto rg = std::forward<R>(r);
        return assign(std::ranges::begin(rg), std::ranges::end(rg));
    }

    pointer data() { return exclusive_data(); }
    iterator begin() { return data(); }
    iterator end() { return data() + size(); }
    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    reference front() pre (size() > 0) { return exclusive_data()[0]; }
    reference back() pre (size() > 0) { return exclusive_data()[size() - 1]; }
    reference operator[](size_type pos) pre (pos < size()) { return exclusive_data()[pos]; }
    
    reference at(size_type pos) {
        if (pos < size()) {
            return exclusive_data()[pos];
        }
        throw std::out_of_range("basic_cow_string::at: position out of range");
    }

    constexpr void reserve(const size_type new_cap) {
        if !consteval {
            reserve_or_exclusive_shrink(std::ranges::max(new_cap, capacity()));
        }
    }

    template<std::invocable<char_type*, size_type> Operation>
    void resize_and_overwrite(size_type count, Operation op) {
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

    constexpr void resize(const size_type new_size, const char_type ch = char_type()) {
        if consteval {
            string_type temp = string();
            temp.resize(new_size, ch);
            write_static_string(temp);
        } else {
            const size_type old_size = size();
            resize_and_overwrite(new_size,
                [ch, old_size](char_type* p, size_type count) noexcept {
                if (count > old_size) {
                    std::ranges::fill(p + old_size, p + count, ch);
                }
                return count;
            });
        }
    }

    constexpr void clear() noexcept {
        if consteval {
            contract_assert(!status().allocated);
            storage.sso = {};
        } else {
            resize(0);
        }
    }

    constexpr void shrink_to_fit() {
        if (status().allocated) {
            if (size() <= storage.sso.capacity) {
                // move to sso
                write_sso(view());
                deallocate_if_needed();
            } else if (!exclusive() || size() < capacity()) {
                // reallocate to smaller buffer
                allocate_exclusive_buffer(view());
            }
        }
    }

private:
    storage_type storage = {};
};

using cow_string = basic_cow_string<char>;
using cow_wstring = basic_cow_string<wchar_t>;
using cow_u8string = basic_cow_string<char8_t>;
using cow_u16string = basic_cow_string<char16_t>;
using cow_u32string = basic_cow_string<char32_t>;

} // namespace cow

#endif // MYLIB_COW_STRING2_H
