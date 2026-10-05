_onedrive_cpp_completion()
{
    local current previous command="" word
    COMPREPLY=()
    current="${COMP_WORDS[COMP_CWORD]}"
    previous="${COMP_WORDS[COMP_CWORD-1]}"

    case "$previous" in
        --config|--log-file)
            mapfile -t COMPREPLY < <(compgen -f -- "$current")
            return
            ;;
        --log-level)
            mapfile -t COMPREPLY < <(compgen -W \
                "trace debug info warn error critical off" -- "$current")
            return
            ;;
        --color)
            mapfile -t COMPREPLY < <(
                compgen -W "auto always never" -- "$current"
            )
            return
            ;;
        --output)
            mapfile -t COMPREPLY < <(compgen -W "text json" -- "$current")
            return
            ;;
    esac

    for word in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
        case "$word" in
            auth|logout|reset-state|download|sync|monitor)
                command="$word"
                break
                ;;
        esac
    done

    if [[ -z "$command" ]]; then
        mapfile -t COMPREPLY < <(compgen -W \
            "auth logout reset-state download sync monitor --help --version" \
            -- "$current")
        return
    fi

    local common_options="
        --config
        --log-level
        --log-file
        --color
        --output
        --quiet
        --help
    "
    case "$command" in
        download)
            mapfile -t COMPREPLY < <(compgen -W \
                "$common_options --dry-run" -- "$current")
            ;;
        sync)
            mapfile -t COMPREPLY < <(compgen -W \
                "$common_options --dry-run --force-large-delete" -- "$current")
            ;;
        reset-state)
            mapfile -t COMPREPLY < <(compgen -W \
                "$common_options --clear-all --yes" -- "$current")
            ;;
        auth|logout|monitor)
            mapfile -t COMPREPLY < <(
                compgen -W "$common_options" -- "$current"
            )
            ;;
    esac
}

complete -F _onedrive_cpp_completion onedrive-cpp
