#pragma once

#include <memory>
#include <proxy/proxy.h>
#include <utility>

namespace onedrive::detail {

struct BorrowedProxyTag {
    explicit BorrowedProxyTag() = default;
};

inline constexpr BorrowedProxyTag borrowed_proxy;

template <typename Facade>
class ProxyService {
public:
    template <typename Implementation, typename... Args>
    explicit ProxyService(
        std::in_place_type_t<Implementation>,
        Args&&... args
    )
        : implementation_{pro::make_proxy<
              Facade,
              Implementation
          >(std::forward<Args>(args)...)} {}

    template <typename Implementation>
    explicit ProxyService(std::unique_ptr<Implementation> implementation)
        : implementation_{std::move(implementation)} {}

    template <typename Implementation>
    ProxyService(BorrowedProxyTag, Implementation& implementation)
        : implementation_{&implementation} {}

    ProxyService(const ProxyService&) = delete;
    ProxyService& operator=(const ProxyService&) = delete;
    ProxyService(ProxyService&&) noexcept = default;
    ProxyService& operator=(ProxyService&&) noexcept = default;
    ~ProxyService() = default;

protected:
    [[nodiscard]] pro::proxy<Facade>& implementation() noexcept {
        return implementation_;
    }

    [[nodiscard]] const pro::proxy<Facade>& implementation() const noexcept {
        return implementation_;
    }

private:
    pro::proxy<Facade> implementation_;
};

}  // namespace onedrive::detail
