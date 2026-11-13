# bash completion for omacalendarctl
_omacalendarctl() {
  local cur prev
  cur=${COMP_WORDS[COMP_CWORD]}
  prev=${COMP_WORDS[COMP_CWORD-1]}
  case "${prev}" in
    --timeout|--days|--calendar) return ;;
  esac
  if [[ "${cur}" == -* ]]; then
    COMPREPLY=($(compgen -W "--help --version --timeout --params-stdin --watch --json --days --calendar" -- "${cur}"))
    return
  fi
  local i word
  for ((i = 1; i < COMP_CWORD; i++)); do
    word=${COMP_WORDS[i]}
    case "${word}" in
      --timeout|--days|--calendar) ((i++)) ;;
      -*) ;;
      *) return ;;
    esac
  done
  local methods
  methods=$(omacalendarctl methods 2>/dev/null)
  COMPREPLY=($(compgen -W "today agenda add sync methods ${methods}" -- "${cur}"))
}
complete -F _omacalendarctl omacalendarctl
