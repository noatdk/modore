# Lazy bootstrap loader for bash / zsh.
#
# Source this from ~/.bashrc or ~/.zshrc. It registers a prompt hook that
# asks modore-host for the actual shell binding snippet the first time the
# shell is ready, so startup stays cheap.

case $- in
  *i*) ;;
  *) return 0 ;;
esac

if [ -n "${ZSH_VERSION-}" ]; then
  MODORE_SHELL_NAME=zsh
elif [ -n "${BASH_VERSION-}" ]; then
  MODORE_SHELL_NAME=bash
else
  MODORE_SHELL_NAME=sh
fi

modore_shell_predeclare_zsh_widgets() {
  if [ -z "${ZSH_VERSION-}" ] || [ -n "${MODORE_SHELL_WIDGETS_PREDECLARED-}" ]; then
    return 0
  fi
  modore_shell_convert() { return 0; }
  modore_shell_convert_katakana() { return 0; }
  modore_shell_choose_candidate() { return 0; }
  zle -N modore_shell_convert
  zle -N modore_shell_convert_katakana
  zle -N modore_shell_choose_candidate
  MODORE_SHELL_WIDGETS_PREDECLARED=1
}

modore_shell_predeclare_zsh_widgets

MODORE_HOST_EXECUTABLE="${MODORE_HOST_EXECUTABLE-}"
if [ -z "$MODORE_HOST_EXECUTABLE" ]; then
  MODORE_HOST_EXECUTABLE="$(command -v modore-host 2>/dev/null)"
fi
if [ -z "$MODORE_HOST_EXECUTABLE" ]; then
  return 0
fi

modore_shell_bootstrap_load() {
  if [ -n "${MODORE_SHELL_BOOTSTRAP_LOADED-}" ]; then
    return 0
  fi
  local modore_bootstrap
  local modore_err
  modore_err="$(mktemp -t modore-shell-bootstrap.XXXXXX)" || return 0
  modore_bootstrap="$(MODORE_SHELL="$MODORE_SHELL_NAME" "$MODORE_HOST_EXECUTABLE" --print-shell-bootstrap 2>"$modore_err")" || {
    cat "$modore_err" >&2
    rm -f "$modore_err"
    return 0
  }
  rm -f "$modore_err"
  eval "$modore_bootstrap" || {
    return 0
  }
  MODORE_SHELL_BOOTSTRAP_LOADED=1
}

if [ -n "${ZSH_VERSION-}" ]; then
  modore_shell_bootstrap_load
elif [ -n "${BASH_VERSION-}" ]; then
  modore_shell_bootstrap_load
fi
