#pragma once

namespace onedrive::app {

class RuntimeFactory;

class Application {
public:
    explicit Application(const RuntimeFactory& runtime_factory);

    int run(int argc, char* argv[]);

private:
    const RuntimeFactory& runtime_factory_;
};

}  // namespace onedrive::app
