// Internal COW-state transition tests
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <cstddef>
#include <print>
#include <string_view>

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
