_onedrive_cpp_completion()
{
    local current previous command="" account_action="" inspect_action="" state_action="" transfer_action="" word
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
        --ui)
            mapfile -t COMPREPLY < <(
                compgen -W "auto console tui" -- "$current"
            )
            return
            ;;
        --theme)
            mapfile -t COMPREPLY < <(
                compgen -W "hacker ocean amber synthwave" -- "$current"
            )
            return
            ;;
        --status)
            mapfile -t COMPREPLY < <(
                compgen -W "ok missing modified type-changed outside-root" \
                    -- "$current"
            )
            return
            ;;
        --mode)
            mapfile -t COMPREPLY < <(
                compgen -W "metadata content" -- "$current"
            )
            return
            ;;
    esac

    for word in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
        case "$word" in
            account|inspect|state|transfer)
                if [[ -z "$command" ]]; then
                    command="$word"
                fi
                ;;
            login|logout)
                if [[ "$command" == "account" ]]; then
                    account_action="$word"
                fi
                ;;
            health|status|drives|shared|sites|quota|storage|partials|files|verify|config)
                if [[ "$command" == "inspect" ]]; then
                    inspect_action="$word"
                fi
                ;;
            reset-cursor|cleanup|migrate|clear)
                if [[ "$command" == "state" ]]; then
                    state_action="$word"
                fi
                ;;
            sync|download|watch)
                if [[ "$command" == "transfer" ]]; then
                    transfer_action="$word"
                fi
                ;;
        esac
    done

    if [[ -z "$command" ]]; then
        mapfile -t COMPREPLY < <(compgen -W \
            "account inspect state transfer --help --version" \
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
        account)
            case "$account_action" in
                login)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme" -- "$current")
                    ;;
                logout)
                    mapfile -t COMPREPLY < <(
                        compgen -W "$common_options" -- "$current"
                    )
                    ;;
                *)
                    mapfile -t COMPREPLY < <(
                        compgen -W "login logout --help" -- "$current"
                    )
                    ;;
            esac
            ;;
        state)
            case "$state_action" in
                reset-cursor)
                    mapfile -t COMPREPLY < <(
                        compgen -W "$common_options" -- "$current"
                    )
                    ;;
                cleanup|migrate)
                    mapfile -t COMPREPLY < <(
                        compgen -W "$common_options --dry-run --yes" -- "$current"
                    )
                    ;;
                clear)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --yes" -- "$current")
                    ;;
                *)
                    mapfile -t COMPREPLY < <(
                        compgen -W "reset-cursor cleanup migrate clear --help" -- "$current"
                    )
                    ;;
            esac
            ;;
        inspect)
            case "$inspect_action" in
                health|status|drives|shared|sites|quota|storage|partials|config)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme" -- "$current")
                    ;;
                files)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme --status" -- "$current")
                    ;;
                verify)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme --mode" -- "$current")
                    ;;
                *)
                    mapfile -t COMPREPLY < <(
                        compgen -W "health status drives shared sites quota storage partials files verify config --help" -- "$current"
                    )
                    ;;
            esac
            ;;
        transfer)
            case "$transfer_action" in
                sync)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme --dry-run --force-large-delete" -- "$current")
                    ;;
                download)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme --dry-run" -- "$current")
                    ;;
                watch)
                    mapfile -t COMPREPLY < <(compgen -W \
                        "$common_options --ui --theme" -- "$current")
                    ;;
                *)
                    mapfile -t COMPREPLY < <(
                        compgen -W "sync download watch --help" -- "$current"
                    )
                    ;;
            esac
            ;;
    esac
}

complete -F _onedrive_cpp_completion onedrive-cpp
