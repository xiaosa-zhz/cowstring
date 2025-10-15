#include <format>
#include <print>

#define MYLIB_COW_STRING_TEST_STATUS
#include "cowstring.hpp"

int main() {
    using namespace mylib::literals;
    mylib::cow_string str = "Hello, World!"_cowstr;
    std::println("{} {}", str.c_str(), str.is_literal());
    str.append(" I am a Copy-On-Write string. Nice to meet you!");
    std::println("{} {}", str, str.is_owned());
    mylib::cow_string str2 = str;
    std::println("{} {} {} {}", str2, str2.is_shared(),
        str2.reference_counter(), str.reference_counter());
    const std::string_view cow = "Copy-On-Write";
    const std::string_view short_cow = "COW";
    const std::string_view long_cow = "Copy-On-Write (COW)";
    str2.replace(str2.find(cow), cow.size(), short_cow);
    std::println("{} {}", str2, str2.is_owned());
    str.replace(str.find(cow), cow.size(), long_cow);
    std::println("{} {}", str, str.is_owned());
    std::println("{}", str.substr(0, 14));
    const char* goodbye = "Goodbye!";
    str2 = goodbye;
    std::println("{} {}", str2, str2.is_owned());
    mylib::cow_string str3 = str2;
    str3.shrink_to_fit();
    std::println("{} {}", str3, str3.is_sso());
}
