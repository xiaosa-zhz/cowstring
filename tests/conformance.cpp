// Conformance checks against std::string for operations whose semantics
// match.  cow_string intentionally requires compile-time string literals for
// its null-terminated pointer APIs, so runtime data is passed as string_view
// or pointer + count here.
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using C = cow::cow_string;
using S = std::string;
using SV = std::string_view;

static_assert(C("abc") == "abc");
static_assert(C("abc") < "abd");

struct results {
    std::size_t cases = 0;
    std::size_t failures = 0;
};

results R;
std::string g_context;

void print_bytes(const char* label, SV s) {
    std::fprintf(stderr, "      %s:", label);
    for (unsigned char ch : s) {
        std::fprintf(stderr, " %02x", unsigned(ch));
    }
    std::fputc('\n', stderr);
}

void report_failure(const char* what) {
    ++R.failures;
    if (R.failures <= 20) {
        std::fprintf(stderr, "FAIL [%s] %s\n", g_context.c_str(), what);
    }
}

void check_same(const C& c, const S& s, const char* what) {
    ++R.cases;
    const bool same = c.size() == s.size()
        && c.view() == SV(s)
        && c.c_str()[c.size()] == '\0';
    if (!same) {
        report_failure(what);
        if (R.failures <= 20) {
            std::fprintf(stderr, "      cow size=%zu std size=%zu\n",
                         c.size(), s.size());
            print_bytes("cow", c.view());
            print_bytes("std", s);
        }
    }
}

void check_size(std::size_t got, std::size_t want, const char* what) {
    ++R.cases;
    if (got != want) {
        report_failure(what);
        if (R.failures <= 20) {
            std::fprintf(stderr, "      got=%zu want=%zu\n", got, want);
        }
    }
}

void check_bool(bool got, bool want, const char* what) {
    ++R.cases;
    if (got != want) {
        report_failure(what);
        if (R.failures <= 20) {
            std::fprintf(stderr, "      got=%d want=%d\n", int(got), int(want));
        }
    }
}

C make(const S& s) {
    return C(s.begin(), s.end());
}

template<typename F>
void pair_op(const S& base, const char* what, F&& f) {
    C c = make(base);
    S s = base;
    g_context = what;
    f(c, s);
    check_same(c, s, what);
}

void test_constructors() {
    const S base("abc\0def", 7);

    {
        C c(base.begin(), base.end());
        check_same(c, base, "ctor(iter,iter)");
    }
    {
        C c;
        check_same(c, S(), "ctor()");
    }
    {
        C c(5, 'x');
        check_same(c, S(5, 'x'), "ctor(count,ch)");
    }
    {
        C c{SV(base)};
        check_same(c, base, "ctor(string_view)");
    }
    {
        C c(SV(base), 2, 3);
        check_same(c, S(SV(base), 2, 3), "ctor(string_view,pos,count)");
    }
    {
        C c(base.data(), 3);
        check_same(c, S(base.data(), 3), "ctor(pointer,count)");
    }
    {
        C c(std::from_range, std::vector<char>{'r', 'a', 'n', 'g', 'e'});
        check_same(c, S(std::from_range, std::vector<char>{'r', 'a', 'n', 'g', 'e'}),
                   "ctor(from_range)");
    }
    {
        const std::initializer_list<char> il{'i', 'l', 'i', 's', 't'};
        C c(il);
        check_same(c, S(il), "ctor(initializer_list)");
    }
    {
        C c1 = make("abcdef");
        C c2(c1);
        check_same(c2, S("abcdef"), "ctor(copy)");
        C c3(c1, 1, 3);
        check_same(c3, S("abcdef", 1, 3), "ctor(copy,pos,count)");
        C c4(std::move(c3));
        check_same(c4, S("bcd"), "ctor(move)");
        C c5(C("abcdef", 6), 2, 2);
        check_same(c5, S("abcdef", 2, 2), "ctor(move,pos,count)");
    }
    {
        C c = "literal";
        check_same(c, S("literal"), "ctor(literal)");
        check_bool(c.is_literal(), true, "literal status");
    }
    {
        C c{S("from-std-string")};
        check_same(c, S("from-std-string"), "ctor(std::string)");
    }
    {
        C c;
        c = SV("assigned-view");
        check_same(c, S("assigned-view"), "operator=(string_view)");
        c = S("assigned-string");
        check_same(c, S("assigned-string"), "operator=(std::string)");
        c = 'q';
        check_same(c, S("q"), "operator=(char)");
        c = std::initializer_list<char>{'x', 'y'};
        check_same(c, S("xy"), "operator=(initializer_list)");
    }
    {
        C c;
        c.assign_range(std::vector<char>{'a', 's', 's', 'i', 'g', 'n'});
        check_same(c, S("assign"), "assign_range");
    }
}

void test_accessors() {
    const S base = "abcdef";
    C c = make(base);

    check_size(c.size(), base.size(), "size");
    check_size(c.length(), base.length(), "length");
    check_bool(c.empty(), base.empty(), "empty");
    check_bool(c.front() == base.front(), true, "front");
    check_bool(c.back() == base.back(), true, "back");
    check_bool(c[2] == base[2], true, "operator[]");
    check_bool(c.at(2) == base.at(2), true, "at");

    char cow_buf[4] = {};
    char std_buf[4] = {};
    check_size(c.copy(cow_buf, 3, 2), base.copy(std_buf, 3, 2), "copy");
    check_bool(std::equal(cow_buf, cow_buf + 3, std_buf), true, "copy content");

    check_bool(SV(c.data(), c.size()) == SV(base), true, "data");
    check_bool(SV(c.c_str(), c.size()) == SV(base), true, "c_str");
    check_bool(c.c_str()[c.size()] == '\0', true, "c_str terminator");

    check_same(C(c.begin(), c.end()), S(base.begin(), base.end()),
               "iterator range");
    check_same(C(c.cbegin(), c.cend()), S(base.cbegin(), base.cend()),
               "const_iterator range");
    check_same(C(c.rbegin(), c.rend()), S(base.rbegin(), base.rend()),
               "reverse_iterator range");
    check_same(C(c.crbegin(), c.crend()), S(base.crbegin(), base.crend()),
               "const_reverse_iterator range");

    check_same(c.substr(1, 3), base.substr(1, 3), "substr");
    check_bool(c.subview(1, 3) == SV(base).substr(1, 3), true, "subview");

    C c2 = make("abcdef");
    C c3 = std::move(c2).substr(1, 3);
    check_same(c3, S("bcd"), "substr(rvalue)");

    C clone = c.clone();
    check_same(clone, base, "clone");
}

void test_find() {
    const S haystacks[] = {
        "", "abcde", "abcdeabcde", "abcdeabcdeabcdeabcde",
        "aaaa", "ababab", "0123456789",
    };
    const S needles[] = {
        "", "a", "abc", "abcde", "abcdeabcde", "z", "ab",
    };

    for (const S& h : haystacks) {
        C c = make(h);
        for (std::size_t pos = 0; pos <= h.size() + 2; ++pos) {
            g_context = "find haystack=" + h + " pos=" + std::to_string(pos);
            for (const S& n : needles) {
                check_size(c.find(SV(n), pos), h.find(SV(n), pos), "find(sv,pos)");
                check_size(c.find(n.data(), pos, n.size()),
                           h.find(n.data(), pos, n.size()), "find(ptr,pos,count)");
                check_size(c.rfind(SV(n), pos), h.rfind(SV(n), pos), "rfind(sv,pos)");
                check_size(c.rfind(n.data(), pos, n.size()),
                           h.rfind(n.data(), pos, n.size()), "rfind(ptr,pos,count)");
                check_size(c.find_first_of(SV(n), pos),
                           h.find_first_of(SV(n), pos), "find_first_of(sv,pos)");
                check_size(c.find_first_of(n.data(), pos, n.size()),
                           h.find_first_of(n.data(), pos, n.size()),
                           "find_first_of(ptr,pos,count)");
                check_size(c.find_first_not_of(SV(n), pos),
                           h.find_first_not_of(SV(n), pos),
                           "find_first_not_of(sv,pos)");
                check_size(c.find_first_not_of(n.data(), pos, n.size()),
                           h.find_first_not_of(n.data(), pos, n.size()),
                           "find_first_not_of(ptr,pos,count)");
                check_size(c.find_last_of(SV(n), pos),
                           h.find_last_of(SV(n), pos), "find_last_of(sv,pos)");
                check_size(c.find_last_of(n.data(), pos, n.size()),
                           h.find_last_of(n.data(), pos, n.size()),
                           "find_last_of(ptr,pos,count)");
                check_size(c.find_last_not_of(SV(n), pos),
                           h.find_last_not_of(SV(n), pos),
                           "find_last_not_of(sv,pos)");
                check_size(c.find_last_not_of(n.data(), pos, n.size()),
                           h.find_last_not_of(n.data(), pos, n.size()),
                           "find_last_not_of(ptr,pos,count)");
            }
            for (char ch : {'a', 'c', 'z', '\0'}) {
                check_size(c.find(ch, pos), h.find(ch, pos), "find(ch,pos)");
                check_size(c.rfind(ch, pos), h.rfind(ch, pos), "rfind(ch,pos)");
                check_size(c.find_first_of(ch, pos), h.find_first_of(ch, pos),
                           "find_first_of(ch,pos)");
                check_size(c.find_first_not_of(ch, pos),
                           h.find_first_not_of(ch, pos),
                           "find_first_not_of(ch,pos)");
                check_size(c.find_last_of(ch, pos), h.find_last_of(ch, pos),
                           "find_last_of(ch,pos)");
                check_size(c.find_last_not_of(ch, pos),
                           h.find_last_not_of(ch, pos),
                           "find_last_not_of(ch,pos)");
            }
        }

        g_context = "starts/ends/contains haystack=" + h;
        for (const S& n : needles) {
            check_bool(c.starts_with(SV(n)), h.starts_with(SV(n)), "starts_with(sv)");
            check_bool(c.ends_with(SV(n)), h.ends_with(SV(n)), "ends_with(sv)");
            check_bool(c.contains(SV(n)), h.contains(SV(n)), "contains(sv)");
        }
        check_bool(c.starts_with('a'), h.starts_with('a'), "starts_with(ch)");
        check_bool(c.ends_with('e'), h.ends_with('e'), "ends_with(ch)");
        check_bool(c.contains('c'), h.contains('c'), "contains(ch)");
        check_bool(c.starts_with("abc"), h.starts_with("abc"), "starts_with(literal)");
        check_bool(c.ends_with("abc"), h.ends_with("abc"), "ends_with(literal)");
        check_bool(c.contains("abc"), h.contains("abc"), "contains(literal)");
    }
}

int sign_of(int value) {
    return (value > 0) - (value < 0);
}

void test_compare() {
    const S values[] = {
        "", "a", "ab", "abc", "abd", "abcdef", "z",
    };
    const S pointer_source = "xy";

    for (const S& a : values) {
        C ca = make(a);
        for (const S& b : values) {
            C cb = make(b);
            g_context = "compare a=" + a + " b=" + b;
            check_bool((ca == cb) == (a == b), true, "operator==(cow)");
            check_bool((ca != cb) == (a != b), true, "operator!=(cow)");
            check_bool((ca < cb) == (a < b), true, "operator<(cow)");
            check_bool((ca <= cb) == (a <= b), true, "operator<=(cow)");
            check_bool((ca > cb) == (a > b), true, "operator>(cow)");
            check_bool((ca >= cb) == (a >= b), true, "operator>=(cow)");
            check_bool(sign_of(ca.compare(cb)) == sign_of(a.compare(b)),
                       true, "compare(cow)");
            check_bool(sign_of(ca.compare(SV(b))) == sign_of(a.compare(SV(b))),
                       true, "compare(sv)");
            check_bool(sign_of(ca.compare(b)) == sign_of(a.compare(b)),
                       true, "compare(std::string)");
            check_bool((ca == SV(b)) == (a == SV(b)), true, "operator==(sv)");
            check_bool((ca == b) == (a == b), true, "operator==(std::string)");

            for (std::size_t pos : {std::size_t(0), a.size() / 2, a.size()}) {
                for (std::size_t count : {std::size_t(0), std::size_t(1),
                                          a.size() / 2, C::npos}) {
                    g_context = "compare pos a=" + a + " b=" + b
                              + " pos=" + std::to_string(pos)
                              + " count=" + std::to_string(count);
                    check_bool(sign_of(ca.compare(pos, count, SV(b)))
                                   == sign_of(a.compare(pos, count, SV(b))),
                               true, "compare(pos,count,sv)");
                }
            }
            check_bool(sign_of(ca.compare(0, 2, SV(b), 0, 2))
                           == sign_of(a.compare(0, 2, SV(b), 0, 2)),
                       true, "compare(pos,count,sv,pos,count)");
            check_bool(sign_of(ca.compare(0, 2, pointer_source.data(), 2))
                           == sign_of(a.compare(0, 2, pointer_source.data(), 2)),
                       true, "compare(pos,count,ptr,count)");
        }
    }

    C c = make("abc");
    check_bool(c == SV("abc"), true, "operator==(sv)");
    check_bool(sign_of(c.compare("abc")) == 0, true, "compare(literal)");
    check_bool(c == "abc", true, "operator==(literal)");
    check_bool(c != "abd", true, "operator!=(literal)");
    check_bool(c < "abd", true, "operator<(literal)");
    check_bool(c <= "abc", true, "operator<=(literal)");
    check_bool(c > "abb", true, "operator>(literal)");
    check_bool(c >= "abc", true, "operator>=(literal)");
    check_bool("abc" == c, true, "operator==(literal, reversed)");
    check_bool("abd" > c, true, "operator>(literal, reversed)");
    constexpr const char* literal_pointer = "abc";
    check_bool(c == literal_pointer, true, "operator==(constexpr pointer)");
    check_bool(sign_of(c.compare(literal_pointer)) == 0, true,
               "compare(constexpr pointer)");
    using namespace cow::literals;
    check_bool(c == "abc"_cows, true, "operator==(cow literal)");
    check_bool("abc"_cows == c, true, "operator==(cow literal, reversed)");
}

void test_modifiers() {
    const S base = "abcdef";

    pair_op(base, "append(sv)", [](C& c, S& s) {
        c.append(SV("XYZ"));
        s.append(SV("XYZ"));
    });
    pair_op(base, "append(sv,pos,count)", [](C& c, S& s) {
        c.append(SV("012345"), 2, 3);
        s.append(SV("012345"), 2, 3);
    });
    pair_op(base, "append(count,ch)", [](C& c, S& s) {
        c.append(3, 'x');
        s.append(3, 'x');
    });
    pair_op(base, "append(iter,iter)", [](C& c, S& s) {
        const S add = "uvw";
        c.append(add.begin(), add.end());
        s.append(add.begin(), add.end());
    });
    pair_op(base, "append(initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c.append(il);
        s.append(il);
    });
    pair_op(base, "append_range", [](C& c, S& s) {
        const std::vector<char> v{'1', '2'};
        c.append_range(v);
        s.append_range(v);
    });
    pair_op(base, "operator+=(sv)", [](C& c, S& s) {
        c += SV("XYZ");
        s += SV("XYZ");
    });
    pair_op(base, "operator+=(ch)", [](C& c, S& s) {
        c += 'x';
        s += 'x';
    });
    pair_op(base, "operator+=(il)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c += il;
        s += il;
    });

    pair_op(base, "insert(pos,count,ch)", [](C& c, S& s) {
        c.insert(2, 3, 'x');
        s.insert(2, 3, 'x');
    });
    pair_op(base, "insert(pos,ch)", [](C& c, S& s) {
        c.insert(2, 'x');
        s.insert(2, 1, 'x');
    });
    pair_op(base, "insert(pos,sv)", [](C& c, S& s) {
        c.insert(2, SV("XYZ"));
        s.insert(2, SV("XYZ"));
    });
    pair_op(base, "insert(pos,sv,pos,count)", [](C& c, S& s) {
        c.insert(2, SV("012345"), 1, 3);
        s.insert(2, SV("012345"), 1, 3);
    });
    pair_op(base, "insert(pos,ptr,count)", [](C& c, S& s) {
        const S add = "XYZ";
        c.insert(2, add.data(), add.size());
        s.insert(2, add.data(), add.size());
    });
    pair_op(base, "insert(pos,iter,iter)", [](C& c, S& s) {
        const S add = "XYZ";
        c.insert(2, add.begin(), add.end());
        s.insert(s.begin() + 2, add.begin(), add.end());
    });
    pair_op(base, "insert(pos,initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c.insert(2, il);
        s.insert(2, il);
    });

    {
        C c = make(base);
        S s = base;
        g_context = "insert(iter,count,ch)";
        auto ci = c.insert(c.begin() + 2, 3, 'x');
        auto si = s.insert(s.begin() + 2, 3, 'x');
        check_same(c, s, "insert(iter,count,ch)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "insert(iter,count,ch) return");
    }
    {
        C c = make(base);
        S s = base;
        g_context = "insert(iter,ch)";
        auto ci = c.insert(c.begin() + 2, 'x');
        auto si = s.insert(s.begin() + 2, 'x');
        check_same(c, s, "insert(iter,ch)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "insert(iter,ch) return");
    }
    {
        C c = make(base);
        S s = base;
        const S add = "XYZ";
        g_context = "insert(iter,iter,iter)";
        auto ci = c.insert(c.begin() + 2, add.begin(), add.end());
        auto si = s.insert(s.begin() + 2, add.begin(), add.end());
        check_same(c, s, "insert(iter,iter,iter)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "insert(iter,iter,iter) return");
    }
    {
        C c = make(base);
        S s = base;
        const std::initializer_list<char> il{'1', '2'};
        g_context = "insert(iter,initializer_list)";
        auto ci = c.insert(c.begin() + 2, il);
        auto si = s.insert(s.begin() + 2, il);
        check_same(c, s, "insert(iter,initializer_list)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "insert(iter,initializer_list) return");
    }
    {
        C c = make(base);
        S s = base;
        const std::vector<char> v{'1', '2'};
        g_context = "insert_range";
        auto ci = c.insert_range(c.begin() + 2, v);
        auto si = s.insert_range(s.begin() + 2, v);
        check_same(c, s, "insert_range");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "insert_range return");
    }

    pair_op(base, "erase(pos,count)", [](C& c, S& s) {
        c.erase(1, 3);
        s.erase(1, 3);
    });
    pair_op(base, "erase(pos)", [](C& c, S& s) {
        c.erase(2);
        s.erase(2);
    });
    {
        C c = make(base);
        S s = base;
        g_context = "erase(iter)";
        auto ci = c.erase(c.begin() + 2);
        auto si = s.erase(s.begin() + 2);
        check_same(c, s, "erase(iter)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "erase(iter) return");
    }
    {
        C c = make(base);
        S s = base;
        g_context = "erase(iter,iter)";
        auto ci = c.erase(c.begin() + 1, c.begin() + 4);
        auto si = s.erase(s.begin() + 1, s.begin() + 4);
        check_same(c, s, "erase(iter,iter)");
        check_size(std::size_t(ci - c.begin()), std::size_t(si - s.begin()),
                   "erase(iter,iter) return");
    }

    pair_op(base, "replace(pos,count,sv)", [](C& c, S& s) {
        c.replace(1, 3, SV("XYZ"));
        s.replace(1, 3, SV("XYZ"));
    });
    pair_op(base, "replace(pos,count,sv,pos,count)", [](C& c, S& s) {
        c.replace(1, 3, SV("012345"), 2, 2);
        s.replace(1, 3, SV("012345"), 2, 2);
    });
    pair_op(base, "replace(pos,count,ptr,count)", [](C& c, S& s) {
        const S add = "XYZ";
        c.replace(1, 3, add.data(), add.size());
        s.replace(1, 3, add.data(), add.size());
    });
    pair_op(base, "replace(pos,count,count,ch)", [](C& c, S& s) {
        c.replace(1, 3, 2, 'x');
        s.replace(1, 3, 2, 'x');
    });
    pair_op(base, "replace(pos,count,iter,iter)", [](C& c, S& s) {
        const S add = "XYZ";
        c.replace(1, 3, add.begin(), add.end());
        s.replace(s.begin() + 1, s.begin() + 4, add.begin(), add.end());
    });
    pair_op(base, "replace(pos,count,initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c.replace(1, 3, il);
        s.replace(1, 3, il);
    });
    pair_op(base, "replace_with_range", [](C& c, S& s) {
        const std::vector<char> v{'1', '2'};
        c.replace_with_range(1, 3, v);
        s.replace_with_range(s.begin() + 1, s.begin() + 4, v);
    });
    pair_op(base, "replace(iter,iter,sv)", [](C& c, S& s) {
        c.replace(c.begin() + 1, c.begin() + 4, SV("XYZ"));
        s.replace(s.begin() + 1, s.begin() + 4, SV("XYZ"));
    });
    pair_op(base, "replace(iter,iter,count,ch)", [](C& c, S& s) {
        c.replace(c.begin() + 1, c.begin() + 4, 2, 'x');
        s.replace(s.begin() + 1, s.begin() + 4, 2, 'x');
    });
    pair_op(base, "replace(iter,iter,iter,iter)", [](C& c, S& s) {
        const S add = "XYZ";
        c.replace(c.begin() + 1, c.begin() + 4, add.begin(), add.end());
        s.replace(s.begin() + 1, s.begin() + 4, add.begin(), add.end());
    });
    pair_op(base, "replace(iter,iter,initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c.replace(c.begin() + 1, c.begin() + 4, il);
        s.replace(s.begin() + 1, s.begin() + 4, il);
    });
    pair_op(base, "replace_with_range(iter,iter)", [](C& c, S& s) {
        const std::vector<char> v{'1', '2'};
        c.replace_with_range(c.begin() + 1, c.begin() + 4, v);
        s.replace_with_range(s.begin() + 1, s.begin() + 4, v);
    });

    pair_op(base, "assign(sv)", [](C& c, S& s) {
        c.assign(SV("XYZ"));
        s.assign(SV("XYZ"));
    });
    pair_op(base, "assign(count,ch)", [](C& c, S& s) {
        c.assign(4, 'x');
        s.assign(4, 'x');
    });
    pair_op(base, "assign(ptr,count)", [](C& c, S& s) {
        const S add = "XYZ";
        c.assign(add.data(), add.size());
        s.assign(add.data(), add.size());
    });
    pair_op(base, "assign(iter,iter)", [](C& c, S& s) {
        const S add = "XYZ";
        c.assign(add.begin(), add.end());
        s.assign(add.begin(), add.end());
    });
    pair_op(base, "assign(initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c.assign(il);
        s.assign(il);
    });
    pair_op(base, "assign_range", [](C& c, S& s) {
        const std::vector<char> v{'1', '2'};
        c.assign_range(v);
        s.assign_range(v);
    });
    pair_op(base, "assign(cow,pos,count)", [](C& c, S& s) {
        C other = make("012345");
        c.assign(other, 1, 3);
        s.assign(S("012345"), 1, 3);
    });
    pair_op(base, "operator=(sv)", [](C& c, S& s) {
        c = SV("new");
        s = "new";
    });
    pair_op(base, "operator=(std::string)", [](C& c, S& s) {
        c = S("new");
        s = S("new");
    });
    pair_op(base, "operator=(char)", [](C& c, S& s) {
        c = 'q';
        s = 'q';
    });
    pair_op(base, "operator=(initializer_list)", [](C& c, S& s) {
        const std::initializer_list<char> il{'1', '2'};
        c = il;
        s = il;
    });
    pair_op(base, "operator=(literal)", [](C& c, S& s) {
        c = "literal";
        s = "literal";
    });

    pair_op(base, "resize grow", [](C& c, S& s) {
        c.resize(10, 'z');
        s.resize(10, 'z');
    });
    pair_op(base, "resize shrink", [](C& c, S& s) {
        c.resize(3, 'z');
        s.resize(3, 'z');
    });
    pair_op(base, "resize grow default", [](C& c, S& s) {
        c.resize(10);
        s.resize(10);
    });
    pair_op(base, "resize_and_overwrite grow", [](C& c, S& s) {
        const auto op = [](char* p, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                p[i] = static_cast<char>('a' + i);
            }
            return n;
        };
        c.resize_and_overwrite(12, op);
        s.resize_and_overwrite(12, op);
    });
    pair_op(base, "resize_and_overwrite shrink", [](C& c, S& s) {
        const auto op = [](char* p, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                p[i] = static_cast<char>('a' + i);
            }
            return n;
        };
        c.resize_and_overwrite(3, op);
        s.resize_and_overwrite(3, op);
    });
    pair_op(base, "clear", [](C& c, S& s) {
        c.clear();
        s.clear();
    });
    pair_op(base, "push_back", [](C& c, S& s) {
        c.push_back('q');
        s.push_back('q');
    });
    pair_op(base, "pop_back", [](C& c, S& s) {
        c.pop_back();
        s.pop_back();
    });
    pair_op(base, "reserve", [](C& c, S& s) {
        c.reserve(64);
        s.reserve(64);
        check_bool(c.capacity() >= c.size(), true, "capacity after reserve");
    });
    pair_op(base, "shrink_to_fit", [](C& c, S& s) {
        c.shrink_to_fit();
        s.shrink_to_fit();
    });

    {
        C a = make("abc");
        C b = make("defgh");
        S sa = "abc";
        S sb = "defgh";
        g_context = "swap";
        a.swap(b);
        sa.swap(sb);
        check_same(a, sa, "swap a");
        check_same(b, sb, "swap b");
    }
}

void test_nonmembers() {
    C a = make("abc");
    C b = make("def");
    S sa = "abc";
    S sb = "def";

    check_same(a + b, sa + sb, "operator+(cow,cow)");
    S plus_right = sa;
    plus_right.append(S("XYZ"));
    S plus_left = S("XYZ");
    plus_left.append(sa);
    const S add = "XYZ";
    check_same(a + add, plus_right, "operator+(cow,string)");
    check_same(add + a, plus_left, "operator+(string,cow)");
    check_same(a + 'x', sa + 'x', "operator+(cow,char)");
    check_same('x' + a, 'x' + sa, "operator+(char,cow)");

    check_bool(std::hash<C>{}(a) == std::hash<SV>{}(SV(sa)), true, "hash");
}

template<typename F>
bool throws_out_of_range(F&& f) {
    try {
        f();
    } catch (const std::out_of_range&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void test_exceptions() {
    C c = make("abc");
    S s = "abc";

    g_context = "at";
    check_bool(throws_out_of_range([&] { (void)c.at(3); }),
               throws_out_of_range([&] { (void)s.at(3); }), "at throws");
    g_context = "substr";
    check_bool(throws_out_of_range([&] { (void)c.substr(4); }),
               throws_out_of_range([&] { (void)s.substr(4); }), "substr throws");
    g_context = "subview";
    check_bool(throws_out_of_range([&] { (void)c.subview(4); }),
               throws_out_of_range([&] { (void)SV(s).substr(4); }), "subview throws");
    g_context = "insert";
    check_bool(throws_out_of_range([&] { c.insert(4, SV("x")); }),
               throws_out_of_range([&] { s.insert(4, SV("x")); }), "insert throws");
    g_context = "erase";
    check_bool(throws_out_of_range([&] { c.erase(4); }),
               throws_out_of_range([&] { s.erase(4); }), "erase throws");
    g_context = "replace";
    check_bool(throws_out_of_range([&] { c.replace(4, 1, SV("x")); }),
               throws_out_of_range([&] { s.replace(4, 1, SV("x")); }),
               "replace throws");
    g_context = "compare";
    check_bool(throws_out_of_range([&] { (void)c.compare(4, 1, SV("x")); }),
               throws_out_of_range([&] { (void)s.compare(4, 1, SV("x")); }),
               "compare throws");
    g_context = "append";
    check_bool(throws_out_of_range([&] { c.append(SV("012345"), 6, 1); }),
               throws_out_of_range([&] { s.append(SV("012345"), 6, 1); }),
               "append throws");
}

void test_cow_semantics() {
    C lit = "literal";
    check_bool(lit.is_literal(), true, "literal ctor status");
    check_same(lit, S("literal"), "literal ctor content");

    C owned = make(S(64, 'x'));
    check_bool(owned.is_owned(), true, "owned status");

    C shared = owned;
    check_bool(owned.is_shared(), true, "copy source shared");
    check_bool(shared.is_shared(), true, "copy target shared");
    check_size(owned.cur_count(), 2, "copy refcount");

    shared.push_back('y');
    check_same(owned, S(64, 'x'), "copy source unchanged");
    check_same(shared, S(64, 'x') + "y", "copy target mutated");
    check_bool(shared.is_owned(), true, "detached copy owned");

    C cloned = owned.clone();
    check_same(cloned, S(64, 'x'), "clone content");
    check_bool(cloned.is_owned(), true, "clone owned");

    C small = "small";
    C small_copy = small;
    check_bool(small.is_literal() && small_copy.is_literal(), true, "literal copy");
    small_copy.push_back('!');
    check_same(small, S("small"), "literal source unchanged");
    check_same(small_copy, S("small!"), "literal target mutated");
}

void test_random() {
    std::mt19937_64 rng(0xC0FFEE);
    C c;
    S s;

    auto random_text = [&rng](std::size_t max_len) {
        const std::size_t len = static_cast<std::size_t>(rng() % (max_len + 1));
        S text;
        text.reserve(len);
        for (std::size_t i = 0; i < len; ++i) {
            text.push_back(static_cast<char>('a' + (rng() % 26)));
        }
        return text;
    };

    for (std::size_t iteration = 0; iteration < 20000; ++iteration) {
        g_context = "random iteration " + std::to_string(iteration);
        if (s.size() > 256) {
            const std::size_t cut = s.size() / 2;
            c.erase(0, cut);
            s.erase(0, cut);
        }

        switch (rng() % 15) {
        case 0: {
            const S text = random_text(8);
            c.append(SV(text));
            s.append(SV(text));
            break;
        }
        case 1: {
            const char ch = static_cast<char>('a' + (rng() % 26));
            c.push_back(ch);
            s.push_back(ch);
            break;
        }
        case 2: {
            const S text = random_text(8);
            const std::size_t pos = static_cast<std::size_t>(rng() % (s.size() + 1));
            c.insert(pos, SV(text));
            s.insert(pos, SV(text));
            break;
        }
        case 3: {
            const char ch = static_cast<char>('a' + (rng() % 26));
            const std::size_t pos = static_cast<std::size_t>(rng() % (s.size() + 1));
            c.insert(pos, ch);
            s.insert(pos, 1, ch);
            break;
        }
        case 4: {
            const std::size_t pos = s.empty()
                ? 0 : static_cast<std::size_t>(rng() % (s.size() + 1));
            const std::size_t count = static_cast<std::size_t>(
                rng() % (s.size() - pos + 1));
            c.erase(pos, count);
            s.erase(pos, count);
            break;
        }
        case 5: {
            const S text = random_text(8);
            const std::size_t pos = s.empty()
                ? 0 : static_cast<std::size_t>(rng() % (s.size() + 1));
            const std::size_t count = static_cast<std::size_t>(
                rng() % (s.size() - pos + 1));
            c.replace(pos, count, SV(text));
            s.replace(pos, count, SV(text));
            break;
        }
        case 6: {
            const char ch = static_cast<char>('a' + (rng() % 26));
            const std::size_t count = static_cast<std::size_t>(rng() % 6);
            const std::size_t pos = s.empty()
                ? 0 : static_cast<std::size_t>(rng() % (s.size() + 1));
            const std::size_t removed = static_cast<std::size_t>(
                rng() % (s.size() - pos + 1));
            c.replace(pos, removed, count, ch);
            s.replace(pos, removed, count, ch);
            break;
        }
        case 7: {
            const S text = random_text(16);
            c.assign(SV(text));
            s.assign(SV(text));
            break;
        }
        case 8: {
            const char ch = static_cast<char>('a' + (rng() % 26));
            c.assign(ch);
            s.assign(1, ch);
            break;
        }
        case 9: {
            if (!s.empty()) {
                c.pop_back();
                s.pop_back();
            }
            break;
        }
        case 10: {
            const std::size_t size = static_cast<std::size_t>(
                rng() % (s.size() + 9));
            const char ch = static_cast<char>('a' + (rng() % 26));
            c.resize(size, ch);
            s.resize(size, ch);
            break;
        }
        case 11: {
            const std::size_t cap = static_cast<std::size_t>(rng() % 512);
            c.reserve(cap);
            s.reserve(cap);
            break;
        }
        case 12: {
            c.shrink_to_fit();
            s.shrink_to_fit();
            break;
        }
        case 13: {
            C copy = c;
            S std_copy = s;
            copy.append(SV("!"));
            std_copy.append(SV("!"));
            check_same(copy, std_copy, "random copy mutation");
            check_same(c, s, "random copy source");
            break;
        }
        case 14: {
            if ((rng() % 32) == 0) {
                c.clear();
                s.clear();
            } else {
                const char ch = static_cast<char>('a' + (rng() % 26));
                c.push_back(ch);
                s.push_back(ch);
            }
            break;
        }
        }
        check_same(c, s, "random operation");
    }
}

} // namespace

int main() {
    test_constructors();
    test_accessors();
    test_find();
    test_compare();
    test_modifiers();
    test_nonmembers();
    test_exceptions();
    test_cow_semantics();
    test_random();

    if (R.failures != 0) {
        std::printf("[conformance] FAILED: cases=%zu failures=%zu\n",
                    R.cases, R.failures);
        return 1;
    }
    std::printf("[conformance] PASS: cases=%zu\n", R.cases);
    return 0;
}
