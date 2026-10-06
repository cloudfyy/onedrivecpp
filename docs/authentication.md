# Microsoft authentication

English | [简体中文](authentication.zh-CN.md)

The client uses the OAuth 2.0 Device Authorization Grant. You must register
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

Device code flow does not require a redirect URI. Because this is a public
client, do not add a client secret: a secret embedded in a desktop or CLI
application cannot be kept confidential.

Microsoft's corresponding documentation is:

- [Register an application in Microsoft Entra ID](https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app)
- [Configure desktop and public client applications](https://learn.microsoft.com/en-us/entra/identity-platform/scenario-desktop-app-configuration)
- [OAuth 2.0 device authorization grant](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code)

## Configure permissions

Open **API permissions > Add a permission > Microsoft Graph > Delegated
permissions**.

For a personal OneDrive account, configure these delegated permissions:

```text
User.Read
Files.ReadWrite
```

`User.Read` allows the client to identify the signed-in account and download
its profile photo. The client also requests `offline_access` so Microsoft can
issue a refresh token. For organizational OneDrive, shared libraries, and SharePoint
scenarios, add the broader delegated permissions only when required:

```text
Files.ReadWrite.All
Sites.ReadWrite.All
```

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
tenant's Directory (tenant) ID. Add `Files.ReadWrite.All` or
`Sites.ReadWrite.All` only for organizational scenarios that require them;
`Sites.ReadWrite.All` is not supported for personal Microsoft accounts. The
default scopes are `User.Read Files.ReadWrite offline_access`; authentication
prints a warning when either broader organizational scope is configured.

## Authorize the client

Run the device authorization flow:

```bash
onedrive-cpp auth
```

Open the displayed URL, enter the user code, sign in with the account matching
the registration's supported account type, and review the requested
permissions. On success, the client retrieves the stable user and Drive
identities plus the profile photo. The refresh token and photo are atomically
saved under the friendly account state directory with owner-only permissions.

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

Start `onedrive-cpp auth` again and use only the newly generated code at the
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

After changing scopes, restart `onedrive-cpp auth`; an existing device code
retains its original scopes and cannot be repaired or reused.

Remove the saved authentication with:

```bash
onedrive-cpp logout
```
