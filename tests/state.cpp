// Internal COW-state transition tests
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <cstddef>
#include <print>
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

} // namespace

int main() {
    using namespace cow::literals;

    constexpr std::string_view initial = "Hello, World!";
    constexpr std::string_view appended =
        " I am a Copy-On-Write string. Nice to meet you!";
    constexpr std::string_view cow = "Copy-On-Write";
    constexpr std::string_view short_cow = "COW";
    constexpr std::string_view long_cow = "Copy-On-Write (COW)";
    constexpr std::string_view after_short =
        "Hello, World! I am a COW string. Nice to meet you!";
    constexpr std::string_view after_long =
        "Hello, World! I am a Copy-On-Write (COW) string. Nice to meet you!";

    cow::cow_string str = "Hello, World!"_cows;
    check(str.is_literal(), "initial literal status");
    check(!str.is_small(), "literal is not SSO");
    check(str == initial, "initial content");
    check_equal(str.size(), initial.size(), "initial size");

    str.append(appended);
    check(str.is_owned(), "append makes owned");
    check(str.cur_count() == 1, "owned refcount is one");
    check(str.size() == initial.size() + appended.size(), "appended size");

    cow::cow_string str2 = str;
    check(str.is_shared(), "source is shared after copy");
    check(str2.is_shared(), "copy is shared");
    check(str.cur_count() == 2, "source refcount is two");
    check(str2.cur_count() == 2, "copy refcount is two");
    check(str2 == str, "copy content");

    str2.replace(str2.find(cow), cow.size(), short_cow);
    check(str2.is_owned(), "replace detaches shared copy");
    check(str2.cur_count() == 1, "detached copy refcount is one");
    check(str2 == after_short, "short replace content");
    check(str.is_shared(), "source remains shared after detach");
    check(str.cur_count() == 1, "source refcount drops to one");

    str.replace(str.find(cow), cow.size(), long_cow);
    check(str.is_owned(), "replace on shared-but-unique string is in place");
    check(str == after_long, "long replace content");
    check(str.substr(0, 14) == "Hello, World! ", "substr content");

    // A short literal is copied into the SSO buffer rather than left bound to
    // static storage: SSO is writable, so assignment and copying materialize
    // the characters instead of aliasing the literal.
    constexpr const char* goodbye = "Goodbye!";
    str2 = goodbye;
    check(str2.is_small(), "assignment from constexpr pointer uses SSO");
    check(!str2.is_owned(), "SSO assignment is not owned");
    check(str2 == "Goodbye!", "SSO assignment content");

    cow::cow_string str3 = str2;
    check(str3.is_small(), "SSO copy stays SSO");
    str3.shrink_to_fit();
    check(str3.is_small(), "shrink_to_fit keeps SSO state");
    check(!str3.is_literal(), "SSO copy is not literal");
    check(str3 == "Goodbye!", "SSO copy content");

    // Only literals longer than the SSO buffer keep pointing at static storage.
    cow::cow_string long_literal = "Hello, World! I am a COW"_cows;
    check(long_literal.is_literal(), "long literal binds static storage");
    cow::cow_string long_copy = long_literal;
    check(long_copy.is_literal(), "copy of long literal binds static storage");
    check(!long_copy.is_small(), "long literal copy is not SSO");

    cow::cow_string cloned = str.clone();
    check(cloned == str, "clone content");
    check(cloned.is_owned(), "clone of allocated string is owned");

    const cow::cow_string empty;
    check(empty.is_small(), "default string is SSO");
    check(empty.empty(), "default string is empty");

    const cow::cow_string small("abc", 3);
    check(small.is_small(), "pointer+count SSO construction");
    check(small == "abc", "pointer+count content");

    const cow::cow_string literal = "literal";
    check(literal.is_literal(), "short literal constructor binds static storage");
    // Copying it materializes into SSO, since the text fits the SSO buffer.
    const cow::cow_string literal_copy = literal;
    check(literal_copy.is_small(), "copy of short literal uses SSO");
    check(literal_copy.cur_count() == 0, "SSO copy has no refcount");

    // --- Substring sharing ---
    // A substring larger than the SSO buffer aliases the source allocation:
    // both objects point into the same buffer and share one reference count.
    {
        cow::cow_string source(64, 's');
        check(source.is_owned(), "substring source starts owned");
        check_equal(source.cur_count(), 1, "substring source refcount one");
        const std::size_t source_cap = source.capacity();

        cow::cow_string shared = source.substr(10, 40);
        check(source.is_shared(), "source becomes shared after substr");
        check(shared.is_shared(), "long substring shares source");
        check_equal(source.cur_count(), 2, "source refcount two after substr");
        check_equal(shared.cur_count(), 2, "substring refcount two");
        check_equal(shared.size(), 40, "substring size");
        check(std::as_const(shared).data() == std::as_const(source).data() + 10,
              "substring aliases source buffer at its offset");
        check_equal(shared.capacity(), source_cap - 10,
                    "substring capacity counts only the tail");
        check(shared == source.subview(10, 40), "substring content");

        // Fitting the SSO buffer is cheaper than sharing, so the source is
        // left untouched and keeps its single reference.
        cow::cow_string small_sub = source.substr(0, 5);
        check(small_sub.is_small(), "short substring uses SSO");
        check(source.is_shared(), "short substring does not detach source");
        check_equal(source.cur_count(), 2, "short substring keeps refcount two");
        check_equal(small_sub.cur_count(), 0, "SSO substring has no refcount");
        check_equal(small_sub.capacity(), cow::cow_string().capacity(),
                    "SSO substring capacity is the SSO buffer");

        // Mutating the substring detaches it from the shared allocation.
        shared.push_back('!');
        check(shared.is_owned(), "mutating substring detaches it");
        check_equal(shared.cur_count(), 1, "detached substring refcount one");
        check_equal(shared.size(), 41, "detached substring grew by one");
        check_equal(source.cur_count(), 1, "source refcount drops after detach");
        check(source.is_shared(), "source keeps stale shared bit until it mutates");
        check(source == source.subview(0, 64), "source content unchanged");
    }

    // Mutating the source detaches it instead, leaving the substring as the
    // only owner of the old allocation.
    {
        cow::cow_string source(64, 'p');
        cow::cow_string sub = source.substr(10, 40);
        check_equal(source.cur_count(), 2, "source and substring share one allocation");

        source.push_back('?');
        check(source.is_owned(), "mutating source detaches it from the substring");
        check(sub.is_shared(), "substring keeps the old allocation");
        check_equal(sub.cur_count(), 1, "substring is the only owner after source detach");
        check_equal(sub.size(), 40, "substring size unchanged by source detach");
        check(sub == std::string(40, 'p'), "substring keeps the old content");
        check(source == source.subview(0, 65), "source content after detach");
    }

    // Nested substrings keep sharing the same allocation and each holds a
    // reference, so the count grows with the number of live views.
    {
        cow::cow_string source(64, 'n');
        cow::cow_string sub1 = source.substr(0, 40);
        cow::cow_string sub2 = sub1.substr(5, 30);
        check_equal(source.cur_count(), 3, "nested substring refcount three");
        check(sub1.is_shared() && sub2.is_shared(), "nested substrings share");
        check(std::as_const(sub2).data() == std::as_const(source).data() + 5,
              "nested substring offset is relative to the source");
        check_equal(sub2.capacity(), source.capacity() - 5,
                    "nested substring capacity counts the source tail");

        // Dropping one view releases its reference but leaves the others.
        sub1.clear();
        check(sub1.is_small(), "cleared substring becomes SSO");
        check_equal(source.cur_count(), 2, "refcount drops when a view is released");
        check(sub2.is_shared(), "remaining substring still shares");
        check(sub2 == source.subview(5, 30), "remaining substring content");
    }

    // Substrings of a literal stay static when long and copy into SSO when short.
    {
        cow::cow_string literal =
            "Hello, World! I am a COW string. Nice to meet you!"_cows;
        check(literal.is_literal(), "substring literal source is literal");

        cow::cow_string lit_sub = literal.substr(0, 40);
        check(lit_sub.is_literal(), "long substring of literal stays literal");
        check_equal(lit_sub.cur_count(), 0, "literal substring has no refcount");
        check(lit_sub == literal.subview(0, 40), "literal substring content");

        cow::cow_string lit_short = literal.substr(0, 5);
        check(lit_short.is_small(), "short substring of literal uses SSO");
        check(literal.is_literal(), "literal source unchanged by substr");
    }

    // Moving out of a shared string transfers the view without copying and
    // leaves the donor empty in SSO state.
    {
        cow::cow_string source(64, 'm');
        cow::cow_string donor = source;
        check_equal(donor.cur_count(), 2, "move donor refcount two");

        cow::cow_string moved = std::move(donor).substr(0, 30);
        check(moved.is_shared(), "moved substring keeps sharing");
        check_equal(moved.cur_count(), 2, "moved substring leaves refcount two");
        check_equal(moved.capacity(), source.capacity(),
                    "moved substring reuses the whole allocation");
        check(std::as_const(moved).data() == std::as_const(source).data(),
              "moved substring aliases source at offset zero");
        check(donor.is_small(), "moved-from donor becomes SSO");
        check(donor.empty(), "moved-from donor is empty");
        check_equal(source.cur_count(), 2, "source still shared with moved substring");
    }

    check(sizeof(cow::cow_string) == sizeof(cow::cow_wstring),
          "char/wchar size");
    check(sizeof(cow::cow_string) == sizeof(cow::cow_u8string),
          "char/u8 size");
    check(sizeof(cow::cow_string) == sizeof(cow::cow_u16string),
          "char/u16 size");
    check(sizeof(cow::cow_string) == sizeof(cow::cow_u32string),
          "char/u32 size");

    if (failures != 0) {
        std::println("[state] FAILED: cases={} failures={}", cases, failures);
        return 1;
    }
    std::println("[state] PASS: cases={}", cases);
    return 0;
}
