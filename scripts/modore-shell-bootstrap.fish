# Lazy bootstrap loader for fish.
#
# Source this from config.fish. It waits for the first prompt event, then
# asks modore-host for the shell-specific binding snippet and sources it.

function modore_shell_bootstrap_load --on-event fish_prompt
    if set -q MODORE_SHELL_BOOTSTRAP_LOADED
        functions -e modore_shell_bootstrap_load
        return
    end

    set -l host ""
    if set -q MODORE_HOST_EXECUTABLE
        set host $MODORE_HOST_EXECUTABLE
    else
        set host (command -v modore-host 2>/dev/null)
    end
    set host (string trim -- $host)
    if test -z "$host"
        return
    end

    set -l stdout_file (mktemp -t modore-shell-bootstrap.out.XXXXXX)
    set -l stderr_file (mktemp -t modore-shell-bootstrap.err.XXXXXX)
    if env MODORE_SHELL=fish command $host --print-shell-bootstrap >$stdout_file 2>$stderr_file
        set -l bootstrap (cat $stdout_file)
    else
        cat $stderr_file >&2
        rm -f $stdout_file $stderr_file
        return
    end
    rm -f $stdout_file $stderr_file
    printf '%s\n' $bootstrap | source
    or begin
        return
    end

    set -gx MODORE_SHELL_BOOTSTRAP_LOADED 1
    functions -e modore_shell_bootstrap_load
end

modore_shell_bootstrap_load
