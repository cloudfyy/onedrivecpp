#include "message.hpp"

#include <stdexcept>

namespace onedrive::cli::detail {

std::string_view message_level(MessageKind kind) {
    switch (kind) {
        case MessageKind::information:
            return "info";
        case MessageKind::success:
            return "success";
        case MessageKind::warning:
            return "warning";
        case MessageKind::error:
            return "error";
    }
    throw_unknown_message_kind();
}

bool message_suppressed(
    const ConsoleOptions& options,
    MessageKind kind
) {
    static_cast<void>(message_level(kind));
    return options.quiet &&
        kind != MessageKind::warning &&
        kind != MessageKind::error;
}

void throw_unknown_message_kind() {
    throw std::logic_error{"unknown console message kind"};
}

}  // namespace onedrive::cli::detail
