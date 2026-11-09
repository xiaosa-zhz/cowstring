#include <format>
#include <print>
#include <contracts>
#include <stacktrace>

#define COW_STRING_TEST_STATUS
#include "cowstring.hpp"

void handle_contract_violation(const std::contracts::contract_violation& cv) {
    std::contracts::invoke_default_contract_violation_handler(cv);
    std::println("Traceback:\n{}", std::stacktrace::current());
}

int main() {
    using namespace cow::literals;
    cow::cow_string str = "Hello, World!"_cows;
    std::println("{} {}", str.c_str(), str.is_literal());
    str.append(" I am a Copy-On-Write string. Nice to meet you!");
    std::println("{} {}", str, str.is_owned());
    cow::cow_string str2 = str;
    std::println("{} {} {} {}", str2, str2.is_shared(),
        str2.cur_count(), str.cur_count());
    const std::string_view cow = "Copy-On-Write";
    const std::string_view short_cow = "COW";
    const std::string_view long_cow = "Copy-On-Write (COW)";
    str2.replace(str2.find(cow), cow.size(), short_cow);
    std::println("{} {}", str2, str2.is_owned());
    str.replace(str.find(cow), cow.size(), long_cow);
    std::println("{} {}", str, str.is_owned());
    std::println("{}", str.substr(0, 14));
    constexpr const char* goodbye = "Goodbye!";
    str2 = goodbye;
    std::println("{} {}", str2, str2.is_owned());
    cow::cow_string str3 = str2;
    str3.shrink_to_fit();
    std::println("{} {}", str3, str3.is_small());
    cow::cow_string _;
    std::println("{} {} {} {} {}",
        sizeof(cow::cow_string),
        sizeof(cow::cow_wstring),
        sizeof(cow::cow_u8string),
        sizeof(cow::cow_u16string),
        sizeof(cow::cow_u32string));
}
