#pragma once

#include <string_view>

namespace onedrive::app {

class Application {
public:
    int run(int argc, char* argv[]);

private:
    static void print_help(std::string_view program);
};

}  // namespace onedrive::app
