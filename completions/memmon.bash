# bash completion for memmon                                -*- shell-script -*-

_memmon()
{
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        -f|--format|-F|--log-format)
            mapfile -t COMPREPLY < <(compgen -W "text json csv" -- "$cur")
            return ;;
        -u|--units)
            mapfile -t COMPREPLY < <(compgen -W "auto k m g" -- "$cur")
            return ;;
        -i|--interval)
            mapfile -t COMPREPLY < <(compgen -W "5s 30s 1m 5m 15m 1h" -- "$cur")
            return ;;
        -l|--log-dir)
            mapfile -t COMPREPLY < <(compgen -d -- "$cur")
            return ;;
        -c|--config)
            mapfile -t COMPREPLY < <(compgen -f -- "$cur")
            return ;;
        -R|--report)
            local dates
            dates="today yesterday $(for d in /var/log/memmon "$HOME/.local/share/memmon"; do
                [ -d "$d" ] && ls "$d" 2>/dev/null; done | sed -n 's/^\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\)\..*/\1/p' | sort -u)"
            mapfile -t COMPREPLY < <(compgen -W "$dates" -- "$cur")
            if [[ "$cur" == */* ]]; then
                mapfile -t -O "${#COMPREPLY[@]}" COMPREPLY < <(compgen -f -- "$cur")
            fi
            return ;;
        -r|--log-retention-days|-t|--top|-w|--warn|-C|--crit|-a|--alert-cmd|-g|--cgroup)
            return ;;
    esac

    if [[ "$cur" == -* ]]; then
        local opts="-i --interval -d --daemon -o --once -p --plain -R --report
            -l --log-dir -r --log-retention-days -F --log-format --no-compress
            -n --no-log -f --format -t --top -w --warn -C --crit -a --alert-cmd
            -g --cgroup -u --units --no-align -c --config -h --help -v --version"
        mapfile -t COMPREPLY < <(compgen -W "$opts" -- "$cur")
    fi
}
complete -F _memmon memmon
