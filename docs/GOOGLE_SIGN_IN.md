# Google sign-in

OmaCalendar signs in to Google in your web browser. The app opens Google's
consent page; after you approve, Google returns to a temporary listener on
`127.0.0.1` that only OmaCalendar's service is waiting on. Tokens are stored in
your keyring (see [Troubleshooting](TROUBLESHOOTING.md#passwords-and-sign-ins-the-keyring)),
never in the calendar database.

## What OmaCalendar asks for

| Permission | Used for |
|---|---|
| See, edit, share and delete events on calendars you can access (`calendar.events`) | Showing and editing events |
| See and change calendar properties, create secondary calendars (`calendar.calendars`) | Calendar names, colours and new calendars |
| See the list of your calendars (`calendar.calendarlist.readonly`) | Finding your calendars |
| View your availability (`calendar.freebusy`) | Guests' availability in the event editor |
| Create, edit, organise and delete your tasks (`tasks`) | Google Tasks lists |

## Signing in again

OmaCalendar asks you to sign in again when:

- you upgrade from 1.x to 2.0 or later, because 2.0 added the free/busy and
  Tasks permissions; this happens once;
- you revoke access in your Google account, change your password, or Google
  expires the grant;
- the keyring lost the stored token (for example on a new machine).

The account shows **Sign in** in the sidebar and in the error banner. If you
don't finish in the browser within 10 minutes the attempt times out; choose
**Sign in** again. **Cancel** on the waiting screen stops an attempt.

## "Google hasn't verified this app"

Google shows this warning when a project's newest permissions have not yet
finished Google's review. If you installed OmaCalendar from the official
release and the consent page names OmaCalendar, you can continue with
**Advanced → Go to OmaCalendar**. Builds you compile yourself use the Google
client you configure and show your own project's name.

## Removing OmaCalendar's access

Remove the account in **Accounts & settings**, which deletes its local data and
its keyring item, then revoke the grant at
<https://myaccount.google.com/permissions>. Revoking only at Google makes the
account show **Sign in** until you remove or reconnect it.
