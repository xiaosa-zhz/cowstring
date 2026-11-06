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

consteval bool is_valid_char_type(std::meta::info type) {
    static constexpr std::meta::info std_char_types[] = {
        ^^char, ^^wchar_t, ^^char8_t, ^^char16_t, ^^char32_t
    };
    for (auto t : std_char_types) {
        if (is_same_type(type, t)) return true;
    }
    return false;
}

template<typename CharT, std::size_t Size>
struct sso_string {
    CharT sso_data[Size] = {};
    unsigned char _[sizeof(CharT) - 1] = {};
    unsigned char size : (CHAR_BIT - 2) = 0;
    unsigned char shared : 1 = 0;
    unsigned char allocated : 1 = 0;
};

template<typename CharT, std::size_t Size>
    requires (sizeof(CharT) == 1)
struct sso_string<CharT, Size> {
    CharT sso_data[Size] = {};
    unsigned char size : (CHAR_BIT - 2) = 0;
    unsigned char shared : 1 = 0;
    unsigned char allocated : 1 = 0;
};

struct status { bool shared; bool allocated; };

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
template<typename AllocatorTraits>
struct cow_string_allocator_propagate_traits {
    static constexpr bool is_always_equal = AllocatorTraits::is_always_equal::value;
    static constexpr bool simple_copy = is_always_equal
        || AllocatorTraits::propagate_on_container_copy_assignment::value;
    static constexpr bool simple_move = is_always_equal
        || AllocatorTraits::propagate_on_container_move_assignment::value;
    static constexpr bool simple_swap = is_always_equal
        || AllocatorTraits::propagate_on_container_swap::value;
};

struct alignas(std::max_align_t) cow_string_control_block
{
    template<typename CharT>
    CharT* data() noexcept {
        return std::launder(static_cast<CharT*>(static_cast<void*>(this + 1)));
    }

    const std::size_t block_count = 0;
    std::atomic_size_t reference_counter = 0;
};

// shall never allocate through the allocator during constant evaluation
struct allocation_result {
    cow_string_control_block* block = nullptr;
    std::size_t capacity = 0;
};

template<typename CharT, typename Alloc>
allocation_result allocate(const Alloc& source, std::size_t capacity) {
    using source_traits = std::allocator_traits<Alloc>;
    using allocator = typename source_traits::template rebind_alloc<cow_string_control_block>;
    using traits = std::allocator_traits<allocator>;
    allocator alloc(source);
    const std::size_t total_size = sizeof(CharT) * (capacity + 1) / sizeof(cow_string_control_block) + 2;
    cow_string_control_block* block = traits::allocate(alloc, total_size); // may throw here
    traits::construct(alloc, block, total_size, 1);
    const std::size_t actual_capacity = (total_size - 1) * sizeof(cow_string_control_block) / sizeof(CharT) - 1;
    return { block, actual_capacity };
}

template<typename Alloc>
void deallocate(const Alloc& source, cow_string_control_block* block) noexcept {
    using source_traits = std::allocator_traits<Alloc>;
    using allocator = typename source_traits::template rebind_alloc<cow_string_control_block>;
    using traits = std::allocator_traits<allocator>;
    allocator alloc(source);
    const std::size_t total_size = block->block_count;
    traits::destroy(alloc, block);
    traits::deallocate(alloc, block, total_size);
}

} // namespace cow::details

template<typename CharT, typename Allocator = std::allocator<CharT>>
class basic_cow_string
{
public:
    using value_type = CharT;
    using char_type = CharT;
    using allocator_type = Allocator;
    using allocator_traits_type = std::allocator_traits<allocator_type>;
    using size_type = allocator_traits_type::size_type;
    using difference_type = allocator_traits_type::difference_type;
    using const_reference = const value_type&;
    using reference = const_reference;
    using const_pointer = allocator_traits_type::const_pointer;
    using pointer = const_pointer;
    using const_iterator = const value_type*;
    using iterator = const_iterator;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using reverse_iterator = std::reverse_iterator<iterator>;

    using string_view_type = std::basic_string_view<value_type>;
    using string_type = std::basic_string<value_type>;

private:
    static_assert(std::is_same_v<value_type, typename allocator_traits_type::value_type>);
    static_assert(details::is_valid_char_type(^^CharT));
    static_assert(std::is_pointer_v<pointer> && std::is_pointer_v<const_pointer>
        && std::is_same_v<std::remove_const_t<std::remove_pointer_t<const_pointer>>, char_type>);

    struct non_sso_string {
        pointer data;
        size_type size;
        size_type capacity : (sizeof(size_type) * CHAR_BIT - 2);
        mutable size_type shared : 1;
        size_type allocated : 1;
    };

    static constexpr size_type bufsz = (sizeof(non_sso_string) - 1) / sizeof(value_type) > 2
        ? (sizeof(non_sso_string) - 1) / sizeof(value_type) : 2;

    using sso_string = details::sso_string<CharT, bufsz>;
    static constexpr size_type sso_capacity = bufsz - 1;

    static_assert(sizeof(sso_string) == sizeof(non_sso_string));

    union storage_type {
        sso_string sso = {};
        non_sso_string non_sso;
    };

    static_assert(sizeof(storage_type) == sizeof(non_sso_string));

    using propagate_traits = details::cow_string_allocator_propagate_traits<allocator_traits_type>;
    static constexpr bool is_always_equal = propagate_traits::is_always_equal;
    static constexpr bool simple_copy = propagate_traits::simple_copy;
    static constexpr bool simple_move = propagate_traits::simple_move;
    static constexpr bool simple_swap = propagate_traits::simple_swap;

public:
    constexpr basic_cow_string() noexcept = default;
    constexpr basic_cow_string(const basic_cow_string&) noexcept; // TODO
    constexpr basic_cow_string(basic_cow_string&&) noexcept; // TODO
    constexpr basic_cow_string& operator=(const basic_cow_string&) noexcept; // TODO
    constexpr basic_cow_string& operator=(basic_cow_string&&) noexcept; // TODO

    class modification_string
    {
    public:
        modification_string() = delete;
        modification_string(const basic_cow_string& str) = delete;
        modification_string(basic_cow_string&& str) = delete;
        modification_string& operator=(const modification_string&) = delete;
        modification_string& operator=(modification_string&&) = delete;

        constexpr ~modification_string() {
            // TODO: write back
            if consteval {
                delete consteval_new_string;
            } else {
                // TODO
            }
        }

    private:
        friend class basic_cow_string;
        constexpr explicit modification_string(basic_cow_string* str) noexcept : str(str) {
            if consteval {
                consteval_new_string = new std::string(*this->str);
            } else {
                // TODO: allocate new control block
            }
        }

        basic_cow_string* str;
        union {
            details::cow_string_control_block* new_cb;
            std::string* consteval_new_string;
        };
    };

private:
    constexpr details::status status() const noexcept {
        if consteval {
            if (std::is_within_lifetime(std::addressof(storage.sso))) {
                return { false, false };
            } else {
                return { storage.non_sso.shared, storage.non_sso.allocated };
            }
        } else {
            const auto probe = std::bit_cast<sso_string>(storage);
            return { probe.shared, probe.allocated };
        }
    }

    constexpr bool is_shared() const noexcept { return status().shared; }
    constexpr bool is_allocated() const noexcept { return status().allocated; }

    storage_type storage = {};
};

using cow_string = basic_cow_string<char>;
using cow_wstring = basic_cow_string<wchar_t>;
using cow_u8string = basic_cow_string<char8_t>;
using cow_u16string = basic_cow_string<char16_t>;
using cow_u32string = basic_cow_string<char32_t>;

} // namespace cow

#endif // MYLIB_COW_STRING2_H
