#include <contracts>
#include <print>
#include <stacktrace>

void handle_contract_violation(const std::contracts::contract_violation& cv) {
    std::contracts::invoke_default_contract_violation_handler(cv);
    std::println(stderr, "Traceback:\n{}", std::stacktrace::current());
}
