# What each provider supports

| | Google Calendar | CalDAV | Local | ICS subscription |
|---|---|---|---|---|
| Read events | Yes | Yes | Yes | Yes |
| Create, edit, delete | Yes | Yes | Yes | No (read-only) |
| Recurring events and exceptions | Yes | Yes | Yes | Read-only |
| "This and following" edits | Yes | When the server passes the check | Yes | No |
| Reply to invitations | Yes | When the server supports scheduling (not this-and-following) | No | No |
| Guests' availability | Yes | No | No | No |
| Video conferencing link | Google Meet | No | No | No |
| Attachments | Shown | Shown | No | Shown |
| Reminders | Yes | Yes | Yes | Muted by default for new subscriptions |
| Tasks | Google Tasks | VTODO task lists | Device-only Tasks list | No |
| Works offline | Cached; changes queue | Cached; changes queue | Always | Cached copy |
| Credentials | Browser sign-in, token in keyring | Password in keyring | None | Optional password in keyring |

Changes made offline are queued and sent when the provider is reachable.
Conflicting edits made elsewhere appear in Activity for you to resolve.
