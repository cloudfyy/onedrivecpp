#include "onedrive/storage/item_database.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_{
              std::filesystem::temp_directory_path() /
              ("onedrive-cpp-item-database-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
          } {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

int fail(const std::string& message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main() {
    TemporaryDirectory temporary_directory;

    {
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();
        if (database.size() != 0) {
            return fail("new state database is not empty");
        }

        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-1",
            .local_path = "documents/report.txt",
        });
        database.upsert({
            .remote_id = "remote-2",
            .etag = "etag-2",
            .local_path = "photos/image.jpg",
        });
        database.upsert({
            .remote_id = "remote-1",
            .etag = "etag-updated",
            .local_path = "documents/report-renamed.txt",
        });

        if (database.size() != 2) {
            return fail("upsert did not preserve the expected item count");
        }
    }

    {
        onedrive::storage::ItemDatabase database{temporary_directory.path()};
        database.open();

        const auto* first = database.find("remote-1");
        const auto* second = database.find("remote-2");
        if (database.size() != 2 || first == nullptr || second == nullptr) {
            return fail("persisted items were not loaded");
        }
        if (first->etag != "etag-updated" ||
            first->local_path != "documents/report-renamed.txt") {
            return fail("updated item state was not persisted");
        }
        if (second->etag != "etag-2" || second->local_path != "photos/image.jpg") {
            return fail("second item state was not persisted");
        }
    }

    if (!std::filesystem::exists(temporary_directory.path() / "items.sqlite3")) {
        return fail("SQLite state database was not created");
    }

    return EXIT_SUCCESS;
}
