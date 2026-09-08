#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "../src/engine/db_engine.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: test_locking <db_path> <mode>\n";
        std::cerr << "Modes: hold, try_open\n";
        return 1;
    }

    std::string db_path = argv[1];
    std::string mode = argv[2];

    try {
        if (mode == "hold") {
            std::cout << "Attempting to acquire DB lock..." << std::endl;
            DatabaseEngine db(db_path);
            std::cout << "SUCCESS_HOLD" << std::endl;
            // Sleep forever until forcefully terminated
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else if (mode == "try_open") {
            DatabaseEngine db(db_path);
            std::cout << "SUCCESS_OPEN" << std::endl;
        } else {
            std::cerr << "Unknown mode: " << mode << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::string err_msg = e.what();
        if (err_msg.find("locked by another process") != std::string::npos) {
            std::cout << "ERROR_LOCKED" << std::endl;
            return 0; // expected in some cases, so exit 0 to allow test script to verify output
        } else {
            std::cerr << "ERROR: " << err_msg << std::endl;
            return 1;
        }
    }

    return 0;
}
