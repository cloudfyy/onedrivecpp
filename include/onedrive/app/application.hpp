#pragma once

#include <gsl/pointers>

namespace onedrive::app {

class RuntimeFactory;

class Application {
public:
    explicit Application(gsl::not_null<const RuntimeFactory*> runtime_factory);

    int run(int argc, char* argv[]);

private:
    gsl::not_null<const RuntimeFactory*> runtime_factory_;
};

}  // namespace onedrive::app
