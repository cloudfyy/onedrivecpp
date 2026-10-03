#include "onedrive/app/application.hpp"
#include "onedrive/app/runtime_factory.hpp"

int main(int argc, char* argv[]) {
    onedrive::app::ProductionRuntimeFactory runtime_factory;
    onedrive::app::Application application{&runtime_factory};
    return application.run(argc, argv);
}
