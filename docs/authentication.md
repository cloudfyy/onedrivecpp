# Microsoft authentication

English | [简体中文](authentication.zh-CN.md) |
[Documentation index](README.md)

The CLI uses the OAuth 2.0 Device Authorization Grant. The GUI uses the system
browser with Authorization Code + PKCE (S256) and a loopback callback.
You must register
your own public client application in Microsoft Entra; do not create a client
secret.

## Register the application

Microsoft currently requires an Azure account with an active subscription,
access to a Microsoft Entra tenant, and permission to register applications.
If you only have a personal Outlook.com/Hotmail account and cannot open
**App registrations**, create a
[free Azure account](https://azure.microsoft.com/pricing/purchase-options/azure-account)
and use its **Default Directory**, or ask the tenant administrator for the
Application Developer role.

1. Sign in to the
   [Microsoft Entra admin center](https://entra.microsoft.com/).
2. Open **Entra ID > App registrations > New registration**.
3. Enter a name such as `onedrive-cpp`.
4. Select the supported account type:
   - For both work/school and personal Microsoft accounts, select
     **Any Entra ID Tenant + Personal Microsoft accounts** and later use
     `auth.tenant_id = "common"`.
   - For personal Microsoft accounts only, select **Personal accounts only**
     and later use `auth.tenant_id = "consumers"`.
   - For one organization only, select **Single tenant**, then use that
     directory's tenant ID instead of `common`.
5. Select **Register**.
6. On the application **Overview** page, copy the **Application (client) ID**.
   Do not copy the Object ID or Directory ID into `auth.application_id`.
7. Open **Authentication > Advanced settings**, set
   **Allow public client flows** to **Yes**, and save.

8. For GUI login, open **Authentication > Add a platform > Mobile and desktop
   applications**, add the redirect URI `http://localhost`, and save. The GUI
   listens on IPv4 loopback with a random port and uses `http://localhost:PORT/`;
   Microsoft ignores the port when matching a localhost redirect registration.
   Do not register this as a Web application.

CLI device code flow does not require a redirect URI. Because this is a public
client, do not add a client secret: a secret embedded in a desktop or CLI
application cannot be kept confidential.

Microsoft's corresponding documentation is:

- [Register an application in Microsoft Entra ID](https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app)
- [Configure desktop and public client applications](https://learn.microsoft.com/en-us/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 device authorization grant](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)
- [OAuth 2.0 authorization code flow](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-auth-code-flow)

## Configure permissions

Open **API permissions > Add a permission > Microsoft Graph > Delegated
permissions**.

For either a personal account or a work/school account accessing its own
OneDrive, configure these delegated permissions:

```text
User.Read
Files.ReadWrite
```

`User.Read` allows the client to identify the signed-in account and download
its profile photo. The client also requests `offline_access` so Microsoft can
issue a refresh token. A work/school account does not require broader permissions
merely because its Drive type is `business`. For shared resources and SharePoint
features, request additional permissions only when the actual API requires them:

```text
Files.ReadWrite.All
Sites.Read.All
Sites.ReadWrite.All
```

`Sites.Read.All` is sufficient for the read-only `inspect sites QUERY` discovery
command. Writing document-library files does not universally require
`Sites.ReadWrite.All`: some file APIs also accept `Files.ReadWrite.All`. Select
the least privileged permission documented for each operation rather than
requesting every permission above.

For organizations that restrict access to selected sites, `Sites.Selected`
supports delegated access but requires both Entra consent and an explicit
application permission assignment on each target site. The signed-in user's own
permissions still apply. While `Sites.Selected` does not permit tenant-wide
wildcard search (`*`), `inspect sites` and the GUI SharePoint library browser
can resolve an assigned site directly by its SharePoint URL,
`hostname:/sites/path`, or site ID.

The client currently uses one configured `auth.scopes` list for both CLI and
GUI; it does not automatically request extra permissions when a SharePoint
feature is selected. After changing that list, stop any running Monitor and
sign in again to obtain any required consent. In the GUI, log out first to
enable sign-in. An existing refresh token is not a substitute for consent to
new permissions. Feature-triggered incremental authorization is planned, not
implemented.

Organizational tenant policies may require an administrator to grant consent.
Personal Microsoft accounts normally grant consent during device sign-in.
See the
[Microsoft Graph permissions reference](https://learn.microsoft.com/en-us/graph/permissions-reference)
for the current permission definitions.

## Configure onedrive-cpp

Create the user configuration if it does not already exist:

```bash
mkdir -p ~/.config/onedrive-cpp
cp /etc/onedrive-cpp/onedrive-cpp.toml ~/.config/onedrive-cpp/config.toml
sed -i "s|/home/USER|$HOME|g" ~/.config/onedrive-cpp/config.toml
```

For an application registered as **Personal Microsoft accounts only**, use:

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "consumers"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

For an application registered as **Any Entra ID Tenant + Personal Microsoft
accounts**, use `common` even when the user signing in has a personal account:

```toml
[auth]
application_id = "YOUR_APPLICATION_CLIENT_ID"
tenant_id = "common"
endpoint = "https://login.microsoftonline.com"
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

For a single-tenant organizational application, replace `common` with the
tenant's Directory (tenant) ID. For an organizational-only multitenant sign-in,
use `organizations`; `consumers` does not accept work/school accounts.
Add `Files.ReadWrite.All`, `Sites.Read.All`, or
`Sites.ReadWrite.All` only for organizational scenarios that require them;
SharePoint site scopes are not supported for personal Microsoft accounts. The
default scopes are `User.Read Files.ReadWrite offline_access`; authentication
prints a warning when a broader organizational scope is configured.

## Authorize the client

In the GUI, click **Sign in**. Complete sign-in and consent in the system
browser, then return to the application. No device code is shown. The GUI
validates a fresh random state and exchanges the code using a fresh PKCE
verifier. It waits up to five minutes; during login, **Log out** changes to
**Cancel login**. That button or closing the window cancels the wait and joins
the login worker. The browser opens automatically; if opening fails, copy the
displayed authorization URL into your browser. Settings are disabled during login.
Existing credentials are not replaced if authorization fails or is cancelled
before account activation. Once account activation starts, it is allowed to
complete its atomic writes.

When saved credentials are available, **Sign in** is disabled and **Log out**
is enabled. Logging out removes only the active account's locally saved refresh
token, using the same runtime lock and operation as CLI logout. It preserves
synchronized files and account/synchronization state, and does not sign out of
the Microsoft account in your browser. Sign in becomes available again once
the credentials are removed and authentication configuration is valid.

GUI Auth Code and CLI Device Code share the same account refresh token when
using the same state directory and application registration. Signing in again
updates that account's shared credentials; CLI logout also affects the GUI.
Both login services hold the existing state-directory runtime lock through
authorization, token exchange and account activation. CLI operations that
refresh or remove credentials use the same lock. A competing operation fails
explicitly without modifying credentials; stop an active CLI monitor before
logging in from the GUI. Access tokens remain in memory; only the refresh token
is persisted atomically with owner-only (0600) permissions. Do not share a state
directory between configurations using different application IDs.

The Qt-independent Core `AuthCodeSession` is a single-use state machine:
`created → awaiting_callback → authorized → exchanging_token → completed`.
Cancellation, expiry and failure are terminal states. Core owns PKCE secrets,
authorization URL construction and callback validation; the desktop adapter
only opens the browser and receives the loopback HTTP request. Custom protocol
redirect handlers are not implemented yet.

In the CLI, run:

```bash
onedrive-cpp account login
```

By default (`--auth-flow auto`), if a desktop GUI session (`DISPLAY` or
`WAYLAND_DISPLAY` with `xdg-open`) is available on an interactive terminal,
`account login` uses the OAuth 2.0 authorization code + PKCE flow on a local
loopback listener and opens the system browser automatically, falling back to
the device code flow if browser launch or loopback binding is unavailable.
Use `--auth-flow auth-code` or `--auth-flow device-code` to select a flow
explicitly. When using device code flow, open the displayed URL, enter the user code, sign in with the account matching
the registration's supported account type, and review the requested
permissions. On success, the client retrieves the stable user and Drive
identities plus the profile photo. The refresh token and photo are atomically
saved under the friendly account state directory with owner-only permissions.

Drive identity queries also read Graph's `driveType`: `personal` identifies
OneDrive Personal, `business` an organizational user's OneDrive, and
`documentLibrary` a SharePoint document library. The type is saved in
`drive.json` and shown by CLI login/status and the GUI Overview page.
Reading saved metadata does not contact Microsoft. Older metadata without a
type is shown as unknown until a successful login or synchronization writes
the refreshed identity. Existing user/Drive ID directories, tokens and
databases are preserved; display names and Drive types are not identity keys.
`common` and `consumers` are authentication selectors, not actual tenant IDs.
Synchronization for the signed-in user's own organizational OneDrive and
configured document libraries is supported in 1.1; broader SharePoint site and
cross-tenant extensions are tracked in `TODO.md`.

## Organizational authentication diagnostics

CLI login, GUI login and access-token refresh share diagnostic advice. Errors
retain Microsoft's description and, when supplied, the OAuth error and numeric
`AADSTS` codes. Core advice is currently English, including in the GUI; the
surrounding GUI controls follow the selected language.
CLI diagnostics use the existing logging path in both text and JSON output
modes; `--log-level off` suppresses them. They are not a separate JSON
authentication-failure event.

| Diagnostic | Recommended action |
|---|---|
| `AADSTS90094` | Ask the tenant administrator to approve the application's configured delegated permissions. |
| `AADSTS65001` or `consent_required` | Sign in again for consent; administrator approval is needed only if organizational policy requires it. |
| `AADSTS65004` | Consent was declined; review the permissions before authorizing again. |
| `AADSTS50076`, `50079`, `50097`, `53000`–`53003` | Complete required authentication/device checks and ask the administrator to review Conditional Access and approved-device/application requirements. |
| `AADSTS530036` | Conditional Access restricts the grant's authentication flow, such as device code or authentication transfer. Stop Monitor, explicitly log out locally and use GUI browser sign-in only if the organization permits it. |
| `interaction_required` or `login_required` | Stop Monitor and sign in interactively again; log out locally first in the GUI. |
| `invalid_grant`, including expired/revoked grant diagnostics | Stop Monitor and run `onedrive-cpp account login`, or log out locally before GUI sign-in. Failed authentication does not automatically delete saved credentials. |
| `unauthorized_client` or `unsupported_grant_type` | Check the configured tenant, supported account types and **Allow public client flows**. Do not add a client secret. |
| `temporarily_unavailable` or `server_error` | Retry later; do not expand permissions or delete credentials merely because the service is temporarily unavailable. |

Browser sign-in cannot bypass organizational policy. Automatic claims-challenge
handling and device-broker/compliant-device authentication are not implemented.
An unknown diagnostic is not treated as proof of a particular policy failure.
OAuth error identifiers control device polling; numeric STS codes only refine
diagnostic advice and never trigger automatic grant switching or token deletion.
No extra permissions are requested to resolve these errors.

The displayed **Configured tenant/selector (not a verified tenant ID)** comes
from `auth.tenant_id`. In particular, `common`, `organizations` and `consumers`
are selectors, not the actual directory used for sign-in. The client does not
decode an unverified access token to infer that directory. Have the tenant
administrator confirm the actual tenant and policy decision in Microsoft Entra
sign-in logs. Live organizational-tenant synchronization acceptance is covered
by `tests/business_e2e.py`.

See Microsoft's [authentication error reference](https://learn.microsoft.com/en-us/entra/identity-platform/reference-error-codes)
and the specific [530036 diagnostic](https://login.microsoftonline.com/error?code=530036).
Do not share authorization codes, tokens or full callback URLs when reporting
an authentication problem.

## Troubleshooting

If Microsoft reports that the account is unsupported, verify the selected
**Supported account types** and use `consumers` for personal-only
registrations or `common` for combined personal and organizational
registrations.

If the program displays `https://www.microsoft.com/link` and that page
immediately reports that a newly generated code is invalid or expired, check
whether a combined personal and organizational application was incorrectly
configured with `auth.tenant_id = "consumers"`. Change it to:

```toml
[auth]
tenant_id = "common"
```

Start `onedrive-cpp account login` again and use only the newly generated code at the
newly displayed `https://login.microsoft.com/device` URL. Previously generated
device codes cannot be reused. Keep `consumers` only for applications whose
supported account type is **Personal Microsoft accounts only**.

If the device code is accepted and account sign-in begins, but Microsoft then
reports that the code expired while the terminal remains at
`Waiting for authorization...`, remove permissions that are unavailable to
the selected account type. In particular, personal Microsoft accounts should
use:

```toml
[auth]
scopes = ["User.Read", "Files.ReadWrite", "offline_access"]
```

After changing scopes, restart `onedrive-cpp account login`; an existing device code
retains its original scopes and cannot be repaired or reused.

Remove the saved authentication with:

```bash
onedrive-cpp account logout
```
