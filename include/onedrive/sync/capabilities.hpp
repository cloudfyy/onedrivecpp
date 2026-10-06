#pragma once

namespace onedrive::sync {

enum class SyncMode {
    bidirectional,
    upload_only,
    download_only,
};

enum class DeletePolicy {
    propagate,
    preserve,
};

enum class ExecutionMode {
    apply,
    preview,
};

class SyncCapabilities {
public:
    [[nodiscard]] constexpr SyncMode sync_mode() const noexcept {
        return sync_mode_;
    }

    [[nodiscard]] constexpr DeletePolicy delete_policy() const noexcept {
        return delete_policy_;
    }

    [[nodiscard]] constexpr ExecutionMode execution_mode() const noexcept {
        return execution_mode_;
    }

    [[nodiscard]] constexpr bool previews() const noexcept {
        return execution_mode_ == ExecutionMode::preview;
    }

    [[nodiscard]] constexpr bool plans_downloads() const noexcept {
        return sync_mode_ != SyncMode::upload_only;
    }

    [[nodiscard]] constexpr bool downloads() const noexcept {
        return !previews() && plans_downloads();
    }

    [[nodiscard]] constexpr bool plans_local_deletions() const noexcept {
        return plans_downloads() && delete_policy_ == DeletePolicy::propagate;
    }

    [[nodiscard]] constexpr bool removes_local_items() const noexcept {
        return !previews() && plans_local_deletions();
    }

    [[nodiscard]] constexpr bool plans_uploads() const noexcept {
        return sync_mode_ != SyncMode::download_only;
    }

    [[nodiscard]] constexpr bool uploads() const noexcept {
        return !previews() && plans_uploads();
    }

    [[nodiscard]] constexpr bool plans_remote_deletions() const noexcept {
        return plans_uploads() && delete_policy_ == DeletePolicy::propagate;
    }

    [[nodiscard]] constexpr bool removes_remote_items() const noexcept {
        return !previews() && plans_remote_deletions();
    }

    [[nodiscard]] constexpr bool resolves_conflicts() const noexcept {
        return !previews() && (plans_downloads() || plans_uploads());
    }

    [[nodiscard]] constexpr bool writes_journal() const noexcept {
        return !previews() && (plans_downloads() || plans_uploads());
    }

    [[nodiscard]] constexpr bool monitors() const noexcept {
        return true;
    }

private:
    friend constexpr SyncCapabilities
        capabilities_for(SyncMode, DeletePolicy, ExecutionMode) noexcept;

    constexpr SyncCapabilities(
        SyncMode sync_mode,
        DeletePolicy delete_policy,
        ExecutionMode execution_mode
    ) noexcept
        : sync_mode_{sync_mode},
          delete_policy_{delete_policy},
          execution_mode_{execution_mode} {
    }

    SyncMode sync_mode_;
    DeletePolicy delete_policy_;
    ExecutionMode execution_mode_;
};

[[nodiscard]] constexpr SyncCapabilities capabilities_for(
    SyncMode sync_mode, DeletePolicy delete_policy, ExecutionMode execution_mode
) noexcept {
    return SyncCapabilities{sync_mode, delete_policy, execution_mode};
}

} // namespace onedrive::sync
