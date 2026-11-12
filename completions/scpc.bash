# Bash completion for scpc (parallel scp wrapper)

_scpc_collect_hosts() {
    local hosts=""

    # 1. ~/.ssh/config Host entries
    if [[ -r "$HOME/.ssh/config" ]]; then
        hosts+=" $(awk '
            /^[[:space:]]*[Hh][Oo][Ss][Tt][[:space:]]+/ {
                sub(/#.*/, "");
                for (i = 2; i <= NF; i++) {
                    if ($i !~ /[*?!]/) print $i;
                }
            }
        ' "$HOME/.ssh/config" 2>/dev/null)"
    fi

    # 2. ~/.ssh/known_hosts
    if [[ -r "$HOME/.ssh/known_hosts" ]]; then
        hosts+=" $(awk '
            !/^[[:space:]]*($|#|@|\|)/ {
                n = split($1, arr, ",");
                for (i = 1; i <= n; i++) {
                    h = arr[i];
                    gsub(/^\[|\]:[0-9]+$/, "", h);
                    if (h != "") print h;
                }
            }
        ' "$HOME/.ssh/known_hosts" 2>/dev/null)"
    fi

    # 3. /etc/hosts
    if [[ -r "/etc/hosts" ]]; then
        hosts+=" $(awk '
            !/^[[:space:]]*($|#)/ {
                sub(/#.*/, "");
                for (i = 2; i <= NF; i++) print $i;
            }
        ' "/etc/hosts" 2>/dev/null)"
    fi

    printf '%s\n' $hosts | sort -u
}

_scpc_bash_complete() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev=""
    if [[ $COMP_CWORD -gt 0 ]]; then
        prev="${COMP_WORDS[COMP_CWORD - 1]}"
    fi

    case "$prev" in
        -F|-i|-S|-D)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
        -o)
            COMPREPLY=( $(compgen -W "asc desc" -- "$cur") )
            return 0
            ;;
        -w|--workers|-c|-J|-l|-P|-X)
            return 0
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        local opts="-w --workers -s --skip -f --full -h --help -3 -4 -6 -A -B -C -O -p -q -R -r -T -v -c -D -F -i -J -l -o -P -S -X"
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
        return 0
    fi

    # Remote path completion if host:path is already started
    if [[ "$cur" == *:* ]]; then
        local user_host="${cur%%:*}"
        local rpath="${cur#*:}"
        local rfiles
        rfiles=$(ssh -o BatchMode=yes -o ConnectTimeout=2 "$user_host" "ls -1dAp -- ${rpath}*" 2>/dev/null)
        if [[ -n "$rfiles" ]]; then
            type compopt &>/dev/null && compopt -o nospace
            COMPREPLY=( $(compgen -W "$rfiles" -- "$rpath") )
            return 0
        fi
        return 0
    fi

    local user_prefix=""
    local host_part="$cur"
    if [[ "$cur" == *@* ]]; then
        user_prefix="${cur%%@*}@"
        host_part="${cur#*@}"
    fi

    local host_candidates=()
    local h
    while IFS= read -r h; do
        [[ -n "$h" ]] && host_candidates+=("${user_prefix}${h}:")
    done < <(_scpc_collect_hosts)

    type compopt &>/dev/null && compopt -o nospace
    COMPREPLY=( $(compgen -W "${host_candidates[*]}" -- "$cur") )

    if [[ -z "$user_prefix" ]]; then
        local files=()
        while IFS= read -r f; do
            files+=("$f")
        done < <(compgen -f -- "$cur")
        COMPREPLY+=( "${files[@]}" )
    fi
    return 0
}

complete -F _scpc_bash_complete scpc

