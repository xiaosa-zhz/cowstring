// Compile-time conformance checks.  In-place mutators are deliberately not
// usable in constant evaluation, so only construction, queries and copies are
// covered here.
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <ranges>
#include <string_view>

using namespace cow::literals;

static_assert(cow::cow_string("abc") == "abc");
static_assert(cow::cow_string("abc") < "abd");
static_assert("abc" == cow::cow_string("abc"));

consteval bool test_construction() {
    const cow::cow_string empty;
    if (!empty.empty() || empty.size() != 0) {
        return false;
    }

    const cow::cow_string literal = "abc";
    if (literal.size() != 3 || literal[1] != 'b' || !literal.is_literal()) {
        return false;
    }

    const cow::cow_string repeated(5, 'x');
    if (repeated != "xxxxx" || repeated.size() != 5 || !repeated.is_small()) {
        return false;
    }

    constexpr std::string_view sv = "abcdef";
    const cow::cow_string view_str(sv);
    if (view_str != "abcdef" || !view_str.is_small()) {
        return false;
    }
    const cow::cow_string view_part(sv, 2, 3);
    if (view_part != "cde") {
        return false;
    }

    constexpr std::array<char, 4> arr{'w', 'x', 'y', 'z'};
    const cow::cow_string iter_str(arr.begin(), arr.end());
    if (iter_str != "wxyz") {
        return false;
    }
    const cow::cow_string range_str(std::from_range, arr);
    if (range_str != "wxyz") {
        return false;
    }

    const std::initializer_list<char> il{'a', 'b', 'c', 'd'};
    const cow::cow_string il_str(il);
    if (il_str != "abcd") {
        return false;
    }

    const cow::cow_string copy(literal);
    if (copy != literal) {
        return false;
    }
    const cow::cow_string copy_part(literal, 1, 2);
    if (copy_part != "bc") {
        return false;
    }

    cow::cow_string movable = "move";
    const cow::cow_string moved(std::move(movable));
    if (moved != "move") {
        return false;
    }

    const cow::cow_string long_literal =
        "0123456789012345678901234567890123456789";
    if (long_literal.size() != 40 || !long_literal.is_literal()
        || long_literal.front() != '0' || long_literal.back() != '9') {
        return false;
    }

    const cow::cow_string long_repeat(64, 'q');
    if (long_repeat.size() != 64 || long_repeat.front() != 'q'
        || long_repeat.back() != 'q') {
        return false;
    }

    constexpr std::string_view long_view =
        "0123456789012345678901234567890123456789";
    const cow::cow_string long_from_view(long_view);
    if (long_from_view != long_view || !long_from_view.is_literal()) {
        return false;
    }

    std::array<char, 40> long_arr{};
    for (char& ch : long_arr) {
        ch = 'a';
    }
    const cow::cow_string long_from_range(std::from_range, long_arr);
    if (long_from_range.size() != 40 || long_from_range.front() != 'a'
        || long_from_range.back() != 'a') {
        return false;
    }

    return true;
}
static_assert(test_construction());

consteval bool test_access() {
    const cow::cow_string s = "abcdef";
    if (s.size() != 6 || s.length() != 6 || s.empty()) {
        return false;
    }
    if (s.front() != 'a' || s.back() != 'f'
        || s[0] != 'a' || s[5] != 'f' || s.at(2) != 'c') {
        return false;
    }
    if (s.view() != std::string_view("abcdef")
        || std::string_view(s.data(), s.size()) != "abcdef"
        || s.c_str()[s.size()] != '\0') {
        return false;
    }
    if (!std::ranges::equal(s, std::string_view("abcdef"))
        || std::distance(s.begin(), s.end()) != 6
        || *(s.begin() + 2) != 'c') {
        return false;
    }
    if (s.rbegin()[0] != 'f' || s.rbegin()[5] != 'a') {
        return false;
    }

    std::array<char, 4> buf{};
    const std::size_t copied = s.copy(buf.data(), 3, 1);
    if (copied != 3 || std::string_view(buf.data(), 3) != "bcd") {
        return false;
    }
    return true;
}
static_assert(test_access());

consteval bool test_search() {
    const cow::cow_string s = "abcdeabcde";
    if (s.find("cd") != 2 || s.find('c', 3) != 7) {
        return false;
    }
    if (s.find("cd", 0, 2) != 2) {
        return false;
    }
    if (s.rfind("ab") != 5 || s.rfind('a') != 5) {
        return false;
    }
    if (s.find_first_of("xyc") != 2 || s.find_first_of('b') != 1) {
        return false;
    }
    if (s.find_first_not_of("ab") != 2
        || s.find_first_not_of('a') != 1) {
        return false;
    }
    if (s.find_last_of("ab") != 6 || s.find_last_of('c') != 7) {
        return false;
    }
    if (s.find_last_not_of("abcde") != cow::cow_string::npos) {
        return false;
    }
    if (!s.starts_with("abc") || !s.ends_with("cde") || !s.contains("cd")) {
        return false;
    }
    if (!s.starts_with('a') || !s.ends_with('e') || !s.contains('c')) {
        return false;
    }

    const cow::cow_string empty;
    if (empty.find("") != 0 || empty.find('a') != cow::cow_string::npos
        || empty.rfind("") != 0) {
        return false;
    }
    return true;
}
static_assert(test_search());

consteval bool test_compare() {
    const cow::cow_string a = "abc";
    const cow::cow_string b = "abd";

    if (!(a == "abc") || a == b || !(a != b)) {
        return false;
    }
    if (!(a < b) || !(a <= b) || !(b > a) || !(b >= a)) {
        return false;
    }
    if (!("abc" == a) || !("abd" > a)) {
        return false;
    }
    if (!(a == std::string_view("abc"))
        || (a <=> std::string_view("abd")) >= 0) {
        return false;
    }
    if (a.compare(b) >= 0 || b.compare(a) <= 0) {
        return false;
    }
    if (a.compare("abc") != 0
        || a.compare(std::string_view("abc")) != 0) {
        return false;
    }
    if (a.compare(0, 2, b) >= 0) {
        return false;
    }
    if (a.compare(0, 2, b, 0, 2) != 0) {
        return false;
    }
    if (a.compare(0, 2, "ab", 2) != 0) {
        return false;
    }
    if (a != "abc"_cows || "abc"_cows != a) {
        return false;
    }
    const auto cmp = a <=> b;
    if (cmp >= 0) {
        return false;
    }
    return true;
}
static_assert(test_compare());

consteval bool test_substrings() {
    const cow::cow_string s = "abcdef";
    if (s.substr(1, 3) != "bcd" || s.substr(2) != "cdef") {
        return false;
    }
    if (s.subview(1, 3) != std::string_view("bcd")
        || s.subview() != std::string_view("abcdef")) {
        return false;
    }
    if (s.clone() != s) {
        return false;
    }
    const cow::cow_string copy(s, 2, 2);
    if (copy != "cd") {
        return false;
    }

    const cow::cow_string long_s =
        "0123456789012345678901234567890123456789";
    if (long_s.substr(1, 10) != "1234567890") {
        return false;
    }
    if (long_s.clone() != long_s) {
        return false;
    }
    return true;
}
static_assert(test_substrings());

consteval bool test_operators() {
    const cow::cow_string a = "abc";
    const cow::cow_string b = "def";

    if (a + b != "abcdef") {
        return false;
    }
    if (a + std::string_view("def") != "abcdef") {
        return false;
    }
    if (std::string_view("abc") + b != "abcdef") {
        return false;
    }
    if (a + 'd' != "abcd" || 'x' + a != "xabc") {
        return false;
    }
    return true;
}
static_assert(test_operators());

consteval bool test_char_types() {
    const cow::cow_wstring w = L"wide";
    if (w.size() != 4 || w[0] != L'w' || w.find(L"de") != 2) {
        return false;
    }
    const cow::cow_u8string u8 = u8"utf8";
    if (u8.size() != 4 || u8[0] != u8'u' || u8.find(u8"tf") != 1) {
        return false;
    }
    const cow::cow_u16string u16 = u"wide";
    if (u16.size() != 4 || u16[0] != u'w' || u16 != u"wide") {
        return false;
    }
    const cow::cow_u32string u32 = U"wide";
    if (u32.size() != 4 || u32[0] != U'w' || u32 != U"wide") {
        return false;
    }
    return true;
}
static_assert(test_char_types());

int main() {
    std::puts("[constexpr] PASS");
    return 0;
}
