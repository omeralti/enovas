// ============================================================================
// Test Suite: Lock-free MPMC Circular Buffer
// ============================================================================
// Bu test dosyası CircularBuffer'ın tüm özelliklerini test eder
// ============================================================================

#include "main.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>

struct TestResults {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;
    
    void report(const std::string& test_name, bool success, const std::string& msg = "") {
        if (success) {
            ++passed;
            std::cout << "✓ " << test_name << " PASSED" << std::endl;
        } else {
            ++failed;
            failures.push_back(test_name + ": " + msg);
            std::cout << "✗ " << test_name << " FAILED: " << msg << std::endl;
        }
    }
    
    void summary() {
        std::cout << "\n========================================" << std::endl;
        std::cout << "TEST SUMMARY" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Passed: " << passed << std::endl;
        std::cout << "Failed: " << failed << std::endl;
        if (!failures.empty()) {
            std::cout << "\nFailures:" << std::endl;
            for (const auto& f : failures) {
                std::cout << "  - " << f << std::endl;
            }
        }
        std::cout << "========================================\n" << std::endl;
    }
};

TestResults results;

void test_basic_producer_consumer() {
    CircularBuffer buffer(4, 64);
    auto ticket = buffer.claim_producer();
    if (!ticket) {
        results.report("test_basic_producer_consumer", false, "claim_producer nullopt");
        return;
    }
    std::snprintf(ticket->cpu_ptr, 64, "test-data");
    *ticket->rf = {1, 2.5};
    *ticket->size_ptr = 10;
    buffer.commit_producer(*ticket);
    
    auto consumer_ticket = buffer.claim_consumer();
    if (!consumer_ticket) {
        results.report("test_basic_producer_consumer", false, "claim_consumer nullopt");
        return;
    }
    bool success = (std::string(consumer_ticket->cpu_ptr) == "test-data" &&
                    consumer_ticket->rf->first == 1 &&
                    consumer_ticket->rf->second == 2.5 &&
                    *consumer_ticket->size_ptr == 10);
    buffer.release_consumer(*consumer_ticket);
    results.report("test_basic_producer_consumer", success);
}

void test_non_blocking() {
    CircularBuffer buffer(4, 64);
    auto ticket = buffer.claim_consumer();
    bool success = !ticket.has_value();
    results.report("test_non_blocking", success, success ? "" : "Expected nullopt");
}

void test_exception_safety() {
    CircularBuffer buffer(4, 64);
    auto ticket1 = buffer.claim_producer();
    if (!ticket1) {
        results.report("test_exception_safety", false, "First claim failed");
        return;
    }
    std::size_t pos1 = ticket1->pos;
    auto ticket2 = buffer.claim_producer();
    if (!ticket2) {
        results.report("test_exception_safety", false, "Second claim failed");
        return;
    }
    bool success = (ticket2->pos == pos1);
    results.report("test_exception_safety", success, success ? "" : "Different slots");
}

void test_multiple_producer_consumer() {
    CircularBuffer buffer(8, 64);
    constexpr int num_producers = 3;
    constexpr int num_consumers = 2;
    constexpr int items_per_producer = 10;
    
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_producers; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < items_per_producer; ++j) {
                auto ticket = buffer.claim_producer();
                if (!ticket) continue;
                std::snprintf(ticket->cpu_ptr, 64, "P%d-%d", i, j);
                *ticket->rf = {i, static_cast<double>(j)};
                *ticket->size_ptr = 10;
                buffer.commit_producer(*ticket);
                ++produced;
            }
        });
    }
    
    for (int i = 0; i < num_consumers; ++i) {
        threads.emplace_back([&]() {
            while (consumed.load() < num_producers * items_per_producer) {
                auto ticket = buffer.claim_consumer();
                if (!ticket) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                buffer.release_consumer(*ticket);
                ++consumed;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    bool success = (produced.load() == num_producers * items_per_producer &&
                    consumed.load() == num_producers * items_per_producer);
    results.report("test_multiple_producer_consumer", success,
                   success ? "" : "Produced/Consumed mismatch");
}

void test_shutdown() {
    CircularBuffer buffer(4, 64);
    buffer.stop();
    auto ticket = buffer.claim_producer();
    bool success = !ticket.has_value();
    results.report("test_shutdown", success, success ? "" : "Expected nullopt");
}

void test_capacity_limit() {
    CircularBuffer buffer(4, 64);
    std::vector<std::optional<CircularBuffer::Ticket>> tickets;
    for (int i = 0; i < 4; ++i) {
        auto ticket = buffer.claim_producer();
        if (ticket) tickets.push_back(ticket);
    }
    auto ticket5 = buffer.claim_producer();
    bool success = !ticket5.has_value();
    for (auto& t : tickets) {
        if (t) buffer.commit_producer(*t);
    }
    results.report("test_capacity_limit", success, success ? "" : "Expected nullopt");
}

void test_thread_safety() {
    CircularBuffer buffer(8, 64);
    constexpr int num_threads = 10;
    constexpr int iterations = 100;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < iterations; ++j) {
                auto ticket = buffer.claim_producer();
                if (ticket) {
                    std::snprintf(ticket->cpu_ptr, 64, "T%d-%d", i, j);
                    buffer.commit_producer(*ticket);
                    ++success_count;
                }
                auto consumer_ticket = buffer.claim_consumer();
                if (consumer_ticket) {
                    buffer.release_consumer(*consumer_ticket);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    bool success = (success_count.load() > 0);
    results.report("test_thread_safety", success, success ? "" : "No operations");
}

void test_commit_increments_tail() {
    CircularBuffer buffer(4, 64);
    auto ticket1 = buffer.claim_producer();
    if (!ticket1) {
        results.report("test_commit_increments_tail", false, "First claim failed");
        return;
    }
    std::size_t pos1 = ticket1->pos;
    buffer.commit_producer(*ticket1);
    
    auto ticket2 = buffer.claim_producer();
    if (!ticket2) {
        results.report("test_commit_increments_tail", false, "Second claim failed");
        return;
    }
    std::size_t pos2 = ticket2->pos;
    bool success = (pos2 != pos1);
    buffer.commit_producer(*ticket2);
    results.report("test_commit_increments_tail", success, success ? "" : "Same slot");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "MPMC Circular Buffer Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    test_basic_producer_consumer();
    test_non_blocking();
    test_exception_safety();
    test_multiple_producer_consumer();
    test_shutdown();
    test_capacity_limit();
    test_thread_safety();
    test_commit_increments_tail();
    
    results.summary();
    return (results.failed == 0) ? 0 : 1;
}

