// API coverage smoke test: calls every public entry point of the cow_string
// API at least once -- every constructor, accessor, search, comparison,
// modifier, every replace overload, the free operators, the literals, and the
// std::hash / std::formatter specialisations.
//
// This is deliberately shallow: it exists so that no public API can silently
// rot (an untested overload is never instantiated and therefore never even
// compiled). Deleted overloads (nullptr constructors / assignment) cannot be
// called and are intentionally absent.
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <forward_list>
#include <functional>
#include <iterator>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Contiguous range whose value type is not char but is convertible to it.
struct not_char {
    constexpr operator char() const noexcept { return '7'; }
};

// ---------------------------------------------------------------- construction
void test_constructors() {
    const S defaulted;
    check(defaulted.empty(), "ctor()");

    const S with_alloc(std::allocator<char>{});
    check(with_alloc.empty(), "ctor(alloc)");

    const S repeated(3, 'a');
    check(repeated == "aaa", "ctor(count,ch)");

    const SV letters = "abc";
    const S from_iter(letters.begin(), letters.end());
    check(from_iter == "abc", "ctor(iter,sent)");

    const std::vector<char> vec{'a', 'b'};
    const S from_range(std::from_range, vec);
    check(from_range == "ab", "ctor(from_range)");

    const std::array<not_char, 4> converted{};
    const S from_converted_iter(converted.begin(), converted.end());
    check(from_converted_iter == "7777", "ctor(iter,sent,convertible contiguous)");
    const S from_converted_range(std::from_range, converted);
    check(from_converted_range == "7777", "ctor(from_range,convertible contiguous)");

    const S ptr_count("abcd", 2);
    check(ptr_count == "ab", "ctor(ptr,count)");

    const S copied(repeated);
    check(copied == "aaa", "ctor(copy)");

    const S copied_alloc(repeated, std::allocator<char>{});
    check(copied_alloc == "aaa", "ctor(copy,alloc)");

    const S big(40, 'z');
    const S sub_pos(big, 1);
    check_equal(sub_pos.size(), 39, "ctor(copy,pos)");
    const S sub_pos_count(big, 1, 2);
    check(sub_pos_count == "zz", "ctor(copy,pos,count)");

    S moved_src(3, 'm');
    const S moved(std::move(moved_src));
    check(moved == "mmm", "ctor(move)");
    S moved_pos_src(40, 'n');
    const S moved_pos(std::move(moved_pos_src), 1);
    check_equal(moved_pos.size(), 39, "ctor(move,pos)");
    S moved_count_src(40, 'o');
    const S moved_count(std::move(moved_count_src), 1, 2);
    check(moved_count == "oo", "ctor(move,pos,count)");

    const S from_list{'a', 'b'};
    check(from_list == "ab", "ctor(init_list)");

    const S from_literal("literal");
    check(from_literal == "literal", "ctor(string literal)");

    const S from_svlike(SV("svlike"));
    check(from_svlike == "svlike", "ctor(StringViewLike)");
    const S from_svlike_alloc(SV("svlike"), std::allocator<char>{});
    check(from_svlike_alloc == "svlike", "ctor(StringViewLike,alloc)");

    constexpr SV six = "abcdef";
    const S sv_pos_count(six, 1, 2);
    check(sv_pos_count == "bc", "ctor(sv,pos,count)");
    const S sv_pos(six, 3);
    check(sv_pos == "def", "ctor(sv,pos)");
}

// ------------------------------------------------------ status and accessors
void test_accessors() {
    const S small(3, 'x');
    check(small.is_small(), "is_small");
    check(!small.is_literal() && !small.is_owned() && !small.is_shared(),
          "small is neither literal/owned/shared");
    check_equal(small.cur_count(), 0, "cur_count of SSO");
    check(small.exclusive(), "exclusive SSO");

    const S literal("literal");
    check(literal.is_literal(), "is_literal");

    const S owned(40, 'y');
    check(owned.is_owned(), "is_owned");
    check(owned.exclusive(), "exclusive owned");

    const S shared_src(40, 'z');
    const S shared_copy = shared_src;
    check(shared_src.is_shared() && shared_copy.is_shared(), "is_shared");
    check_equal(shared_src.cur_count(), 2, "cur_count shared");
    check(!shared_copy.exclusive(), "shared is not exclusive");

    check(S::max_size() > 0, "max_size");
    check_equal(S::npos, static_cast<std::size_t>(-1), "npos");

    const S s("Hello, World!");
    check_equal(s.size(), s.length(), "size/length");
    check(!s.empty(), "empty");
    check(s.view() == SV("Hello, World!"), "view");
    check(s.string() == std::string("Hello, World!"), "string");

    const SV implicit = s;
    check(implicit == s.view(), "operator string_view");

    const S cloned = s.clone();
    check(cloned == s, "clone");

    check(s.data() != nullptr, "data() const");
    check(s.begin() != s.end(), "begin/end const");
    check(s.cbegin() != s.cend(), "cbegin/cend");
    check(*s.rbegin() == '!', "rbegin");
    check_equal(std::size_t(s.rend() - s.rbegin()), s.size(), "rend");
    check(*s.crbegin() == '!', "crbegin");
    check(s.crend() != s.crbegin(), "crend");
    // Iterator arithmetic: only n + it, it + n and it - it were used before,
    // so make sure the members that walk backwards are instantiated too.
    auto backwards = s.begin() + 5;
    backwards -= 2;
    check(*backwards == 'l', "iterator operator-=(n)");
    check(*(s.begin() + 5) == ',', "iterator operator+(n)");
    check(*(2 + s.begin()) == 'l', "iterator operator+(n, it)");
    check(*(s.end() - 2) == 'd', "iterator operator-(n)");
    check_equal(std::size_t(s.end() - s.begin()), s.size(), "iterator operator-(it)");
    check(s.begin()[1] == 'e', "iterator operator[](n)");
    // Relational/equality operators of the iterator (both directions of <=>).
    auto lo = s.begin();
    auto hi = s.begin() + 1;
    check(lo < hi && lo <= hi, "iterator less/less-equal");
    check(hi > lo && hi >= lo, "iterator greater/greater-equal");
    check(lo != hi && !(lo == hi), "iterator equality");
    check(!(hi < lo) && !(lo > hi), "iterator negative comparison");
    check(*std::prev(s.end()) == '!', "iterator is a contiguous iterator");
    check(s.front() == 'H' && s.back() == '!', "front/back const");
    check(s[0] == 'H', "operator[] const");
    check(s.at(1) == 'e', "at const");
    // A literal is not writable, so it reports zero capacity by design; the
    // capacity queries are exercised on an owned string instead.
    check_equal(s.capacity(), 0, "capacity of literal");
    check_equal(s.real_capacity(), 0, "real_capacity of literal");
    const S owned_str(40, 'c');
    check(owned_str.capacity() >= owned_str.size(), "capacity");
    check(owned_str.real_capacity() >= owned_str.capacity(), "real_capacity");
    check(s.get_allocator() == std::allocator<char>{}, "get_allocator");
}

// --------------------------------------------------------------------- search
void test_search() {
    const S s("abcabc");

    check_equal(s.find(SV("bc")), 1, "find(sv)");
    check_equal(s.find('b'), 1, "find(ch)");
    check_equal(s.find("bc", 2, 2), 4, "find(ptr,pos,count)");

    check_equal(s.rfind(SV("bc")), 4, "rfind(sv)");
    check_equal(s.rfind('b'), 4, "rfind(ch)");
    check_equal(s.rfind("bc", 5, 2), 4, "rfind(ptr,pos,count)");

    check_equal(s.find_first_of(SV("cb")), 1, "find_first_of(sv)");
    check_equal(s.find_first_of('c'), 2, "find_first_of(ch)");
    check_equal(s.find_first_of("cb", 0, 2), 1, "find_first_of(ptr,pos,count)");

    check_equal(s.find_first_not_of(SV("ab")), 2, "find_first_not_of(sv)");
    check_equal(s.find_first_not_of('a'), 1, "find_first_not_of(ch)");
    check_equal(s.find_first_not_of("ab", 0, 2), 2,
                "find_first_not_of(ptr,pos,count)");

    check_equal(s.find_last_of(SV("ab")), 4, "find_last_of(sv)");
    check_equal(s.find_last_of('a'), 3, "find_last_of(ch)");
    check_equal(s.find_last_of("ab", S::npos, 2), 4,
                "find_last_of(ptr,pos,count)");

    check_equal(s.find_last_not_of(SV("bc")), 3, "find_last_not_of(sv)");
    check_equal(s.find_last_not_of('c'), 4, "find_last_not_of(ch)");
    check_equal(s.find_last_not_of("bc", S::npos, 2), 3,
                "find_last_not_of(ptr,pos,count)");

    check(s.starts_with(SV("abc")) && s.starts_with('a'), "starts_with");
    check(s.ends_with(SV("abc")) && s.ends_with('c'), "ends_with");
    check(s.contains(SV("bc")) && s.contains('b'), "contains");
}

// ------------------------------------------------------------------- compare
void test_compare() {
    const S a("abc");
    const S b("abd");

    check(a.compare(b) < 0, "compare(StringViewLike)");
    check(a.compare(SV("abc")) == 0, "compare(sv)");
    check(a.compare(0, 3, SV("abc")) == 0, "compare(pos,count,sv)");
    check(a.compare(0, 2, SV("ab"), 0, 2) == 0,
          "compare(pos1,count1,v,pos2,count2)");
    check(a.compare(0, 2, "ab", 2) == 0, "compare(pos1,count1,cstr,count2)");

    check(a == a && !(a == b), "operator==(cow)");
    check(!(a != a) && a != b, "operator!=(cow)");
    check((a <=> b) < 0 && (b <=> a) > 0, "operator<=>(cow)");
    check(a == SV("abc"), "operator==(StringViewLike)");
    check((a <=> SV("abd")) < 0, "operator<=>(StringViewLike)");
}

// ------------------------------------------------------- copy, subview, substr
void test_slices() {
    const S a("abc");

    std::array<char, 8> buffer{};
    check_equal(a.copy(buffer.data(), 2), 2, "copy(dest,count)");
    check_equal(a.copy(buffer.data(), 2, 1), 2, "copy(dest,count,pos)");

    check(a.subview(1, 2) == SV("bc"), "subview(pos,count)");
    check(a.subview() == SV("abc"), "subview()");
    check(a.substr(1) == "bc", "substr(pos)");
    check(a.substr(0, 2) == "ab", "substr(pos,count)");

    S movable("abcd");
    check(std::move(movable).substr(1, 2) == "bc", "substr(pos,count) &&");
}

// ---------------------------------------------------------------- assignment
void test_assignment() {
    S s;

    s = SV("abc");
    check(s == "abc", "operator=(StringViewLike)");
    s.assign(SV("abc"));
    check(s == "abc", "assign(StringViewLike)");

    s = 'x';
    check(s == "x", "operator=(ch)");
    s.assign('y');
    check(s == "y", "assign(ch)");

    s = {'a', 'b'};
    check(s == "ab", "operator=(init_list)");
    s.assign({'c', 'd'});
    check(s == "cd", "assign(init_list)");

    const SV range = "abcdef";
    s.assign(range.begin(), range.end());
    check(s == "abcdef", "assign(iter,sent)");
    s.assign(3, 'q');
    check(s == "qqq", "assign(count,ch)");
    s.assign("ab", 2);
    check(s == "ab", "assign(ptr,count)");

    const S other("zz");
    s.assign(other);
    check(s == "zz", "assign(cow const&)");
    s = other;
    check(s == "zz", "operator=(cow const&)");
    s.assign(S(SV("tmp")));
    check(s == "tmp", "assign(cow&&)");
    s = S(SV("tmp2"));
    check(s == "tmp2", "operator=(cow&&)");
    s.assign(other, 0, 1);
    check(s == "z", "assign(cow,pos,count)");
    s.assign(SV("xyz"), 1, 2);
    check(s == "yz", "assign(sv,pos,count)");

    const std::vector<char> vec{'a', 'b'};
    s.assign_range(vec);
    check(s == "ab", "assign_range");
}

// ------------------------------------------------------ capacity and mutators
void test_modifiers() {
    S s("abc");
    s.clear();
    check(s.empty(), "clear");

    s = S(40, 'x');
    s.reserve(100);
    check(s.capacity() >= 100, "reserve");
    s.shrink_to_fit();
    check(s.capacity() >= s.size(), "shrink_to_fit");
    s.ensure_exclusive();
    check(s.exclusive(), "ensure_exclusive");

    S w(3, 'a');
    check(*w.data() == 'a', "data()");
    check(w.begin() != w.end(), "begin/end");
    check_equal(std::size_t(w.end() - w.begin()), 3, "end()");
    auto w_backwards = w.end();
    w_backwards -= 2;
    check(*w_backwards == 'a', "mutable iterator operator-=(n)");
    check(*(w.end() - 3) == *(w.begin()), "mutable iterator operator-(n)");
    check(*(2 + w.begin()) == *(w.begin() + 2), "mutable iterator operator+(n, it)");
    check(w.begin() + 1 > w.begin(), "iterator comparison");
    check(*w.rbegin() == 'a', "rbegin()");
    check(w.rend() != w.rbegin(), "rend()");
    check(w.front() == 'a' && w.back() == 'a', "front()/back()");
    w[0] = 'b';
    check(w[0] == 'b', "operator[] mutable");
    w.at(1) = 'c';
    check(w.at(1) == 'c', "at mutable");

    w.resize_and_overwrite(2, [](char* p, std::size_t n) {
        p[0] = 'x';
        p[1] = 'y';
        return n;
    });
    check(w == "xy", "resize_and_overwrite");
    w.resize(4, 'z');
    check(w == "xyzz", "resize");
    w.push_back('!');
    check(w.back() == '!', "push_back");
    w.pop_back();
    check(w == "xyzz", "pop_back");

    S a("abc");
    a.append(2, 'p');
    a.append(SV("qr"));
    a.append(SV("mnop"), 0, 2);
    a.append("st", 2);
    a.append(SV("uv").begin(), SV("uv").end());
    a.append({'w', 'x'});
    a.append_range(std::vector<char>{'y', 'z'});
    a += SV("!");
    a += '?';
    a += {'+', '='};
    check(a == "abcppqrmnstuvwxyz!?+=", "append/operator+= overloads");

    S i("abc");
    i.insert(1, 2, 'x');
    i.insert(0, 'z');
    i.insert(1, SV("QQ"));
    i.insert(1, SV("wxyz"), 1, 2);
    i.insert(0, "AB", 1);
    i.insert(0, SV("cd").begin(), SV("cd").end());
    i.insert(0, {'K'});
    check(i.size() > 3, "insert(index,...) overloads");

    S it("abc");
    it.insert(it.begin(), 2, 'x');
    it.insert(it.end(), 'y');
    it.insert(it.begin(), SV("ab").begin(), SV("ab").end());
    it.insert(it.end(), {'z'});
    it.insert_range(it.begin(), std::vector<char>{'1', '2'});
    check(it.size() > 3, "insert(const_iterator,...) overloads");

    S e("abcdef");
    e.erase();
    check(e.empty(), "erase()");

    S e2("abcdef");
    e2.erase(1, 2);
    check(e2 == "adef", "erase(index,count)");

    S e3("abcdef");
    auto e3_it = e3.erase(e3.begin());
    check(e3 == "bcdef" && e3_it == e3.begin(), "erase(pos)");

    S e4("abcdef");
    auto e4_it = e4.erase(e4.begin() + 1, e4.begin() + 3);
    check(e4 == "adef" && e4_it == e4.begin() + 1, "erase(first,last)");

    S x("abc"), y("def");
    x.swap(y);
    check(x == "def" && y == "abc", "swap member");
    swap(x, y);
    check(x == "abc" && y == "def", "swap free");
}

// Every replace overload gets its own fresh string.
void test_replace_overloads() {
    {
        S s("abcdef");
        s.replace(1, 2, SV("XY"));
        check(s == "aXYdef", "replace(pos,count,sv)");
    }
    {
        S s("abcdef");
        s.replace(1, 2, SV("wxyz"), 1, 2);
        check(s == "axydef", "replace(pos,count,sv,pos2,count2)");
    }
    {
        S s("abcdef");
        s.replace(s.cbegin(), s.cbegin() + 1, SV("Q"));
        check(s == "Qbcdef", "replace(first,last,sv)");
    }
    {
        S s("abcdef");
        s.replace(1, 2, "AB", 2);
        check(s == "aABdef", "replace(pos,count,cstr,count2)");
    }
    {
        S s("abcdef");
        s.replace(s.cbegin(), s.cbegin() + 1, "C", 1);
        check(s == "Cbcdef", "replace(first,last,cstr,count2)");
    }
    {
        S s("abcdef");
        s.replace(1, 2, 3, 'x');
        check(s == "axxxdef", "replace(pos,count,count2,ch)");
    }
    {
        S s("abcdef");
        s.replace(s.cbegin(), s.cbegin() + 1, 2, 'y');
        check(s == "yybcdef", "replace(first,last,count2,ch)");
    }
    {
        S s("abcdef");
        s.replace(1, 2, SV("cd").begin(), SV("cd").end());
        check(s == "acddef", "replace(pos,count,Iter,Sent)");
    }
    {
        S s("abcdef");
        s.replace(s.cbegin(), s.cbegin() + 1, SV("ef").begin(), SV("ef").end());
        check(s == "efbcdef", "replace(first,last,Iter,Sent)");
    }
    {
        S s("abcdef");
        s.replace(1, 2, {'g', 'h'});
        check(s == "aghdef", "replace(pos,count,init_list)");
    }
    {
        S s("abcdef");
        s.replace(s.cbegin(), s.cbegin() + 1, {'i'});
        check(s == "ibcdef", "replace(first,last,init_list)");
    }
    {
        S s("abcdef");
        s.replace_with_range(1, 2, std::vector<char>{'j'});
        check(s == "ajdef", "replace_with_range(pos,count,R)");
    }
    {
        S s("abcdef");
        s.replace_with_range(s.cbegin(), s.cbegin() + 1, std::vector<char>{'k'});
        check(s == "kbcdef", "replace_with_range(first,last,R)");
    }
}

// ---------------------------------------- iterator / sentinel pair overloads
// Every interface that takes an iterator/sentinel pair has to decide how to
// consume it. The pairs below all describe the same three characters, "xyz",
// but each one selects a different path:
//
//   string-like : contiguous, pointer convertible to const char *
//                 -> handed to the pointer overload, no buffer involved
//   foreign     : contiguous, but the pointer is not const char *
//                 -> buffered, although it is contiguous
//   forward     : forward iterator whose sentinel does not know the distance
//                 -> the distance is walked, then buffered
//   counted     : input iterator whose sentinel does know the distance
//                 -> the distance comes from the sentinel, then buffered
//   input       : plain single pass input iterator
//                 -> appended a chunk at a time
enum class shape { string_like, foreign, forward, counted, input };

template<shape Shape, typename Fn>
void with_source(std::string_view name, Fn&& fn) {
    if constexpr (Shape == shape::string_like) {
        static constexpr std::array<char, 3> data{'x', 'y', 'z'};
        fn(data.begin(), data.end(), name);
    } else if constexpr (Shape == shape::foreign) {
        static constexpr std::array<unsigned char, 3> data{'x', 'y', 'z'};
        fn(data.begin(), data.end(), name);
    } else if constexpr (Shape == shape::forward) {
        static const std::forward_list<char> data{'x', 'y', 'z'};
        fn(data.begin(), data.end(), name);
    } else if constexpr (Shape == shape::counted) {
        std::istringstream stream("xyz");
        fn(std::counted_iterator(std::istreambuf_iterator<char>(stream), 3),
           std::default_sentinel, name);
    } else {
        std::istringstream stream("xyz");
        fn(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}, name);
    }
}

template<typename Fn>
void for_each_shape(Fn&& fn) {
    with_source<shape::string_like>("string-like iterator", fn);
    with_source<shape::foreign>("foreign contiguous iterator", fn);
    with_source<shape::forward>("forward iterator", fn);
    with_source<shape::counted>("counted input iterator", fn);
    with_source<shape::input>("plain input iterator", fn);
}

void test_iterator_shape_overloads() {
    // Constructor from an iterator range.
    for_each_shape([](auto first, auto last, std::string_view shape) {
        const S s(first, last);
        check(s.view() == SV("xyz"), std::format("ctor(iter, sent) from {}", shape));
    });

    // assign(iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("12345");
        s.assign(first, last);
        check(s.view() == SV("xyz"), std::format("assign(iter, sent) from {}", shape));
    });

    // append(iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("abc");
        s.append(first, last);
        check(s.view() == SV("abcxyz"), std::format("append(iter, sent) from {}", shape));
    });

    // insert(size_type, iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("abc");
        s.insert(1, first, last);
        check(s.view() == SV("axyzbc"), std::format("insert(pos, iter, sent) from {}", shape));
    });

    // insert(const_iterator, iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("abc");
        s.insert(s.cbegin() + 1, first, last);
        check(s.view() == SV("axyzbc"), std::format("insert(iterator, iter, sent) from {}", shape));
    });

    // replace(size_type, size_type, iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("abc");
        s.replace(1, 1, first, last);
        check(s.view() == SV("axyzc"),
              std::format("replace(pos, count, iter, sent) from {}", shape));
    });

    // replace(const_iterator, const_iterator, iter, sent)
    for_each_shape([](auto first, auto last, std::string_view shape) {
        S s("abc");
        s.replace(s.cbegin() + 1, s.cbegin() + 2, first, last);
        check(s.view() == SV("axyzc"),
              std::format("replace(iterator, iterator, iter, sent) from {}", shape));
    });
}

// ------------------------------------------------------------ free functions
void test_free_functions() {
    const S left("ab");
    const S right("cd");

    check((left + right) == "abcd", "operator+(cow,cow)");
    check((left + SV("cd")) == "abcd", "operator+(cow,StringViewLike)");
    check((SV("ab") + right) == "abcd", "operator+(StringViewLike,cow)");
    check((left + 'c') == "abc", "operator+(cow,ch)");
    check(('z' + left) == "zab", "operator+(ch,cow)");

    using namespace cow::literals;
    check("abc"_cows == S("abc"), "operator\"\"_cows(char)");
    const auto wide = L"abc"_cows;
    check(wide.size() == 3 && wide[0] == L'a', "operator\"\"_cows(wchar_t)");
    const auto u8 = u8"abc"_cows;
    check(u8.size() == 3 && u8[0] == u8'a', "operator\"\"_cows(char8_t)");
    const auto u16 = u"abc"_cows;
    check(u16.size() == 3 && u16[0] == u'a', "operator\"\"_cows(char16_t)");
    const auto u32 = U"abc"_cows;
    check(u32.size() == 3 && u32[0] == U'a', "operator\"\"_cows(char32_t)");

    const std::hash<S> hasher;
    check_equal(hasher(S("abc")),
                std::hash<SV>{}(SV("abc")), "std::hash");

    check(std::format("{}", S("abc")) == "abc", "std::formatter");
    check(std::format("{:>5}", S("ab")) == "   ab",
          "std::formatter with format spec");
}

// ------------------------------------------- compile-time-only entry points
consteval bool test_consteval_api() {
    const cow::cow_string s = "abc";
    // compare(literal_helper) and compare(pos,count,literal_helper)
    if (s.compare("abc") != 0 || s.compare(0, 3, "abc") != 0) {
        return false;
    }
    if (!s.is_literal() || s.is_small()) {
        return false;
    }
    // consteval construction from a literal goes through literal_helper too.
    const cow::cow_string from_literal = "xyz";
    return from_literal.compare("xyz") == 0;
}
static_assert(test_consteval_api());

} // namespace

int main() {
    test_constructors();
    test_accessors();
    test_search();
    test_compare();
    test_slices();
    test_assignment();
    test_modifiers();
    test_replace_overloads();
    test_iterator_shape_overloads();
    test_free_functions();

    if (failures != 0) {
        std::println("[api] FAILED: cases={} failures={}", cases, failures);
        return 1;
    }
    std::println("[api] PASS: cases={}", cases);
    return 0;
}
