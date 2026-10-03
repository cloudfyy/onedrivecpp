#include "onedrive/app/application.hpp"
#include "onedrive/app/runtime_factory.hpp"

int main(int argc, char* argv[]) {
    onedrive::app::RuntimeFactory runtime_factory{
        std::in_place_type<onedrive::app::ProductionRuntimeFactory>
    };
    onedrive::app::Application application{&runtime_factory};
    return application.run(argc, argv);
}
