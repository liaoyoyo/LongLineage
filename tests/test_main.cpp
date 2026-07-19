// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/hts.h>
#include <jansson.h>
#include <openssl/crypto.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

int main() {
    static_assert(__cplusplus >= 201703L, "LongLineage requires C++17");

    const char* htslib_version = hts_version();
    if (htslib_version == nullptr || std::string(htslib_version) != "1.18") {
        std::cerr << "dependency_smoke: HTSlib exact-version failure\n";
        return 1;
    }
    if (JANSSON_VERSION_HEX < 0x020D00) {
        std::cerr << "dependency_smoke: Jansson version failure\n";
        return 1;
    }
    if (OpenSSL_version_num() < 0x30000000L) {
        std::cerr << "dependency_smoke: OpenSSL version failure\n";
        return 1;
    }

    std::atomic<bool> worker_ran{false};
    std::thread worker([&worker_ran]() { worker_ran.store(true); });
    worker.join();
    if (!worker_ran.load()) {
        std::cerr << "dependency_smoke: Threads failure\n";
        return 1;
    }

    std::cout << "dependency_smoke: PASS htslib=" << htslib_version << " jansson=" << JANSSON_VERSION
              << " openssl=" << OpenSSL_version(OPENSSL_VERSION) << '\n';
    return 0;
}
