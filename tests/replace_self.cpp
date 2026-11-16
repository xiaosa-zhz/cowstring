#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <print>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

// Exposes the cow status queries (is_shared()/cur_count()) used below to assert
// that the cross-sharing cases really share one allocation.
#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"
#undef COW_STRING_TEST_STATUS

namespace {

enum class call_mode {
    const_iterator,
    pointer,
    string_view,
    iterator_pair,
    with_range,
};

constexpr call_mode all_modes[] = {
    call_mode::const_iterator,
    call_mode::pointer,
    call_mode::string_view,
    call_mode::iterator_pair,
    call_mode::with_range,
};

const char* mode_name(call_mode mode) {
    switch (mode) {
    case call_mode::const_iterator: return "const_iterator";
    case call_mode::pointer: return "pointer";
    case call_mode::string_view: return "string_view";
    case call_mode::iterator_pair: return "iterator_pair";
    case call_mode::with_range: return "with_range";
    }
    return "unknown";
}

template<typename CharT>
std::basic_string<CharT> make_old(std::size_t n) {
    std::basic_string<CharT> s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<CharT>(CharT('a') + CharT((i * 7 + 3) % 26)));
    }
    return s;
}

template<typename CharT>
std::basic_string<CharT> model(const std::basic_string<CharT>& old, std::size_t pos,
                               std::size_t count, std::size_t source_pos,
                               std::size_t source_len) {
    const std::size_t rcount = std::min(count, old.size() - pos);
    return old.substr(0, pos) + old.substr(source_pos, source_len)
         + old.substr(pos + rcount);
}

template<typename CharT>
void print_sequence(const char* label, const std::basic_string<CharT>& s) {
    std::print(stderr, "      {}:", label);
    for (CharT ch : s) {
        std::print(stderr, " {:x}", static_cast<unsigned long>(ch));
    }
    std::print(stderr, "\n");
}

struct results {
    std::size_t cases = 0;
    std::size_t failures = 0;
};

template<typename CharT>
void check_case(results& result, const std::basic_string<CharT>& old, std::size_t pos,
                std::size_t count, std::size_t source_pos, std::size_t source_len,
                std::size_t cap_request, bool shrink, bool shared, call_mode mode) {
    using string_type = cow::basic_cow_string<CharT>;

    const std::basic_string<CharT> want = model(old, pos, count, source_pos, source_len);

    string_type s(old.begin(), old.end());
    if (cap_request != 0) {
        s.reserve(cap_request);
    }
    if (shrink) {
        s.shrink_to_fit();
    }
    const std::size_t cap = s.capacity();

    string_type keep;
    if (shared) {
        keep = s;
    }

    const string_type& cs = s;
    switch (mode) {
    case call_mode::const_iterator:
        s.replace(pos, count, cs.cbegin() + source_pos,
                  cs.cbegin() + source_pos + source_len);
        break;
    case call_mode::pointer:
        s.replace(pos, count, cs.data() + source_pos,
                  cs.data() + source_pos + source_len);
        break;
    case call_mode::string_view: {
        const std::basic_string_view<CharT> view(cs.data() + source_pos, source_len);
        s.replace(pos, count, view);
        break;
    }
    case call_mode::iterator_pair: {
        const std::size_t rcount = std::min(count, old.size() - pos);
        s.replace(cs.cbegin() + pos, cs.cbegin() + pos + rcount,
                  cs.cbegin() + source_pos, cs.cbegin() + source_pos + source_len);
        break;
    }
    case call_mode::with_range: {
        auto range = std::ranges::subrange(cs.cbegin() + source_pos,
                                           cs.cbegin() + source_pos + source_len);
        s.replace_with_range(pos, count, range);
        break;
    }
    }

    ++result.cases;
    const std::basic_string<CharT> got(s.data(), s.size());
    if (got != want) {
        ++result.failures;
        if (result.failures <= 10) {
            std::println(stderr,
                "FAIL type-size={} pos={} count={} src=[{},{}) cap={} "
                "shrink={} shared={} mode={}",
                old.size(), pos, count, source_pos, source_pos + source_len, cap,
                int(shrink), int(shared), mode_name(mode));
            print_sequence("old ", old);
            print_sequence("got ", got);
            print_sequence("want", want);
        }
    }
}

template<typename CharT>
void run_exhaustive(results& result, std::size_t max_n) {
    using string_type = cow::basic_cow_string<CharT>;
    for (std::size_t n = 0; n <= max_n; ++n) {
        const std::basic_string<CharT> old = make_old<CharT>(n);
        for (std::size_t pos = 0; pos <= n; ++pos) {
            for (std::size_t count : {0uz, 1uz, 2uz,
                                      n, n + 3, string_type::npos}) {
                for (std::size_t source_pos = 0; source_pos <= n; ++source_pos) {
                    for (std::size_t source_len = 0; source_pos + source_len <= n;
                         ++source_len) {
                        for (call_mode mode : all_modes) {
                            check_case(result, old, pos, count, source_pos, source_len,
                                       0, false, false, mode);
                            check_case(result, old, pos, count, source_pos, source_len,
                                       0, false, true, mode);
                        }
                    }
                }
            }
        }
    }
}

template<typename CharT>
void run_selected(results& result) {
    using string_type = cow::basic_cow_string<CharT>;
    for (std::size_t n : {31uz, 64uz, 129uz}) {
        const std::basic_string<CharT> old = make_old<CharT>(n);
        for (std::size_t pos : {0uz, n / 2, n}) {
            for (std::size_t count : {0uz, 1uz, n / 2,
                                      n, string_type::npos}) {
                for (std::size_t source_pos : {0uz, 1uz, n / 2,
                                               n - 1, n}) {
                    for (std::size_t source_len : {0uz, n - source_pos}) {
                        for (std::size_t cap : {0uz, n + 1, 2 * n + 4}) {
                            for (bool shrink : {false, true}) {
                                for (bool shared : {false, true}) {
                                    for (call_mode mode : all_modes) {
                                        check_case(result, old, pos, count, source_pos,
                                                   source_len, cap, shrink, shared, mode);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

template<typename CharT>
void check_external(results& result, const std::basic_string<CharT>& old,
                    std::size_t pos, std::size_t count,
                    const std::basic_string<CharT>& source,
                    std::size_t cap_request, bool shrink, bool pointer_source) {
    using string_type = cow::basic_cow_string<CharT>;

    const std::size_t rcount = std::min(count, old.size() - pos);
    const std::basic_string<CharT> want =
        old.substr(0, pos) + source + old.substr(pos + rcount);

    string_type s(old.begin(), old.end());
    if (cap_request != 0) {
        s.reserve(cap_request);
    }
    if (shrink) {
        s.shrink_to_fit();
    }

    if (pointer_source) {
        s.replace(pos, count, source.data(), source.data() + source.size());
    } else {
        s.replace(pos, count, source.begin(), source.end());
    }

    ++result.cases;
    const std::basic_string<CharT> got(s.data(), s.size());
    if (got != want) {
        ++result.failures;
        if (result.failures <= 10) {
            std::println(stderr,
                "FAIL external old-size={} pos={} count={} source-size={} "
                "cap={} shrink={} pointer={}",
                old.size(), pos, count, source.size(), s.capacity(),
                shrink, pointer_source);
            print_sequence("old   ", old);
            print_sequence("source", source);
            print_sequence("got   ", got);
            print_sequence("want  ", want);
        }
    }
}

template<typename CharT>
void run_external(results& result) {
    using string_type = cow::basic_cow_string<CharT>;
    for (std::size_t n : {0uz, 1uz, 5uz,
                          10uz, 31uz, 64uz,
                          129uz}) {
        const std::basic_string<CharT> old = make_old<CharT>(n);
        for (std::size_t source_len : {0uz, 1uz,
                                       5uz, 40uz}) {
            std::basic_string<CharT> source;
            source.reserve(source_len);
            for (std::size_t i = 0; i < source_len; ++i) {
                source.push_back(static_cast<CharT>(CharT('A') + CharT((i * 5 + 1) % 26)));
            }
            for (std::size_t pos : {0uz, n / 2, n}) {
                for (std::size_t count : {0uz, 1uz, n / 2,
                                          n, string_type::npos}) {
                    for (std::size_t cap : {0uz, n + 1, 2 * n + 4}) {
                        for (bool shrink : {false, true}) {
                            check_external(result, old, pos, count, source,
                                           cap, shrink, false);
                            check_external(result, old, pos, count, source,
                                           cap, shrink, true);
                        }
                    }
                }
            }
        }
    }
}

// Replace whose source belongs to a different object sharing the same
// allocation: both the receiver and the source are substring views of one base
// string. The receiver is shared, so the in-place path cannot run and the
// fallback has to keep the source valid while the receiver detaches.
template<typename CharT>
void run_cross_sharing(results& result) {
    using string_type = cow::basic_cow_string<CharT>;

    // One allocation, three owners: the base string plus two overlapping
    // substring views. The receiver replaces with a range taken from either
    // the other view or the base itself, so the source can sit before, inside
    // or after the receiver's own live range.
    const std::basic_string<CharT> whole = make_old<CharT>(96);
    constexpr std::size_t view_len = 40;      // above SSO capacity for every char type
    constexpr std::size_t receiver_pos = 10;
    const std::basic_string<CharT> receiver_text = whole.substr(receiver_pos, view_len);

    const auto text = [](const string_type& s) {
        return std::basic_string<CharT>(std::as_const(s).data(), s.size());
    };

    for (std::size_t source_pos : {0uz, 20uz, 50uz}) {
        const std::basic_string<CharT> view_text = whole.substr(source_pos, view_len);
        // 0: the other view, 1: the owning base, 2: the receiver itself.
        for (int source_kind : {0, 1, 2}) {
            const std::basic_string<CharT>& source_text = source_kind == 1 ? whole
                                                     : source_kind == 2 ? receiver_text
                                                                        : view_text;
            for (std::size_t pos : {0uz, 20uz, view_len}) {
                for (std::size_t count : {0uz, 1uz,
                                          20uz, string_type::npos}) {
                    for (std::size_t src_pos : {0uz, 10uz, 30uz}) {
                        for (std::size_t src_len : {0uz, 1uz,
                                                    20uz, view_len}) {
                            if (src_pos + src_len > view_len) {
                                continue;
                            }
                            for (bool via_iterators : {false, true}) {
                                const std::size_t rcount = std::min(count, view_len - pos);
                                const std::basic_string<CharT> want =
                                    receiver_text.substr(0, pos)
                                  + source_text.substr(src_pos, src_len)
                                  + receiver_text.substr(pos + rcount);

                                string_type base(whole.begin(), whole.end());
                                string_type other(base, source_pos, view_len);
                                string_type receiver(base, receiver_pos, view_len);
                                const bool shared_before = receiver.is_shared()
                                    && base.cur_count() == 3 && other.cur_count() == 3;

                                const string_type& cs = source_kind == 1 ? base
                                                        : source_kind == 2 ? receiver
                                                                           : other;
                                if (via_iterators) {
                                    receiver.replace(pos, count, cs.cbegin() + src_pos,
                                                     cs.cbegin() + src_pos + src_len);
                                } else {
                                    receiver.replace(pos, count, cs.data() + src_pos,
                                                     cs.data() + src_pos + src_len);
                                }

                                ++result.cases;
                                const std::basic_string<CharT> got = text(receiver);
                                const bool base_ok = text(base) == whole;
                                const bool other_ok = text(other) == view_text;
                                // Writing through a shared view detaches only the
                                // receiver: the other two owners keep sharing.
                                const bool detached_ok =
                                    receiver.cur_count() == 1 && base.cur_count() == 2;
                                if (got != want || !shared_before || !base_ok
                                    || !other_ok || !detached_ok) {
                                    ++result.failures;
                                    if (result.failures <= 10) {
                                        std::println(stderr,
                                            "FAIL cross-sharing pos={} count={} src=[{}, {}) "
                                            "iterators={} source_kind={} shared={} base_ok={} "
                                            "other_ok={} detached_ok={}",
                                            pos, count, src_pos, src_pos + src_len,
                                            int(via_iterators), source_kind,
                                            int(shared_before), int(base_ok),
                                            int(other_ok), int(detached_ok));
                                        print_sequence("want", want);
                                        print_sequence("got ", got);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// An exclusive string whose live range starts in the middle of its allocation:
// a substring view that outlived the owning string. In-place replace must not
// silently re-base the result at the allocation start, because the object keeps
// pointing at its own offset.
template<typename CharT>
void run_prefixed_inplace(results& result) {
    using string_type = cow::basic_cow_string<CharT>;

    const std::basic_string<CharT> whole = make_old<CharT>(96);
    constexpr std::size_t view_len = 40;      // above SSO capacity for every char type

    const auto text = [](const string_type& s) {
        return std::basic_string<CharT>(std::as_const(s).data(), s.size());
    };

    for (std::size_t prefix : {1uz, 5uz, 10uz,
                               20uz}) {
        const std::basic_string<CharT> view_text = whole.substr(prefix, view_len);
        for (std::size_t pos : {0uz, 1uz, 5uz,
                                10uz, 20uz, view_len}) {
            for (std::size_t count : {0uz, 1uz, 5uz,
                                      10uz, string_type::npos}) {
                for (std::size_t src_pos : {0uz, 1uz, 5uz,
                                            10uz, 30uz}) {
                    for (std::size_t src_len : {0uz, 1uz, 5uz,
                                                10uz, 25uz,
                                                40uz}) {
                        if (src_pos + src_len > view_len) {
                            continue;
                        }
                        const std::size_t rcount = std::min(count, view_len - pos);
                        const std::basic_string<CharT> want =
                            view_text.substr(0, pos)
                          + view_text.substr(src_pos, src_len)
                          + view_text.substr(pos + rcount);

                        string_type owner(whole.begin(), whole.end());
                        string_type view(owner, prefix, view_len);
                        owner.clear();          // the view is now the only owner
                        const bool sole_owner = view.cur_count() == 1;

                        view.replace(pos, count, std::as_const(view).data() + src_pos,
                                     std::as_const(view).data() + src_pos + src_len);

                        ++result.cases;
                        const std::basic_string<CharT> got = text(view);
                        if (got != want || !sole_owner) {
                            ++result.failures;
                            if (result.failures <= 10) {
                                std::println(stderr,
                                    "FAIL prefixed prefix={} pos={} count={} src=[{}, {}) "
                                    "sole_owner={}",
                                    prefix, pos, count, src_pos, src_pos + src_len,
                                    int(sole_owner));
                                print_sequence("want", want);
                                print_sequence("got ", got);
                            }
                        }
                    }
                }
            }
        }
    }
}

// Source lies in the same allocation but (partly) outside the live range of the receiver,
// e.g. a prefix of the allocation whose tail contains the head of the receiver.
template<typename CharT>
void run_outside_source(results& result) {
    using string_type = cow::basic_cow_string<CharT>;

    const std::basic_string<CharT> whole = make_old<CharT>(96);
    constexpr std::size_t view_len = 40;

    for (std::size_t prefix : {std::size_t(5), std::size_t(10), std::size_t(20)}) {
        const std::basic_string<CharT> view_text = whole.substr(prefix, view_len);
        for (std::size_t pos : {std::size_t(0), std::size_t(1), std::size_t(3), std::size_t(5),
                                std::size_t(10), std::size_t(20), view_len}) {
            for (std::size_t count : {std::size_t(0), std::size_t(1), std::size_t(5),
                                      std::size_t(10), std::size_t(30), string_type::npos}) {
                for (std::size_t src_abs = 0; src_abs < 96; src_abs += 1) {
                    for (std::size_t src_len : {std::size_t(1), std::size_t(5), std::size_t(10),
                                                std::size_t(15), std::size_t(25),
                                                std::size_t(45)}) {
                        if (src_abs + src_len > whole.size()) {
                            continue;
                        }
                        const std::size_t rcount = std::min(count, view_len - pos);
                        const std::basic_string<CharT> want =
                            view_text.substr(0, pos)
                          + whole.substr(src_abs, src_len)
                          + view_text.substr(pos + rcount);

                        string_type owner(whole.begin(), whole.end());
                        string_type view(owner, prefix, view_len);
                        owner.clear();
                        const CharT* const base = std::as_const(view).data() - prefix;

                        view.replace(pos, count, base + src_abs, base + src_abs + src_len);

                        ++result.cases;
                        const std::basic_string<CharT> got(std::as_const(view).data(),
                                                           view.size());
                        if (got != want) {
                            ++result.failures;
                            if (result.failures <= 10) {
                                std::println(stderr,
                                    "FAIL outside prefix={} pos={} count={} abs=[{}, {})",
                                    prefix, pos, count, src_abs, src_abs + src_len);
                                print_sequence("want", want);
                                print_sequence("got ", got);
                            }
                        }
                    }
                }
            }
        }
    }
}

template<typename CharT>
void run_random(results& result, std::size_t iterations, std::uint64_t seed) {
    using string_type = cow::basic_cow_string<CharT>;
    std::mt19937_64 rng(seed);
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t n = static_cast<std::size_t>(rng() % 160);
        const std::basic_string<CharT> old = make_old<CharT>(n);

        const std::size_t pos = n == 0 ? 0 : static_cast<std::size_t>(rng() % (n + 1));
        const std::size_t counts[] = {0, 1, 2, n, n + 3, string_type::npos};
        const std::size_t count = counts[rng() % std::size(counts)];
        const std::size_t source_pos =
            n == 0 ? 0 : static_cast<std::size_t>(rng() % (n + 1));
        const std::size_t source_len =
            source_pos == n ? 0 : static_cast<std::size_t>(rng() % (n - source_pos + 1));

        const std::size_t caps[] = {0, n + 1, 2 * n + 4, n / 2 + 1};
        const std::size_t cap = caps[rng() % std::size(caps)];
        const bool shrink = (rng() % 2) != 0;
        const bool shared = (rng() % 2) != 0;
        const call_mode mode = all_modes[rng() % std::size(all_modes)];

        check_case(result, old, pos, count, source_pos, source_len,
                   cap, shrink, shared, mode);
    }
}

template<typename CharT>
results run_suite(const char* name, std::size_t exhaustive_n,
                  std::size_t random_iterations, std::uint64_t seed) {
    results result;
    run_exhaustive<CharT>(result, exhaustive_n);
    run_selected<CharT>(result);
    run_external<CharT>(result);
    run_cross_sharing<CharT>(result);
    run_prefixed_inplace<CharT>(result);
    run_outside_source<CharT>(result);
    run_random<CharT>(result, random_iterations, seed);
    std::println("[replace_self] {:<8} cases={} failures={}",
                 name, result.cases, result.failures);
    return result;
}

} // namespace

int main() {
    results total;
    auto add = [&total](const results& r) {
        total.cases += r.cases;
        total.failures += r.failures;
    };

    add(run_suite<char>("char", 10, 20000, 0x5eed'0001));
    add(run_suite<wchar_t>("wchar_t", 5, 5000, 0x5eed'0002));
    add(run_suite<char8_t>("char8_t", 5, 5000, 0x5eed'0003));
    add(run_suite<char16_t>("char16_t", 5, 5000, 0x5eed'0004));
    add(run_suite<char32_t>("char32_t", 5, 5000, 0x5eed'0005));

    if (total.failures != 0) {
        std::println("[replace_self] FAILED: cases={} failures={}",
                     total.cases, total.failures);
        return 1;
    }
    std::println("[replace_self] PASS: cases={}", total.cases);
    return 0;
}
