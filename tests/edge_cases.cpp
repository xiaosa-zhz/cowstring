// Edge-case coverage tests
//
// The broad suites (conformance, replace_self, state, api_coverage) exercise
// the ordinary paths. This file targets the narrow branches of the COW state
// machine that those suites never enter:
//
//   * moving a substring out of a literal (no allocation to steal)
//   * reusing the prefix of a substring allocation in reserve()
//   * self swap / self copy-assign / self move-assign
//   * deep copies between unequal (stateful) allocators: copy and move with
//     pos/count, swap, copy and move assignment
//   * the materialisation path of a large, non-contiguous replace source
//   * both extra strategies of operator+
//   * the std::length_error thrown when a capacity request exceeds max_size()
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <atomic>
#include <cstddef>
#include <iterator>
#include <list>
#include <memory>
#include <print>
#include <sstream>
#include <generator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::size_t cases = 0;
std::size_t failures = 0;

void check(bool condition, std::string_view what) {
    ++cases;
    if (!condition) {
        ++failures;
        std::println(stderr, "FAIL: {}", what);
    }
}

void check_equal(std::size_t got, std::size_t want, std::string_view what) {
    ++cases;
    if (got != want) {
        ++failures;
        std::println(stderr, "FAIL: {} (got={}, want={})", what, got, want);
    }
}

using S = cow::cow_string;
using SV = std::string_view;

// Mirrors the two buffers that replace() uses for non-contiguous sources:
//   * the sized/forward branch materialises the whole source into one stack
//     buffer of stack_buffer_max characters (when the receiver is not empty);
//   * the single-pass input branch flushes a smaller cache in chunks.
// The sizes below are chosen to land on, and just past, those boundaries.
constexpr std::size_t stack_buffer_max = 2048 / sizeof(S::value_type);
constexpr std::size_t input_cache_max = 128 / sizeof(S::value_type);

// ---------------------------------------------------------------------------
// Moving from a literal with a position/count has no allocation to steal, so
// the destination has to stay bound to the static storage at the requested
// offset. This is the assign_static_string() branch of set_states_on_move().
void test_move_from_literal() {
    constexpr SV whole = "0123456789abcdefghijklmnopqrstuvwxyz";

    S source = "0123456789abcdefghijklmnopqrstuvwxyz";
    check(source.is_literal(), "long literal is bound to static storage");
    check_equal(source.size(), whole.size(), "literal source size");
    const char* const base = std::as_const(source).data();

    S moved(std::move(source), 5, 25);
    check(moved.is_literal(), "move(pos,count) from literal stays literal");
    check_equal(moved.size(), 25, "move(pos,count) from literal size");
    check(moved.view() == whole.substr(5, 25), "move(pos,count) from literal content");
    check(std::as_const(moved).data() == base + 5, "move(pos,count) from literal offset");

    S rest_source = "0123456789abcdefghijklmnopqrstuvwxyz";
    S rest(std::move(rest_source), 2);
    check(rest.is_literal(), "move(pos) from literal stays literal");
    check_equal(rest.size(), whole.size() - 2, "move(pos) from literal size");
    check(rest.view() == whole.substr(2), "move(pos) from literal content");

    // A substring that fits the small buffer is written to SSO instead, so the
    // literal branch must not be taken for it.
    S prefix_source = "0123456789abcdefghijklmnopqrstuvwxyz";
    S small(std::move(prefix_source), 5, 10);
    check(small.is_small(), "move(pos,count) from literal into SSO");
    check(small.view() == whole.substr(5, 10), "move(pos,count) SSO content");
}

// ---------------------------------------------------------------------------
// A substring shares the source allocation with a prefix offset, so its usable
// capacity stops at the end of the allocation. Once the owner is gone the
// survivor is exclusive but still has a gap in front of it; reserve() into that
// gap must relocate the contents to the base of the allocation instead of
// allocating a new buffer.
void test_prefix_reserve_reuse() {
    std::string pattern;
    for (std::size_t i = 0; i != 100; ++i) {
        pattern.push_back(char('a' + i % 26));
    }

    const char* base = nullptr;
    S sub;
    {
        S owner(pattern);
        check(owner.is_owned(), "source is an owned string");
        base = std::as_const(owner).data();
        sub = S(owner, 10, 50);  // 50 > SSO capacity, so it shares
        check(sub.is_shared(), "substring shares the allocation");
        check_equal(sub.cur_count(), 2, "substring reference count");
    }
    check(sub.exclusive(), "surviving substring became exclusive");

    const std::string expected(sub.view());
    const auto capacity = sub.capacity();
    const auto real_capacity = sub.real_capacity();
    const char* const before = std::as_const(sub).data();
    check_equal(std::size_t(before - base), 10, "substring keeps the prefix offset");
    // The prefix is exactly the difference between the two capacities, so the
    // gap is always non-empty for a substring taken at a non-zero offset.
    check(real_capacity > capacity, "substring has a prefix gap to reuse");
    check_equal(std::size_t(real_capacity - capacity), 10, "prefix gap size");

    sub.reserve(capacity + 1);  // fits in the gap: rebase, do not allocate
    check_equal(sub.size(), expected.size(), "reserve keeps the size");
    check(sub.view() == expected, "reserve keeps the contents");
    check(std::as_const(sub).data() == base, "reserve rebased onto the allocation base");
    check_equal(sub.capacity(), real_capacity, "reserve exposes the whole allocation");

    // The reused prefix is writable, and the string stays usable afterwards.
    sub[0] = 'Z';
    check(sub.front() == 'Z', "reused prefix is writable");
    sub.push_back('!');
    check_equal(sub.size(), expected.size() + 1, "string usable after the rebase");
    check(sub.back() == '!', "appended character after the rebase");
}

// ---------------------------------------------------------------------------
// Self swap / self assignment must be inert, keep the buffer and keep the
// reference count of a shared allocation intact.
void test_self_swap_and_assign() {
    S s(40, 'z');
    const char* const buffer = std::as_const(s).data();

    s.swap(s);
    check_equal(s.size(), 40, "self swap keeps the size");
    check(s.front() == 'z', "self swap keeps the contents");
    check(std::as_const(s).data() == buffer, "self swap keeps the buffer");

    // Go through a differently named reference so that the compiler does not
    // flag the deliberate self assignment as suspicious.
    S& alias = s;
    alias = s;
    check_equal(s.size(), 40, "self copy assignment keeps the size");
    check(std::as_const(s).data() == buffer, "self copy assignment keeps the buffer");

    s = std::move(alias);
    check_equal(s.size(), 40, "self move assignment keeps the size");
    check(s.front() == 'z', "self move assignment keeps the contents");
    check(std::as_const(s).data() == buffer, "self move assignment keeps the buffer");

    S owner(40, 'y');
    S shared = owner;
    check_equal(shared.cur_count(), 2, "shared before self swap");
    shared.swap(shared);
    check_equal(shared.cur_count(), 2, "self swap keeps the reference count");
    check(shared.view() == owner.view(), "self swap keeps a shared view intact");
}

// ---------------------------------------------------------------------------
// Capacity policy of replace() that has to reallocate:
//   * an exclusive string keeps its contents in place, so growing it doubles
//     the allocation and the spare capacity is reused by later replaces;
//   * a string that has to detach from a shared allocation is rebuilt from
//     scratch, so it asks for exactly the new size and never carries the old
//     (possibly much larger) allocation along.
void test_replace_growth_policy() {
    // Exclusive string: double the allocation, and reuse it next time.
    {
        S s(40, 'a');
        const std::size_t old_real_capacity = s.real_capacity();
        const std::string first(20, 'b');

        s.replace(0, 0, first);  // 60 > 48, so this one reallocates
        check_equal(s.size(), 60, "growing replace size");
        check(s.real_capacity() >= 2 * old_real_capacity, "growing replace doubles the allocation");
        check(s.real_capacity() >= s.size(), "grown allocation fits the new size");

        const char* const buffer = std::as_const(s).data();
        const std::string second(20, 'c');
        s.replace(0, 0, second);  // 80 <= the doubled capacity: stays in place
        check_equal(s.size(), 80, "second growing replace size");
        check(std::as_const(s).data() == buffer, "second growing replace reuses the spare capacity");
        check(s.view() == SV(second + first + std::string(40, 'a')),
              "growing replace contents");

        // A replace that also shrinks the string does not allocate at all.
        const char* const buffer_after = std::as_const(s).data();
        s.replace(0, 40, SV("tiny"));
        check(std::as_const(s).data() == buffer_after, "shrinking replace keeps the buffer");
        check(s.view() == SV("tiny" + std::string(40, 'a')), "shrinking replace contents");
    }

    // Shared string that has to detach: the new size still fits the old
    // allocation, so ask for exactly the new size instead of doubling.
    {
        S owner(100, 'x');
        S shared = owner;
        check(shared.is_shared(), "receiver shares its allocation");
        const std::size_t old_real_capacity = shared.real_capacity();
        const std::string added(10, 'y');  // 110 <= 112

        shared.replace(0, 0, added);
        check_equal(shared.size(), 110, "shared growing replace size");
        check(shared.exclusive(), "shared growing replace detaches");
        check(shared.real_capacity() >= shared.size(), "detached allocation fits the contents");
        check(shared.real_capacity() < 2 * old_real_capacity,
              "shared growing replace does not double");
        check(shared.view() == SV(added + std::string(100, 'x')),
              "shared growing replace contents");
        check(owner.view() == SV(std::string(100, 'x')), "shared owner is left untouched");
        check_equal(owner.cur_count(), 1, "owner is alone again after the detach");
    }

    // Shared string that has to detach and outgrow the old allocation: the new
    // allocation is still exactly the new size, not the doubled old one.
    {
        S owner(100, 'x');
        S shared = owner;
        const std::size_t old_real_capacity = shared.real_capacity();
        const std::string added(40, 'z');  // 140 > 112

        shared.replace(0, 0, added);
        check_equal(shared.size(), 140, "shared outgrowing replace size");
        check(shared.real_capacity() >= shared.size(), "detached allocation fits the contents");
        check(shared.real_capacity() < 2 * old_real_capacity,
              "shared outgrowing replace does not double");
        check(shared.view() == SV(added + std::string(100, 'x')),
              "shared outgrowing replace contents");
        check_equal(owner.cur_count(), 1, "owner is alone again after the outgrowing detach");
    }

    // Shared string that shrinks while detaching: the much larger old
    // allocation must not be carried over to the fresh one.
    {
        S owner(1200, 'a');
        S shared = owner;
        const std::size_t old_real_capacity = shared.real_capacity();
        shared.replace(0, 1100, SV("x"));  // 101 characters left

        check_equal(shared.size(), 101, "shrinking detach size");
        check(shared.view() == SV("x" + std::string(100, 'a')), "shrinking detach contents");
        check(shared.real_capacity() >= shared.size(), "shrinking detach fits the contents");
        check(shared.real_capacity() < old_real_capacity,
              "shrinking detach drops the old allocation size");
        check(owner.view() == SV(std::string(1200, 'a')), "shrinking detach leaves the owner alone");
        check_equal(owner.cur_count(), 1, "owner is alone again after the shrinking detach");
    }
}

// ---------------------------------------------------------------------------
// A stateful allocator whose instances do not compare equal, does not
// propagate on copy/move/swap and is not always equal: strings owned by it can
// never share storage, so every cross-allocator operation has to deep copy.
struct allocation_stats {
    std::atomic<long> live_blocks{0};
    std::atomic<long> total_allocations{0};
};

template<typename T>
struct tagged_allocator {
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;

    int tag = 0;
    allocation_stats* stats = nullptr;

    tagged_allocator() = default;
    explicit tagged_allocator(int id, allocation_stats* s = nullptr) : tag(id), stats(s) {}
    template<typename U>
    tagged_allocator(const tagged_allocator<U>& other) : tag(other.tag), stats(other.stats) {}

    T* allocate(std::size_t count) {
        if (stats != nullptr) {
            stats->live_blocks.fetch_add(1, std::memory_order_relaxed);
            stats->total_allocations.fetch_add(1, std::memory_order_relaxed);
        }
        return std::allocator<T>{}.allocate(count);
    }

    void deallocate(T* ptr, std::size_t count) noexcept {
        if (stats != nullptr) {
            stats->live_blocks.fetch_sub(1, std::memory_order_relaxed);
        }
        std::allocator<T>{}.deallocate(ptr, count);
    }

    friend bool operator==(const tagged_allocator& lhs, const tagged_allocator& rhs) {
        return lhs.tag == rhs.tag && lhs.stats == rhs.stats;
    }
};

void test_allocator_mismatch() {
    using T = cow::basic_cow_string<char, tagged_allocator<char>>;

    allocation_stats stats;
    const tagged_allocator<char> first(1, &stats);
    const tagged_allocator<char> second(2, &stats);

    // Copy with pos/count: the subview exceeds SSO and the allocators differ,
    // so the substring is copied into a fresh allocation of the destination.
    {
        T source(60, 'q', first);
        check(source.get_allocator().tag == 1, "source allocator tag");
        T copy(source, 10, 40, second);
        check_equal(copy.size(), 40, "cross-allocator copy(pos,count) size");
        check(copy.view() == SV(source.view()).substr(10, 40), "cross-allocator copy content");
        check(copy.get_allocator().tag == 2, "copy keeps the destination allocator");
        check(!copy.is_shared(), "cross-allocator copy is not shared");
    }

    // Move with pos/count under unequal allocators cannot steal storage either.
    {
        T source(60, 'q', first);
        T moved(std::move(source), 10, 40, second);
        check_equal(moved.size(), 40, "cross-allocator move(pos,count) size");
        check(moved.view() == SV(std::string(40, 'q')), "cross-allocator move content");
        check(moved.get_allocator().tag == 2, "move keeps the destination allocator");
    }

    // swap between unequal, non-propagating allocators: contents swap, the
    // allocators stay with their objects. (Formally UB for std containers;
    // cow_string makes it well defined.)
    {
        T lhs(60, 'x', first);
        T rhs(60, 'y', second);
        const long before = stats.live_blocks.load();
        lhs.swap(rhs);
        check(lhs.view() == SV(std::string(60, 'y')), "cross-allocator swap lhs contents");
        check(rhs.view() == SV(std::string(60, 'x')), "cross-allocator swap rhs contents");
        check(lhs.get_allocator().tag == 1, "cross-allocator swap keeps lhs allocator");
        check(rhs.get_allocator().tag == 2, "cross-allocator swap keeps rhs allocator");
        check_equal(std::size_t(stats.live_blocks.load()), std::size_t(before),
                    "cross-allocator swap moves blocks, does not leak them");
    }

    // Copy assignment and move assignment across unequal allocators.
    {
        T dst(60, 'a', first);
        T src(60, 'b', second);
        dst = src;
        check(dst.view() == SV(std::string(60, 'b')), "cross-allocator copy assignment");
        check(dst.get_allocator().tag == 1, "copy assignment keeps the allocator");
        check(src.view() == SV(std::string(60, 'b')), "copy assignment keeps the source");

        T other(60, 'c', second);
        dst = std::move(other);
        check(dst.view() == SV(std::string(60, 'c')), "cross-allocator move assignment");
        check(dst.get_allocator().tag == 1, "move assignment keeps the allocator");
    }

    // Moving without an explicit allocator adopts the allocator of the source
    // (allocator_move), including its non always-equal branch.
    {
        T src(60, 'm', first);
        T moved(std::move(src));
        check(moved.view() == SV(std::string(60, 'm')), "plain move ctor contents");
        check(moved.get_allocator().tag == 1, "plain move ctor adopts the allocator");
        check(moved.is_owned(), "plain move ctor steals the buffer");
        check_equal(moved.cur_count(), 1, "plain move ctor leaves one owner");
    }

    // Two equal allocator instances can share again, which is the middle branch
    // of assign(): copy the state, do not copy the characters.
    {
        const tagged_allocator<char> same_tag(1, &stats);
        T dst(60, 'a', first);
        T src(60, 'b', same_tag);
        check(dst.get_allocator() == src.get_allocator(), "allocators compare equal");

        dst = src;
        check(dst.view() == src.view(), "equal-allocator copy assignment contents");
        check(dst.is_shared(), "equal-allocator copy assignment shares");
        check_equal(dst.cur_count(), 2, "equal-allocator copy assignment refcount");

        T stolen(60, 'c', same_tag);
        dst = std::move(stolen);
        check(dst.view() == SV(std::string(60, 'c')), "equal-allocator move assignment contents");
        check(dst.is_owned(), "equal-allocator move assignment steals the buffer");
        check_equal(dst.cur_count(), 1, "equal-allocator move assignment refcount");
        check(stolen.empty(), "equal-allocator move assignment empties the source");
    }

    // swap where one side holds no allocation: the small (or literal) state is
    // handed over verbatim by the deep-copy helper.
    {
        T small_string(3, 's', first);
        T big_string(60, 'b', second);
        check(small_string.is_small(), "swap operand is small");
        small_string.swap(big_string);
        check(big_string.view() == SV("sss"), "swap moved the small contents over");
        check(small_string.view() == SV(std::string(60, 'b')), "swap moved the big contents over");
        check(big_string.is_small(), "small state moved to the other object");
        check_equal(small_string.get_allocator().tag, 1, "swap keeps the small operand allocator");
        check_equal(big_string.get_allocator().tag, 2, "swap keeps the big operand allocator");
    }

    // A literal operand exercises the same non-allocated branch with static
    // storage instead of the small buffer.
    {
        T literal_side = "0123456789abcdefghijklmnopqrstuvwxyz";
        T allocated_side(60, 'L', second);
        check(literal_side.is_literal(), "swap operand is literal");
        literal_side.swap(allocated_side);
        check(allocated_side.view() == SV("0123456789abcdefghijklmnopqrstuvwxyz"),
              "swap moved the literal contents over");
        check(literal_side.view() == SV(std::string(60, 'L')), "swap moved the allocated contents over");
        check(allocated_side.is_literal(), "literal state moved to the other object");
    }

    check_equal(std::size_t(stats.live_blocks.load()), 0, "all blocks released");
    check(stats.total_allocations.load() > 0, "the allocator was actually used");
}

// ---------------------------------------------------------------------------
// A replace source that is not contiguous is materialised first, either into
// the shared stack buffer or into a temporary string. Which of the two is used
// depends only on the length of the source, so the sizes below sit on that
// boundary.
void test_large_noncontiguous_replace() {
    constexpr std::size_t room = 3 * stack_buffer_max + 10;

    // Just below the buffer limit: the source is copied onto the stack.
    const std::list<char> fits(stack_buffer_max - 1, 'b');
    S buffered(10, 'a');
    buffered.reserve(room);
    buffered.replace(0, 0, fits.begin(), fits.end());
    check_equal(buffered.size(), stack_buffer_max + 9, "buffered non-contiguous replace size");
    check(buffered.view() == SV(std::string(stack_buffer_max - 1, 'b') + std::string(10, 'a')),
          "buffered non-contiguous replace contents");

    // Exactly one buffer worth still fits: the branch compares with <=.
    const std::list<char> at_limit(stack_buffer_max, 'c');
    S at_limit_target(10, 'a');
    at_limit_target.reserve(room);
    at_limit_target.replace(0, 0, at_limit.begin(), at_limit.end());
    check_equal(at_limit_target.size(), stack_buffer_max + 10,
                "at-limit non-contiguous replace size");
    check(at_limit_target.view() == SV(std::string(stack_buffer_max, 'c') + std::string(10, 'a')),
          "at-limit non-contiguous replace contents");

    // One past the limit: the source goes through a temporary cow_string.
    const std::list<char> beyond(stack_buffer_max + 1, 'd');
    S large(10, 'a');
    large.reserve(room);
    large.replace(0, 0, beyond.begin(), beyond.end());
    check_equal(large.size(), stack_buffer_max + 11, "large non-contiguous replace size");
    check(large.view() == SV(std::string(stack_buffer_max + 1, 'd') + std::string(10, 'a')),
          "large non-contiguous replace contents");

    // A shared receiver cannot be written in place, so the replacement detaches
    // it and leaves the other owner alone.
    S owner(1200, 'a');
    S shared = owner;
    check(!shared.exclusive(), "receiver is shared");
    const std::list<char> tail(30, 'b');
    shared.replace(0, 10, tail.begin(), tail.end());
    check_equal(shared.size(), 1220, "shared receiver replace size");
    check(shared.view() == SV(std::string(30, 'b') + std::string(1190, 'a')),
          "shared receiver replace contents");
    check(shared.exclusive(), "shared receiver is detached by the replace");
    check_equal(owner.size(), 1200, "shared owner is not modified");
    check_equal(owner.cur_count(), 1, "owner is alone again after the detach");

    // A single pass source has no distance and is materialised directly.
    std::istringstream input("XYZ");
    S from_input("abc");
    from_input.replace(1, 1, std::istream_iterator<char>(input), std::istream_iterator<char>());
    check(from_input.view() == SV("aXYZc"), "single pass replace contents");
}

std::generator<char> failing_input_range(const char* begin, const char* end, const char* fail_at, int* failures) {
    for (const char* cur = begin; cur != end; ++cur) {
        if (cur == fail_at) {
            ++*failures;
            throw std::runtime_error("source read failed");
        }
        co_yield *cur;
    }
}

// A multipass (forward) source whose dereference throws at a chosen position.
// Unlike failing_input_range() (a generator, i.e. an input range), this one
// takes the sized/forward branch of replace().
struct throwing_forward_iterator {
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using value_type = char;
    using difference_type = std::ptrdiff_t;

    const char* cur = nullptr;
    const char* fail_at = nullptr;
    int* failures = nullptr;

    char operator*() const {
        if (cur == fail_at) {
            ++*failures;
            throw std::runtime_error("source read failed");
        }
        return *cur;
    }

    throwing_forward_iterator& operator++() {
        ++cur;
        return *this;
    }

    throwing_forward_iterator operator++(int) {
        auto old = *this;
        ++cur;
        return old;
    }

    friend bool operator==(const throwing_forward_iterator& lhs,
                           const throwing_forward_iterator& rhs) {
        return lhs.cur == rhs.cur;
    }
};

void test_replace_single_pass_empty() {
    // Success path: an empty receiver is filled directly, without a temporary.
    // istreambuf_iterator is a plain single pass input iterator and, unlike
    // istream_iterator, does not skip whitespace.
    std::istringstream input("streamed in");
    S s;
    s.replace(0, 0, std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{});
    check(s.view() == SV("streamed in"), "single pass source into an empty string");
    check(s.exclusive(), "replace leaves the string exclusive");

    // The iterator range constructor forwards to the same path, which is what
    // the direct append loop mainly exists for.
    std::istringstream ctor_input("constructed");
    S built(std::istreambuf_iterator<char>(ctor_input), std::istreambuf_iterator<char>{});
    check(built.view() == SV("constructed"), "single pass iterator constructor");

    std::istringstream empty_input("");
    S from_empty(std::istreambuf_iterator<char>(empty_input), std::istreambuf_iterator<char>{});
    check(from_empty.empty(), "single pass source can be empty");
    check(from_empty.exclusive(), "empty result is exclusive");

    // Just below one buffer: it grows past the small string and is only handed
    // over once the source is exhausted, so no intermediate flush happens.
    const std::string below_limit(input_cache_max - 1, 'g');
    std::istringstream long_input(below_limit);
    S grown(std::istreambuf_iterator<char>(long_input), std::istreambuf_iterator<char>{});
    check(grown.view() == SV(below_limit), "single pass source just below the buffer limit");
    check(grown.is_owned(), "long single pass source is allocated");

    // Source larger than one buffer: the buffer has to be flushed while reading,
    // and the tail left over is flushed once the source is exhausted.
    const std::string bulk(3 * input_cache_max + 7, 'b');
    std::istringstream bulk_input(bulk);
    S from_bulk(std::istreambuf_iterator<char>(bulk_input), std::istreambuf_iterator<char>{});
    check(from_bulk.view() == SV(bulk), "single pass source spanning several chunks");
    check(from_bulk.is_owned(), "chunked single pass source is allocated");

    // An exact multiple of the chunk size leaves the buffer empty at the end,
    // so the final flush is skipped.
    const std::string exact(2 * input_cache_max, 'c');
    std::istringstream exact_input(exact);
    S from_exact(std::istreambuf_iterator<char>(exact_input), std::istreambuf_iterator<char>{});
    check(from_exact.view() == SV(exact), "single pass source on a chunk boundary");

    // A non-empty receiver keeps using the materialise-then-replace path: once
    // for a literal receiver (not small) and once for a small receiver that
    // merely has characters in it.
    std::istringstream small_input("XY");
    S non_empty("abc");
    non_empty.replace(1, 1, std::istream_iterator<char>(small_input), std::istream_iterator<char>());
    check(non_empty.view() == SV("aXYc"), "single pass source into a non-empty string");

    std::istringstream tiny_input("Q");
    S tiny(2, 'a');
    check(tiny.is_small() && !tiny.empty(), "receiver is small but not empty");
    tiny.replace(0, 0, std::istreambuf_iterator<char>(tiny_input), std::istreambuf_iterator<char>{});
    check(tiny.view() == SV("Qaa"), "small non-empty receiver materialises the source");
}

void test_replace_throw_in_source() {
    // The read fails while the string is still small.
    {
        const char* const text = "abcdefgh";
        int failures = 0;
        S s;
        bool caught = false;
        try {
            s.replace_with_range(0, 0, failing_input_range(text, text + 8, text + 3, &failures));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "replace propagates a source failure");
        check(s.empty(), "partial characters are cleared");
        check(s.is_small(), "small string has nothing to release");
        check_equal(std::size_t(failures), 1, "the source failed once");
    }

    // The read fails after the string has already grown an allocation, so the
    // cleanup has to clear it and give the buffer back.
    {
        const std::string text(40, 'x');
        int failures = 0;
        S s;
        bool caught = false;
        try {
            s.replace_with_range(0, 0, failing_input_range(text.data(), text.data() + 40, text.data() + 30, &failures));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "replace propagates a late source failure");
        check(s.empty(), "grown string is cleared after a failure");
        check(s.is_small(), "grown string releases its buffer after a failure");
        check_equal(std::size_t(failures), 1, "the late source failed once");
        s.push_back('z');  // still usable
        check(s.view() == SV("z"), "string is reusable after a source failure");
    }

    // The read fails only after whole chunks have already been handed over to
    // the string, so the cleanup has to drop those as well.
    {
        const std::string text(3 * input_cache_max + 9, 'w');
        const char* const fail_at = text.data() + 2 * input_cache_max + 1;
        int failures = 0;
        S s;
        bool caught = false;
        try {
            s.replace_with_range(0, 0,
                                 failing_input_range(text.data(), text.data() + text.size(),
                                                     fail_at, &failures));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "replace propagates a failure past a chunk boundary");
        check(s.empty(), "already appended chunks are cleared");
        check(s.is_small(), "buffer is released after a failure past a chunk");
        check_equal(std::size_t(failures), 1, "the source failed once past a chunk");
    }

    // A throwing iterator range constructor: the exception escapes and the
    // half-built string is released without leaking.
    {
        const std::string text(40, 'y');
        int failures = 0;
        bool caught = false;
        try {
            S built(std::from_range, failing_input_range(text.data(), text.data() + 40, text.data() + 30, &failures));
            (void)built;
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "iterator constructor propagates a source failure");
        check_equal(std::size_t(failures), 1, "the constructor source failed once");
    }
}

// ---------------------------------------------------------------------------
// The empty-small fast path of replace(): when the receiver holds nothing, a
// sized/forward source is written straight into the freshly sized storage.
// Nothing can alias an empty string, so the stack buffer and the temporary
// materialisation are skipped even for sources larger than the stack buffer.
void test_replace_empty_small_fast_path() {
    // Forward (non-sized) source: the distance is walked, then copied directly.
    {
        const std::list<char> source(40, 'f');
        S s;
        check(s.is_small() && s.empty(), "empty fast path starts small and empty");
        s.replace(0, 0, source.begin(), source.end());
        check(s.view() == SV(std::string(40, 'f')), "empty receiver fills from a forward range");
        check(s.exclusive(), "empty receiver is exclusive after the fill");
    }

    // An empty source leaves the receiver untouched.
    {
        const std::list<char> none;
        S s;
        s.replace(0, 0, none.begin(), none.end());
        check(s.empty() && s.is_small(), "empty forward source leaves the receiver empty");
        check(s.exclusive(), "empty forward source keeps the receiver exclusive");
    }

    // Larger than the stack buffer: no temporary string, the empty receiver is
    // resized once and the source is copied straight in.
    {
        const std::list<char> huge(2 * stack_buffer_max + 5, 'h');
        S s;
        s.replace(0, 0, huge.begin(), huge.end());
        check_equal(s.size(), huge.size(), "empty receiver takes a large forward source");
        check(s.is_owned(), "large empty fast path allocates");
        check(s.view() == SV(std::string(huge.size(), 'h')),
              "large empty fast path contents");
    }

    // A source failure midway through the direct copy has to roll the receiver
    // back and release the buffer that adjust_old_for_replace() just grew.
    {
        const std::string text(200, 't');
        int failures = 0;
        S s;
        bool caught = false;
        try {
            s.replace(0, 0,
                      throwing_forward_iterator{text.data(), text.data() + 150, &failures},
                      throwing_forward_iterator{text.data() + text.size(), nullptr, &failures});
        } catch (const std::runtime_error&) {
            caught = true;
        }
        check(caught, "forward source failure propagates");
        check(s.empty(), "partial forward characters are cleared");
        check(s.is_small(), "forward source failure releases the grown buffer");
        check_equal(std::size_t(failures), 1, "the forward source failed once");
        s.push_back('z');
        check(s.view() == SV("z"), "receiver is reusable after a forward source failure");
    }
}

// ---------------------------------------------------------------------------
// operator+ tries the left operand, then the right operand, then falls back to
// appending into the left one.
void test_operator_plus_paths() {
    // The left operand is a short literal (no room), the right one is exclusive
    // with spare capacity: the left operand goes in front of the right one.
    S rhs(30, 'x');
    rhs.reserve(200);
    check(rhs.exclusive(), "right operand is exclusive");
    S sum = S("ab") + std::move(rhs);
    check_equal(sum.size(), 32, "operator+ reuses the right operand size");
    check(sum.view() == SV("ab" + std::string(30, 'x')),
          "operator+ reuses the right operand contents");

    // With both operands shared and without spare capacity, operator+ appends
    // into the copy of the left operand.
    S left(50, 'a');
    S left_alias = left;
    S right(50, 'b');
    S right_alias = right;
    check(!left.exclusive() && !right.exclusive(), "both operands are shared");
    S joined = left + right;
    check_equal(joined.size(), 100, "operator+ fallback size");
    check(joined.view() == SV(std::string(50, 'a') + std::string(50, 'b')),
          "operator+ fallback contents");
}

// ---------------------------------------------------------------------------
// shrink_to_fit() on an exclusive string without spare capacity is a no-op,
// which is what makes the operation idempotent.
void test_shrink_to_fit_no_slack() {
    S exact(64, 'a');  // 64 is a multiple of the control block size: no slack
    check(exact.exclusive(), "string is exclusive");
    check_equal(exact.capacity(), exact.size(), "allocation is exactly full");
    const char* const buffer = std::as_const(exact).data();
    exact.shrink_to_fit();
    check_equal(exact.size(), 64, "no-slack shrink keeps the size");
    check_equal(exact.capacity(), 64, "no-slack shrink keeps the capacity");
    check(std::as_const(exact).data() == buffer, "no-slack shrink keeps the buffer");

    // The same state reached from a string that does have slack to give up.
    S slack(50, 'b');
    slack.resize(slack.capacity(), 'b');
    check_equal(slack.size(), slack.capacity(), "resized up to capacity");
    slack.shrink_to_fit();
    check_equal(slack.size(), slack.capacity(), "shrinking a full string is a no-op");
    check(slack.view() == SV(std::string(64, 'b')), "no-slack shrink keeps the contents");
}

// ---------------------------------------------------------------------------
// details::allocate() rejects capacities beyond max_size(). reserve() must
// surface that as std::length_error and leave the string untouched.
void test_allocation_too_large() {
    S owned(100, 'L');
    bool threw_length_error = false;
    try {
        owned.reserve(S::max_size() + 1);
    } catch (const std::length_error&) {
        threw_length_error = true;
    }
    check(threw_length_error, "reserve(max_size()+1) throws std::length_error");
    check_equal(owned.size(), 100, "size unchanged after length_error");
    check(owned.view() == SV(std::string(100, 'L')), "contents unchanged after length_error");
    owned.push_back('!');  // still usable
    check_equal(owned.size(), 101, "string usable after length_error");

    // Same request on a shared string, which takes the "copy then allocate"
    // route through allocate_exclusive_buffer().
    S owner(100, 'S');
    S shared = owner;
    threw_length_error = false;
    try {
        shared.reserve(S::max_size() + 1);
    } catch (const std::length_error&) {
        threw_length_error = true;
    }
    check(threw_length_error, "shared reserve(max_size()+1) throws std::length_error");
    check(shared.view() == owner.view(), "shared string unchanged after length_error");
    check_equal(shared.cur_count(), 2, "shared string still shared after length_error");

    // A larger capacity than max_size() is also rejected on a literal, which
    // reaches the growing branch directly.
    S literal = "0123456789abcdefghijklmnopqrstuvwxyz";
    threw_length_error = false;
    try {
        literal.reserve(S::max_size() + 16);
    } catch (const std::length_error&) {
        threw_length_error = true;
    }
    check(threw_length_error, "literal reserve(max_size()+16) throws std::length_error");
    check(literal.is_literal(), "literal unchanged after length_error");
}

} // namespace

int main() {
    test_move_from_literal();
    test_prefix_reserve_reuse();
    test_replace_growth_policy();
    test_self_swap_and_assign();
    test_allocator_mismatch();
    test_large_noncontiguous_replace();
    test_replace_single_pass_empty();
    test_replace_empty_small_fast_path();
    test_replace_throw_in_source();
    test_operator_plus_paths();
    test_shrink_to_fit_no_slack();
    test_allocation_too_large();

    if (failures != 0) {
        std::println("[edge] FAILED: cases={} failures={}", cases, failures);
        return 1;
    }
    std::println("[edge] PASS: cases={}", cases);
    return 0;
}
