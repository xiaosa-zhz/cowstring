#pragma once
#ifndef MYLIB_COW_STRING_H
#define MYLIB_COW_STRING_H 1

#include <concepts>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <iterator>
#include <string>
#include <string_view>
#include <memory>
#include <format>
#include <atomic>
#include <new>
#include <algorithm>
#include <initializer_list>
#include <ranges>

namespace mylib {

    // Forward declaration
    template<typename CharT, typename Allocator>
    class basic_cow_string;

    namespace details {

        // cow string has 4 states, distinguished by low 2 bits of control
        // 1. small string: control bits are 00 and higher bits of control is size
        // 2. literal string: control bits are 01
        // 3. owned string: control bits are 10 and higher bits of control is pointer to control block
        // 4. shared string: control bits are 11 and higher bits of control is pointer to control block
        template<typename CharT>
        struct storage_block
        {
            using char_type = CharT;

            struct non_sso_string {
                std::uint64_t size;
                std::uint64_t capacity;
                const char_type* data;
            };

            static constexpr std::size_t sso_capacity = sizeof(non_sso_string) / sizeof(char_type) - 1;
            static constexpr std::uintptr_t control_mask = 0b11;
            static constexpr std::uintptr_t shared_mask = 0b01;
            static constexpr std::uintptr_t allocated_mask = 0b10;

            union sso_block {
                char_type small[sso_capacity + 1] = {};
                non_sso_string non_sso;
            };

            sso_block storage = {};
            mutable std::uintptr_t control = 0;
        };

        struct alignas(std::max_align_t) cow_string_control_block
        {
            template<typename CharT>
            constexpr CharT* data() noexcept {
                return std::launder(static_cast<CharT*>(static_cast<void*>(this + 1)));
            }

            const std::size_t block_count = 0;
            std::atomic_size_t reference_counter = 0;
        };

        struct allocation_result {
            cow_string_control_block* block = nullptr;
            std::size_t capacity = 0;
        };

        template<typename CharT, typename Alloc>
        constexpr allocation_result allocate(const Alloc& source, std::size_t capacity) {
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
        constexpr void deallocate(const Alloc& source, cow_string_control_block* block) noexcept {
            using source_traits = std::allocator_traits<Alloc>;
            using allocator = typename source_traits::template rebind_alloc<cow_string_control_block>;
            using traits = std::allocator_traits<allocator>;
            allocator alloc(source);
            const std::size_t total_size = block->block_count;
            traits::destroy(alloc, block);
            traits::deallocate(alloc, block, total_size);
        }

        static_assert(alignof(cow_string_control_block) >= 4);

        template<typename Iter, typename Sent, typename CharT>
        concept eligible_iter_and_sent = std::input_iterator<std::decay_t<Iter>>
            && std::sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>
            && std::convertible_to<std::iter_reference_t<std::decay_t<Iter>>, CharT>;

        template<typename R, typename CharT>
        concept container_compatible_range = std::ranges::input_range<R>
            && std::convertible_to<std::ranges::range_reference_t<R>, CharT>;

        template<typename CharT, std::size_t N>
        struct cow_string_literal_helper {
            using char_type = CharT;
            using cow_string_type = basic_cow_string<CharT, std::allocator<CharT>>;

            consteval cow_string_literal_helper(const CharT(&str)[N]) noexcept {
                std::ranges::copy_n(str, N, literal);
            }

            CharT literal[N];
        };

    } // namespace mylib::details

    template<typename CharT, typename Allocator = std::allocator<CharT>>
    class basic_cow_string : private Allocator, private details::storage_block<CharT>
    {
    public:
        using value_type = CharT;
        using char_type = CharT;
        using allocator_type = std::allocator_traits<Allocator>::template rebind_alloc<char_type>;
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

        static constexpr size_type npos = static_cast<size_type>(-1);

        constexpr explicit basic_cow_string(const allocator_type& alloc) noexcept
            : allocator_type(alloc)
        {}

        constexpr basic_cow_string() noexcept(std::is_nothrow_default_constructible_v<allocator_type>)
            : basic_cow_string(allocator_type())
        {}

        constexpr basic_cow_string(size_type count, const char_type ch,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(alloc) {
            append(count, ch);
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string(Iter&& iter, Sent&& sent,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(alloc) {
            append(std::forward<Iter>(iter), std::forward<Sent>(sent));
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string(std::from_range_t, R&& r,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(alloc) {
            append_range(std::forward<R>(r));
        }

        constexpr basic_cow_string(const char_type* s, size_type count,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(string_view_type(s, count), alloc)
        {}

        constexpr basic_cow_string(std::nullptr_t) = delete;

        constexpr basic_cow_string(const basic_cow_string& other) noexcept
            : basic_cow_string(other.get_allocator()) {
            if (other.allocated()) {
                // owned or shared string
                // share with source
                other.control |= 0b01; // mark as shared
                this->control = other.control;
                this->storage.non_sso = other.storage.non_sso;
                control_block()->reference_counter.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (other.shared()) {
                    // literal string
                    this->storage.non_sso = other.storage.non_sso;
                    this->control = other.control;
                } else {
                    // small string
                    const std::size_t size = other.control >> 2;
                    std::ranges::copy_n(other.storage.small, size + 1, this->storage.small);
                    this->control = other.control;
                }
            }
        }

        constexpr basic_cow_string(basic_cow_string&& other) noexcept
            : basic_cow_string(other.get_allocator()) {
            if (other.allocated()) {
                // owned or shared string
                // set source to sso empty string
                this->control = other.control;
                this->storage.non_sso = other.storage.non_sso;
                other.control = 0;
                other.storage.small[0] = char_type();
            } else {
                if (other.shared()) {
                    // literal string
                    this->storage.non_sso = other.storage.non_sso;
                    this->control = other.control;
                } else {
                    // small string
                    const std::size_t size = other.control >> 2;
                    std::ranges::copy_n(other.storage.small, size + 1, this->storage.small);
                    this->control = other.control;
                }
            }
        }

        constexpr basic_cow_string(const basic_cow_string& other,
            const size_type pos, const size_type count) noexcept
            : basic_cow_string(other.substr(pos, count))
        {}

        constexpr basic_cow_string(const basic_cow_string& other,
            const size_type pos) noexcept
            : basic_cow_string(other, pos, npos)
        {}

        constexpr basic_cow_string(basic_cow_string&& other,
            const size_type pos, const size_type count) noexcept
            : basic_cow_string(std::move(other).substr(pos, count))
        {}

        constexpr basic_cow_string(basic_cow_string&& other,
            const size_type pos) noexcept
            : basic_cow_string(std::move(other), pos, npos)
        {}

        constexpr basic_cow_string(std::initializer_list<char_type> il,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(il.begin(), il.end(), alloc)
        {}

        // From literal
        // May assign literal to cow_string via conversion
        template<std::size_t N>
        consteval basic_cow_string(const char_type(&literal)[N]) noexcept {
            this->storage.non_sso.size = N - 1;
            this->storage.non_sso.capacity = 0; // 0 capacity indicates always need allocation on write
            this->storage.non_sso.data = literal;
            this->control = 0b01;
        }

        template<std::convertible_to<string_view_type> StringViewLike>
            requires (!std::is_array_v<std::remove_cvref_t<StringViewLike>>)
        constexpr basic_cow_string(const StringViewLike& sv, const allocator_type& alloc = allocator_type()) 
            : basic_cow_string(alloc) {
            const string_view_type view = sv;
            const std::size_t size = view.size();
            if (size > this->sso_capacity) {
                allocate_exclusive_buffer(view);
            } else {
                std::ranges::copy_n(view.data(), size, this->storage.small);
                this->storage.small[size] = char_type();
                this->control = size << 2;
            }
        }

        template<std::convertible_to<string_view_type> StringViewLike>
            requires (!std::is_array_v<std::remove_cvref_t<StringViewLike>>)
        constexpr basic_cow_string(const StringViewLike& sv,
            const size_type pos, const size_type count,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(string_view_type(sv).substr(pos, count), alloc)
        {}

        template<std::convertible_to<string_view_type> StringViewLike>
            requires (!std::is_array_v<std::remove_cvref_t<StringViewLike>>)
        constexpr basic_cow_string(const StringViewLike& sv,
            const size_type pos,
            const allocator_type& alloc = allocator_type())
            : basic_cow_string(sv, pos, npos, alloc)
        {}

        constexpr ~basic_cow_string() {
            deallocate_if_needed();
        }

#ifdef MYLIB_COW_STRING_TEST_STATUS

        // For test only
        constexpr bool is_sso() const noexcept {
            return !shared() && !allocated();
        }

        // For test only
        constexpr bool is_literal() const noexcept {
            return shared() && !allocated();
        }

        // For test only
        constexpr bool is_owned() const noexcept {
            return !shared() && allocated();
        }

        // For test only
        constexpr bool is_shared() const noexcept {
            return shared() && allocated();
        }

        // For test only
        constexpr std::size_t reference_counter() const noexcept {
            if (allocated()) {
                return control_block()->reference_counter.load(std::memory_order_relaxed);
            } else {
                return 0;
            }
        }

#endif // MYLIB_COW_STRING_TEST_STATUS
        
        // Non-modifying methods

        constexpr size_type size() const noexcept {
            if (!allocated() && !shared()) {
                return this->control >> 2;
            } else {
                return this->storage.non_sso.size;
            }
        }

        constexpr size_type capacity() const noexcept {
            if (!allocated()) {
                return this->sso_capacity;
            } else {
                return this->storage.non_sso.capacity;
            }
        }

        constexpr size_type length() const noexcept { return size(); }
        constexpr bool empty() const noexcept { return size() == 0; }

        constexpr bool unique() const noexcept {
            if (!shared() || !allocated()) {
                return true;
            } else {
                if (control_block()->reference_counter.load(std::memory_order_acquire) == 1) {
                    this->control &= ~this->shared_mask;
                    return true;
                } else {
                    return false;
                }
            }
        }

        constexpr string_view_type view() const noexcept {
            if (!allocated() && !shared()) {
                // small string
                const std::size_t size = this->control >> 2;
                return string_view_type{ this->storage.small, size };
            } else {
                // literal, owned or shared string
                const std::size_t size = this->storage.non_sso.size;
                return string_view_type{ this->storage.non_sso.data, size };
            }
        }

        constexpr string_type string() const { return string_type(view()); }
        constexpr operator string_view_type() const noexcept { return view(); }

        constexpr const allocator_type& get_allocator() const noexcept {
            return static_cast<const allocator_type&>(*this);
        }

        constexpr basic_cow_string substr(
            const size_type pos = 0, const size_type count = npos) const & noexcept {
            return auto(*this).substr(pos, count);
        }

        constexpr basic_cow_string substr(
            const size_type pos = 0, const size_type count = npos) && noexcept {
            basic_cow_string copy = std::move(*this);
            const size_type original_size = copy.size();
            const size_type size = std::ranges::min(count, original_size - pos);
            [[assume(pos <= original_size)]];
            if (copy.allocated() || copy.shared()) {
                // literal or shared string
                copy.storage.non_sso.size = size;
                copy.storage.non_sso.data += pos;
            } else {
                // small string
                if (pos > 0) {
                    std::ranges::copy_n(copy.storage.small + pos, size,
                        copy.storage.small);
                }
                copy.storage.small[size] = char_type();
                copy.control = size << 2;
            }
            return copy;
        }

        constexpr string_view_type subview(
            const size_type pos = 0, const size_type count = npos) const noexcept {
            return view().substr(pos, count);
        }

        constexpr const char_type* data() const noexcept { return data_unsafe(); }
        constexpr const char_type* c_str() const noexcept { return data_unsafe(); }
        constexpr const_iterator begin() const noexcept { return data_unsafe(); }
        constexpr const_iterator end() const noexcept { return data_unsafe() + size(); }
        constexpr const_iterator cbegin() const noexcept { return begin(); }
        constexpr const_iterator cend() const noexcept { return end(); }
        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
        constexpr const_reverse_iterator crend() const noexcept { return rend(); }        

        constexpr const char_type& front() const noexcept {
            [[assume(size() > 0)]];
            return data_unsafe()[0];
        }

        constexpr const char_type& back() const noexcept {
            const size_type sz = size();
            [[assume(sz > 0)]];
            return data_unsafe()[sz - 1];
        }

        constexpr const char_type& operator[](const size_type pos) const noexcept {
            const size_type sz = size();
            [[assume(pos < sz)]];
            return data_unsafe()[pos];
        }

        constexpr const char_type& at(const size_type pos) const {
            if (pos >= size()) {
                throw std::out_of_range("basic_cow_string::at: position out of range");
            }
            return data_unsafe()[pos];
        }

        constexpr size_type find(const string_view_type str, const size_type pos = 0) const noexcept {
            return view().find(str, pos);
        }

        constexpr size_type find(const char_type ch, const size_type pos = 0) const noexcept {
            return view().find(ch, pos);
        }

        constexpr size_type rfind(const string_view_type str, const size_type pos = npos) const noexcept {
            return view().rfind(str, pos);
        }

        constexpr size_type rfind(const char_type ch, const size_type pos = npos) const noexcept {
            return view().rfind(ch, pos);
        }

        constexpr size_type find_first_of(const string_view_type str, const size_type pos = 0) const noexcept {
            return view().find_first_of(str, pos);
        }

        constexpr size_type find_first_of(const char_type ch, const size_type pos = 0) const noexcept {
            return view().find_first_of(ch, pos);
        }

        constexpr size_type find_first_not_of(const string_view_type str, const size_type pos = 0) const noexcept {
            return view().find_first_not_of(str, pos);
        }

        constexpr size_type find_first_not_of(const char_type ch, const size_type pos = 0) const noexcept {
            return view().find_first_not_of(ch, pos);
        }

        constexpr size_type find_last_of(const string_view_type str, const size_type pos = npos) const noexcept {
            return view().find_last_of(str, pos);
        }

        constexpr size_type find_last_of(const char_type ch, const size_type pos = npos) const noexcept {
            return view().find_last_of(ch, pos);
        }

        constexpr size_type find_last_not_of(const string_view_type str, const size_type pos = npos) const noexcept {
            return view().find_last_not_of(str, pos);
        }

        constexpr size_type find_last_not_of(const char_type ch, const size_type pos = npos) const noexcept {
            return view().find_last_not_of(ch, pos);
        }

        constexpr bool starts_with(const string_view_type str) const noexcept {
            return view().starts_with(str);
        }

        constexpr bool starts_with(const char_type ch) const noexcept {
            return view().starts_with(ch);
        }

        constexpr bool ends_with(const string_view_type str) const noexcept {
            return view().ends_with(str);
        }

        constexpr bool ends_with(const char_type ch) const noexcept {
            return view().ends_with(ch);
        }

        constexpr bool contains(const string_view_type str) const noexcept {
            return view().contains(str);
        }

        constexpr bool contains(const char_type ch) const noexcept {
            return view().contains(ch);
        }

        // Modifying methods

        constexpr void reserve(const size_type new_cap) {
            reserve_or_exclusive_shrink(std::ranges::max(new_cap, capacity()));
        }

        template<std::invocable<char_type*, size_type> Operation>
        constexpr void resize_and_overwrite(size_type count, Operation op) {
            // get necessary storage
            char_type* const p = reserve_or_exclusive_shrink(count);
            // now exclusive
            size_type r = std::move(op)(p, count);
            [[assume(r <= count)]];
            if (allocated()) {
                this->storage.non_sso.size = r;
            } else {
                this->control = (r << 2);
            }
            p[r] = char_type();
        }

        constexpr void resize(const size_type new_size, const char_type ch = char_type()) {
            const size_type old_size = size();
            resize_and_overwrite(new_size,
                [ch, old_size](char_type* p, size_type count) constexpr noexcept {
                if (count > old_size) {
                    std::ranges::fill(p + old_size, p + count, ch);
                }
                return count;
            });
        }

        constexpr void clear() noexcept { resize(0); }

        constexpr void shrink_to_fit() {
            if (allocated()) {
                if (size() <= this->sso_capacity) {
                    // move to sso
                    write_sso_buffer(view());
                    deallocate_if_needed();
                } else if (!unique() || size() < capacity()) {
                    // reallocate to smaller buffer
                    allocate_exclusive_buffer(view());
                }
            }
        }

        constexpr void swap(basic_cow_string& other) noexcept {
            std::ranges::swap((allocator_type&)*this, (allocator_type&)other);
            using storage = details::storage_block<char_type>;
            std::ranges::swap((storage&)(*this), (storage&)other);
        }

        constexpr basic_cow_string& assign(std::nullptr_t) = delete;
        constexpr basic_cow_string& operator=(std::nullptr_t) = delete;

        constexpr basic_cow_string& assign(const basic_cow_string& other) noexcept {
            basic_cow_string(other).swap(*this);
            return *this;
        }

        constexpr basic_cow_string& operator=(const basic_cow_string& other) noexcept {
            return assign(other);
        }

        constexpr basic_cow_string& assign(basic_cow_string&& other) noexcept {
            basic_cow_string(std::move(other)).swap(*this);
            return *this;
        }

        constexpr basic_cow_string& operator=(basic_cow_string&& other) noexcept {
            return assign(std::move(other));
        }

        template<std::convertible_to<string_view_type> StringViewLike>
            requires (!std::is_array_v<std::remove_cvref_t<StringViewLike>>)
        constexpr basic_cow_string& assign(const StringViewLike& sv) {
            const string_view_type view = sv;
            const auto* const data = view.data();
            const size_type size = view.size();
            resize_and_overwrite(view.size(), 
                [data, size](char_type* p, size_type count) constexpr noexcept {
                std::ranges::copy_n(data, size, p);
                return count;
            });
            return *this;
        }

        template<std::convertible_to<string_view_type> StringViewLike>
            requires (!std::is_array_v<std::remove_cvref_t<StringViewLike>>)
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
            return assign(il.begin(), il.end());
        }

        constexpr basic_cow_string& operator=(std::initializer_list<char_type> il) {
            return assign(il);
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string& assign(Iter&& iter, Sent&& sent) {
            if constexpr (!std::sized_sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>) {
                basic_cow_string(std::forward<Iter>(iter), std::forward<Sent>(sent), get_allocator())
                    .swap(*this);
            } else {
                clear();
                append(std::forward<Iter>(iter), std::forward<Sent>(sent));
            }
        }

        constexpr basic_cow_string& assign(size_type count, const char_type ch) {
            resize(count, ch);
            return *this;
        }

        constexpr basic_cow_string& assign(const char_type* s, size_type count) {
            return assign(string_view_type(s, count));
        }

        constexpr basic_cow_string& assign(const basic_cow_string& str,
            const size_type pos, const size_type count = npos) {
            basic_cow_string(str, pos, count).swap(*this);
            return *this;
        }

        constexpr basic_cow_string& assign(basic_cow_string&& str,
            const size_type pos, const size_type count = npos) {
            basic_cow_string(std::move(str), pos, count).swap(*this);
            return *this;
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string& assign_range(R&& r) {
            auto rg = std::forward<R>(r);
            return assign(std::ranges::begin(rg), std::ranges::end(rg));
        }

        constexpr void push_back(const char_type ch) {
            const size_type old_size = size();
            resize_and_overwrite(old_size + 1,
                [ch, old_size](char_type* p, size_type count) constexpr noexcept {
                p[old_size] = ch;
                return count;
            });
        }

        constexpr void pop_back() {
            const size_type old_size = size();
            [[assume(old_size > 0)]];
            resize(old_size - 1);
        }

        constexpr basic_cow_string& append(const size_type count, const char_type ch) {
            resize(size() + count, ch);
            return *this;
        }

        constexpr basic_cow_string& append(const string_view_type sv,
            const size_type pos, const size_type count = npos) {
            const string_view_type view = string_view_type(sv).substr(pos, count);
            const size_type old_size = size();
            const size_type add_size = view.size();
            resize_and_overwrite(old_size + add_size,
                [old_size, add_size, view](char_type* p, size_type count) constexpr noexcept {
                std::ranges::copy_n(view.data(), add_size, p + old_size);
                return count;
            });
            return *this;
        }

        constexpr basic_cow_string& append(const string_view_type sv) {
            return append(sv, 0, npos);
        }

        constexpr basic_cow_string& append(const char_type* s, const size_type count) {
            return append(string_view_type(s, count));
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string& append(Iter&& iter, Sent&& sent) {
            const size_type old_size = size();
            auto i = std::forward<Iter>(iter);
            [[maybe_unused]]
            auto s = std::forward<Sent>(sent);
            if constexpr (!std::sized_sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>) {
                // push_back fallback
                try {
                    for (; i != s; ++i) {
                        push_back(*i);
                    }
                } catch (...) {
                    resize(old_size);
                    throw;
                }
            } else {
                const size_type add_size = std::ranges::distance(iter, sent);
                resize_and_overwrite(old_size + add_size,
                    [old_size, add_size, &i](char_type* p, size_type count) constexpr noexcept {
                    std::ranges::copy_n(i, add_size, p + old_size);
                    return count;
                });
            }
            return *this;
        }

        constexpr basic_cow_string& append(std::initializer_list<char_type> il) {
            return append(il.begin(), il.end());
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string& append_range(R&& r) {
            auto rg = std::forward<R>(r);
            // TODO: add approximately_sized_range branch
            return append(std::ranges::begin(rg), std::ranges::end(rg));
        }

        template<std::convertible_to<string_view_type> StringViewLike>
        constexpr basic_cow_string& operator+=(const StringViewLike& sv) {
            return append(string_view_type(sv));
        }

        constexpr basic_cow_string& operator+=(const char_type ch) {
            push_back(ch);
            return *this;
        }

        constexpr basic_cow_string& operator+=(std::initializer_list<char_type> il) {
            return append(il);
        }

        constexpr char_type* data() noexcept { return exclusive(); }
        constexpr iterator begin() noexcept { return data(); }
        constexpr iterator end() noexcept { return data() + size(); }
        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

        constexpr char_type& front() noexcept {
            const size_type sz = size();
            [[assume(sz > 0)]];
            return exclusive()[0];
        }

        constexpr char_type& back() noexcept {
            const size_type sz = size();
            [[assume(sz > 0)]];
            return exclusive()[sz - 1];
        }

        constexpr char_type& operator[](const size_type pos) noexcept {
            const size_type sz = size();
            [[assume(pos < sz)]];
            return exclusive()[pos];
        }

        constexpr char_type& at(const size_type pos) {
            if (pos >= size()) {
                throw std::out_of_range("basic_cow_string::at: position out of range");
            }
            return exclusive()[pos];
        }

        constexpr basic_cow_string& insert(const size_type index,
            const size_type count, const char_type ch) {
            const size_type old_size = size();
            [[assume(index <= old_size)]];
            resize_and_overwrite(old_size + count,
                [index, count, ch, old_size](char_type* p, size_type new_size) constexpr noexcept {
                std::ranges::copy_backward(p + index, p + old_size, p + new_size);
                std::ranges::fill_n(p + index, count, ch);
                return new_size;
            });
            return *this;
        }

        constexpr basic_cow_string& insert(const size_type index,
            const char_type ch) {
            return insert(index, 1, ch);
        }

        constexpr basic_cow_string& insert(const size_type index,
            const string_view_type str) {
            const size_type old_size = size();
            const size_type add_size = str.size();
            [[assume(index <= old_size)]];
            resize_and_overwrite(old_size + add_size,
                [index, add_size, str, old_size](char_type* p, size_type new_size) constexpr noexcept {
                std::ranges::copy_backward(p + index, p + old_size, p + new_size);
                std::ranges::copy_n(str.data(), add_size, p + index, p + new_size);
                return new_size;
            });
            return *this;
        }

        constexpr basic_cow_string& insert(const size_type index,
            const char_type* s, const size_type count) {
            return insert(index, string_view_type(s, count));
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string& insert(const size_type index,
            Iter&& iter, Sent&& sent) {
            const size_type old_size = size();
            [[assume(index <= old_size)]];
            auto i = std::forward<Iter>(iter);
            [[maybe_unused]]
            auto s = std::forward<Sent>(sent);
            if constexpr (!std::sized_sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>) {
                // push_back fallback
                basic_cow_string temp = substr(0, index);
                try {
                    for (; i != s; ++i) {
                        temp.push_back(*i);
                    }
                } catch (...) {
                    throw;
                }
                temp.append(subview(index, npos));
                swap(temp);
            } else {
                const size_type add_size = std::ranges::distance(iter, sent);
                resize_and_overwrite(old_size + add_size,
                    [index, add_size, old_size, &i](char_type* p, size_type new_size) constexpr noexcept {
                    std::ranges::copy_backward(p + index, p + old_size, p + new_size);
                    std::ranges::copy_n(i, add_size, p + index);
                    return new_size;
                });
            }
            return *this;
        }

        constexpr basic_cow_string& insert(const size_type index,
            std::initializer_list<char_type> il) {
            return insert(index, il.begin(), il.end());
        }

        constexpr iterator insert(const_iterator pos, const size_type count, const char_type ch) {
            const size_type index = pos - cbegin();
            insert(index, count, ch);
            return begin() + index;
        }

        constexpr iterator insert(const_iterator pos, const char_type ch) {
            return insert(pos, 1, ch);
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr iterator insert(const_iterator pos, Iter&& iter, Sent&& sent) {
            const size_type index = pos - cbegin();
            insert(index, std::forward<Iter>(iter), std::forward<Sent>(sent));
            return begin() + index;
        }

        constexpr iterator insert(const_iterator pos, std::initializer_list<char_type> il) {
            return insert(pos, il.begin(), il.end());
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string& insert_range(const size_type index, R&& r) {
            auto rg = std::forward<R>(r);
            return insert(index, std::ranges::begin(rg), std::ranges::end(rg));
        }

        constexpr basic_cow_string& erase(const size_type index = 0, const size_type count = npos) {
            const size_type old_size = size();
            [[assume(index <= old_size)]];
            if (old_size - index <= count) {
                resize(index);
                return *this;
            }
            char_type* const data = exclusive();
            std::ranges::copy(data + index + count, data + old_size, data + index);
            resize(old_size - count);
            return *this;
        }

        constexpr iterator erase(const_iterator pos) {
            const size_type index = pos - cbegin();
            erase(index, 1);
            return begin() + index;
        }

        constexpr iterator erase(const_iterator first, const_iterator last) {
            const size_type index = first - cbegin();
            const size_type count = last - first;
            erase(index, count);
            return begin() + index;
        }

        constexpr basic_cow_string& replace(const size_type pos, const size_type count,
            const string_view_type str) {
            const size_type old_size = size();
            [[assume(pos <= old_size)]];
            if (old_size - pos <= count) {
                resize(pos);
                return append(str);
            }
            const size_type rcount = std::ranges::min(count, old_size - pos);
            const size_type add_size = str.size();
            const size_type new_size = old_size - rcount + add_size;
            if (new_size > old_size) {
                resize(new_size);
                char_type* const data = data_unsafe();
                std::ranges::copy_backward(data + pos + rcount, data + old_size, data + new_size);
                std::ranges::copy_n(str.data(), add_size, data + pos);
            } else {
                char_type* const data = exclusive();
                std::ranges::copy_n(str.data(), add_size, data + pos);
                std::ranges::copy(data + pos + rcount, data + old_size, data + pos + add_size);
                resize(new_size);
            }
            return *this;
        }

        constexpr basic_cow_string& replace(const_iterator first, const_iterator last,
            const string_view_type str) {
            const size_type index = first - cbegin();
            const size_type count = last - first;
            return replace(index, count, str);
        }

        constexpr basic_cow_string& replace(const size_type pos, const size_type count,
            const char_type* cstr, const size_type count2) {
            return replace(pos, count, string_view_type(cstr, count2));
        }

        constexpr basic_cow_string& replace(const_iterator first, const_iterator last,
            const char_type* cstr, size_type count2) {
            return replace(first, last, string_view_type(cstr, count2));
        }

        constexpr basic_cow_string& replace(const size_type pos, const size_type count,
            const size_type count2, const char_type ch) {
            const size_type old_size = size();
            [[assume(pos <= old_size)]];
            if (old_size - pos <= count) {
                resize(pos);
                return append(count2, ch);
            }
            const size_type rcount = std::ranges::min(count, old_size - pos);
            const size_type add_size = count2;
            const size_type new_size = old_size - rcount + add_size;
            if (new_size > old_size) {
                resize(new_size);
                char_type* const data = data_unsafe();
                std::ranges::copy_backward(data + pos + rcount, data + old_size, data + new_size);
                std::ranges::fill_n(data + pos, add_size, ch);
            } else {
                char_type* const data = exclusive();
                std::ranges::fill_n(data + pos, add_size, ch);
                std::ranges::copy(data + pos + rcount, data + old_size, data + pos + add_size);
                resize(new_size);
            }
            return *this;
        }
        
        constexpr basic_cow_string& replace(const_iterator first, const_iterator last,
            const size_type count2, const char_type ch) {
            const size_type index = first - cbegin();
            const size_type count = last - first;
            return replace(index, count, count2, ch);
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string& replace(const size_type pos, const size_type count,
            Iter&& iter, Sent&& sent) {
            const size_type old_size = size();
            [[assume(pos <= old_size)]];
            auto i = std::forward<Iter>(iter);
            [[maybe_unused]]
            auto s = std::forward<Sent>(sent);
            if constexpr (!std::sized_sentinel_for<std::decay_t<Sent>, std::decay_t<Iter>>) {
                // push_back fallback
                basic_cow_string temp = substr(0, pos);
                try {
                    for (; i != s; ++i) {
                        temp.push_back(*i);
                    }
                } catch (...) {
                    throw;
                }
                if (old_size - pos > count) {
                    temp.append(subview(pos + count, npos));
                }
                swap(temp);
            } else {
                if (old_size - pos <= count) {
                    resize(pos);
                    return append(std::forward<Iter>(i), std::forward<Sent>(s));
                }
                const size_type rcount = std::ranges::min(count, old_size - pos);
                const size_type add_size = std::ranges::distance(i, s);
                const size_type new_size = old_size - rcount + add_size;
                if (new_size > old_size) {
                    resize(new_size);
                    char_type* const data = data_unsafe();
                    std::ranges::copy_backward(data + pos + rcount, data + old_size, data + new_size);
                    std::ranges::move(i, s, data + pos);
                } else {
                    char_type* const data = exclusive();
                    std::ranges::move(i, s, data + pos);
                    std::ranges::copy(data + pos + rcount, data + old_size, data + pos + add_size);
                    resize(new_size);
                }
            }
            return *this;
        }

        template<typename Iter, typename Sent>
            requires details::eligible_iter_and_sent<Iter, Sent, char_type>
        constexpr basic_cow_string& replace(const_iterator first, const_iterator last,
            Iter&& iter, Sent&& sent) {
            const size_type index = first - cbegin();
            const size_type count = last - first;
            return replace(index, count, std::forward<Iter>(iter), std::forward<Sent>(sent));
        }

        constexpr basic_cow_string& replace(const size_type pos, const size_type count,
            std::initializer_list<char_type> il) {
            return replace(pos, count, il.begin(), il.end());
        }

        constexpr basic_cow_string& replace(const_iterator first, const_iterator last,
            std::initializer_list<char_type> il) {
            return replace(first, last, il.begin(), il.end());
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string& replace_with_range(const size_type pos, const size_type count,
            R&& r) {
            auto rg = std::forward<R>(r);
            return replace(pos, count, std::ranges::begin(rg), std::ranges::end(rg));
        }

        template<details::container_compatible_range<char_type> R>
        constexpr basic_cow_string& replace_with_range(const_iterator first, const_iterator last,
            R&& r) {
            auto rg = std::forward<R>(r);
            return replace(first, last, std::ranges::begin(rg), std::ranges::end(rg));
        }

    private:
        details::cow_string_control_block* control_block() const noexcept {
            return reinterpret_cast<details::cow_string_control_block*>(this->control & ~this->control_mask);
        }

        constexpr bool shared() const noexcept { return (this->control & this->shared_mask) != 0; }
        constexpr bool allocated() const noexcept { return (this->control & this->allocated_mask) != 0; }

        constexpr void deallocate_if_needed() noexcept {
            if (allocated()) {
                auto* const block = control_block();
                if (shared()) {
                    if (block->reference_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        details::deallocate(get_allocator(), block);
                    }
                } else {
                    details::deallocate(get_allocator(), block);
                }
            }
        }

        constexpr char_type* allocate_exclusive_buffer(
            const string_view_type str, const std::size_t capacity) {
            const char_type* const str_data = str.data();
            const std::size_t size = std::ranges::min(str.size(), capacity);
            auto [block, actual_capacity] = details::allocate<char_type>(get_allocator(), capacity);
            char_type* const data = block->template data<char_type>();
            std::ranges::copy_n(str_data, size, data);
            data[size] = char_type();
            deallocate_if_needed();
            this->storage.non_sso.size = size;
            this->storage.non_sso.capacity = actual_capacity;
            this->storage.non_sso.data = data;
            this->control = reinterpret_cast<std::uintptr_t>(block) | 0b10;
            return data;
        }

        constexpr char_type* allocate_exclusive_buffer(const string_view_type str) {
            return allocate_exclusive_buffer(str, str.size());
        }

        // Post-condition: exclusive
        constexpr char_type* reserve_or_exclusive_shrink(const size_type new_cap) {
            if (!unique()) {
                if (new_cap > this->sso_capacity) {
                    return allocate_exclusive_buffer(view(), new_cap);
                } else {
                    return write_sso_buffer(view().substr(0, new_cap));
                }
            } else {
                const size_type old_cap = capacity();
                if (new_cap > old_cap) {
                    return allocate_exclusive_buffer(view(), std::ranges::max(new_cap, old_cap * 2));
                } else {
                    if (allocated() && allocated_data() != std::as_const(*this).allocated_data()) {
                        // alias string
                        return write_allocated_buffer(view());
                    } else {
                        return data_unsafe();
                    }
                }
            }
        }

        constexpr char_type* exclusive() { return reserve_or_exclusive_shrink(size()); }

        constexpr char_type* write_sso_buffer(const string_view_type str) noexcept {
            const std::size_t size = str.size();
            char_type* const data = sso_data();
            std::ranges::copy_n(str.data(), size, data);
            data[size] = char_type();
            this->control = (size << 2);
            return data;
        }

        constexpr char_type* write_allocated_buffer(const string_view_type str) {
            const std::size_t size = str.size();
            char_type* const data = allocated_data();
            std::ranges::copy_n(str.data(), size, data);
            data[size] = char_type();
            this->storage.non_sso.size = size;
            this->storage.non_sso.data = data;
            return data;
        }

        constexpr char_type* sso_data() noexcept { return this->storage.small; }
        constexpr const char_type* sso_data() const noexcept { return this->storage.small; }

        constexpr char_type* allocated_data() noexcept {
            return control_block()->template data<char_type>();
        }

        constexpr const char_type* allocated_data() const noexcept {
            return this->storage.non_sso.data;
        }

        constexpr char_type* data_unsafe() noexcept {
            if (allocated()) {
                return allocated_data();
            } else {
                if (shared()) {
                    std::unreachable();
                } else {
                    return sso_data();
                }
            }
        }

        constexpr const char_type* data_unsafe() const noexcept {
            if (allocated()) {
                return allocated_data();
            } else {
                if (shared()) {
                    return this->storage.non_sso.data;
                } else {
                    return sso_data();
                }
            }
        }
    };

    template<typename CharT, typename Alloc, typename U = CharT>
    constexpr auto erase(basic_cow_string<CharT, Alloc>& c, const U& value)
        -> typename basic_cow_string<CharT, Alloc>::size_type {
        auto it = std::ranges::remove(c, value);
        const auto count = std::ranges::distance(it, c.end());
        c.erase(it, c.end());
        return count;
    }

    template<typename CharT, typename Alloc, typename Pred>
    constexpr auto erase_if(basic_cow_string<CharT, Alloc>& c, Pred pred)
        -> typename basic_cow_string<CharT, Alloc>::size_type {
        auto it = std::ranges::remove_if(c, std::move(pred));
        const auto count = std::ranges::distance(it, c.end());
        c.erase(it, c.end());
        return count;
    }


    inline namespace cow_string_operators {

        template<typename CharT, typename Alloc1, typename Alloc2>
        bool operator==(
            const basic_cow_string<CharT, Alloc1>& lhs,
            const basic_cow_string<CharT, Alloc2>& rhs) noexcept {
            return lhs.view() == rhs.view();
        }

        template<typename CharT, typename Alloc1, typename Alloc2>
        auto operator<=>(
            const basic_cow_string<CharT, Alloc1>& lhs,
            const basic_cow_string<CharT, Alloc2>& rhs) noexcept {
            return lhs.view() <=> rhs.view();
        }

        template<typename CharT, typename Alloc>
        bool operator==(
            const basic_cow_string<CharT, Alloc>& lhs,
            const std::type_identity_t<std::basic_string_view<CharT>> rhs) noexcept {
            return lhs.view() == rhs;
        }

        template<typename CharT, typename Alloc>
        auto operator<=>(
            const basic_cow_string<CharT, Alloc>& lhs,
            const std::type_identity_t<std::basic_string_view<CharT>> rhs) noexcept {
            return lhs.view() <=> rhs;
        }

        template<typename CharT, typename Alloc1, typename Alloc2>
        basic_cow_string<CharT, Alloc1> operator+(
            const basic_cow_string<CharT, Alloc1>& lhs,
            const basic_cow_string<CharT, Alloc2>& rhs) {
            basic_cow_string<CharT, Alloc1> result = lhs;
            result.append(rhs);
            return result;
        }

        template<typename CharT, typename Alloc1, typename Alloc2>
        basic_cow_string<CharT, Alloc1> operator+(
            basic_cow_string<CharT, Alloc1>&& lhs,
            const basic_cow_string<CharT, Alloc2>& rhs) {
            lhs.append(rhs);
            return std::move(lhs);
        }

        template<typename CharT, typename Alloc>
        basic_cow_string<CharT, Alloc> operator+(
            const basic_cow_string<CharT, Alloc>& lhs,
            const std::type_identity_t<std::basic_string_view<CharT>> rhs) noexcept {
            basic_cow_string<CharT, Alloc> result = lhs;
            result.append(rhs);
            return result;
        }

        template<typename CharT, typename Alloc>
        basic_cow_string<CharT, Alloc> operator+(
            basic_cow_string<CharT, Alloc>&& lhs,
            const std::type_identity_t<std::basic_string_view<CharT>> rhs) noexcept {
            lhs.append(rhs);
            return std::move(lhs);
        }

        template<typename CharT, typename Alloc>
        basic_cow_string<CharT, Alloc> operator+(
            const std::type_identity_t<std::basic_string_view<CharT>> lhs,
            const basic_cow_string<CharT, Alloc>& rhs) noexcept {
            basic_cow_string<CharT, Alloc> result;
            result.reserve(lhs.size() + rhs.size());
            result.append(lhs);
            result.append(rhs);
            return result;
        }

        template<typename CharT, typename Alloc>
        basic_cow_string<CharT, Alloc> operator+(
            const std::type_identity_t<std::basic_string_view<CharT>> lhs,
            basic_cow_string<CharT, Alloc>&& rhs) noexcept {
            rhs.insert(0, lhs);
            return std::move(rhs);
        }

    } // namespace cow_string_operators

    namespace literals::inline cow_string_literals {

        template<details::cow_string_literal_helper helper>
        consteval auto operator""_cowstr() -> typename decltype(helper)::cow_string_type {
            return helper.literal;
        }

    } // namespace literals::cow_string_literals

    using cow_string = basic_cow_string<char>;
    using cow_wstring = basic_cow_string<wchar_t>;
    using cow_u8string = basic_cow_string<char8_t>;
    using cow_u16string = basic_cow_string<char16_t>;
    using cow_u32string = basic_cow_string<char32_t>;
    
    static_assert(sizeof(cow_string) == 4 * sizeof(void*));

} // namespace mylib

namespace std {

    template<typename CharT, typename Allocator, typename TargetCharT>
    struct formatter<mylib::basic_cow_string<CharT, Allocator>, TargetCharT>
        : private formatter<basic_string_view<CharT>, TargetCharT>
    {
        using arg = mylib::basic_cow_string<CharT, Allocator>;
        using base = std::formatter<basic_string_view<CharT>, CharT>;
        using base::parse;

        template<typename FormatContext>
        auto format(const arg& str, FormatContext& ctx) const {
            return base::format(str.view(), ctx);
        }
    };

    template<typename CharT, typename Allocator>
    struct hash<mylib::basic_cow_string<CharT, Allocator>> {
        using argument_type = mylib::basic_cow_string<CharT, Allocator>;
        using result_type = std::size_t;

        result_type operator()(const argument_type& str) const noexcept {
            return std::hash<std::basic_string_view<CharT>>{}(str.view());
        }
    };

} // namespace std

#endif // MYLIB_COW_STRING_H
