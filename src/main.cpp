#include "onedrive/app/app.hpp"
#include "onedrive/app/factory.hpp"

int main(int argc, char* argv[]) {
    onedrive::app::RuntimeFactory runtime_factory{
        std::in_place_type<onedrive::app::ProductionRuntimeFactory>
    };
    onedrive::app::Application application{&runtime_factory};
    return application.run(argc, argv);
}
