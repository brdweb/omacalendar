# fish completion for omacalendarctl
complete -c omacalendarctl -f
complete -c omacalendarctl -s h -l help -d 'Show help'
complete -c omacalendarctl -l version -d 'Show the version'
complete -c omacalendarctl -l timeout -x -d 'Seconds to wait for a response'
complete -c omacalendarctl -l params-stdin -d 'Read JSON params from stdin'
complete -c omacalendarctl -l watch -d 'Print notifications after the response'
complete -c omacalendarctl -l json -d 'Print results as JSON'
complete -c omacalendarctl -l days -x -d 'Days for agenda'
complete -c omacalendarctl -l calendar -x -d 'Calendar for add'
complete -c omacalendarctl -l copy-to -r -F -d 'Also copy the backup here'
complete -c omacalendarctl -n __fish_use_subcommand -a today -d "List today's events"
complete -c omacalendarctl -n __fish_use_subcommand -a agenda -d 'List upcoming events'
complete -c omacalendarctl -n __fish_use_subcommand -a add -d 'Create an event from quick-add text'
complete -c omacalendarctl -n __fish_use_subcommand -a sync -d 'Sync every account'
complete -c omacalendarctl -n __fish_use_subcommand -a methods -d 'List IPC methods'
complete -c omacalendarctl -n __fish_use_subcommand -a backup -d 'Save a copy of the calendar database'
complete -c omacalendarctl -n __fish_use_subcommand -a '(omacalendarctl methods 2>/dev/null)'
