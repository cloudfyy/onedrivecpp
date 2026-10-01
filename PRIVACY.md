# onedrive-cpp Privacy Statement

**Effective date:** October 1, 2026

This Privacy Statement describes how onedrive-cpp (the "Software") handles
information. onedrive-cpp is an independent, open-source client that runs on
your device and is intended to communicate directly with Microsoft services.

This statement is a project-maintainer draft and is not legal advice. Adapt
it before offering a hosted or commercial service or adding telemetry,
accounts, or project-operated infrastructure.

## 1. Summary

- The maintainers do not operate an onedrive-cpp cloud service.
- The Software does not include project-operated analytics or telemetry.
- Authentication credentials and synchronization state are stored locally on
  your device.
- Account authorization and OneDrive operations communicate with Microsoft.
- Microsoft handles information under its own privacy terms.

## 2. Information processed by the Software

Depending on the features you use, the Software may process:

- configuration values, including local paths, account/tenant settings, and
  requested permission scopes;
- OAuth access and refresh tokens issued by Microsoft;
- OneDrive file and folder names, identifiers, metadata, and content;
- local file and folder names, metadata, and content;
- synchronization state and operational error information.

The Software processes this information to authenticate your account, access
the files you direct it to access, maintain synchronization state, and report
operational results.

## 3. Local storage

By default, the Software stores state under
`~/.local/state/onedrive-cpp`. The OAuth refresh token is stored in a local
file with owner-only `0600` permissions. Synchronization metadata may be
stored in a local SQLite database.

Anyone who can access your operating-system account, backups, storage device,
or state directory may be able to access this information. You are
responsible for securing your device and backups.

## 4. Network communications

The Software communicates with Microsoft identity endpoints to authenticate
your account and with Microsoft Graph/OneDrive endpoints to perform requested
operations. Information necessary for those operations is transmitted to
Microsoft over HTTPS.

The maintainers do not receive your Microsoft password. Password entry and
account consent occur on Microsoft-operated pages. The maintainers do not
receive your OAuth tokens or file contents unless you separately choose to
send them, which you should not do.

Microsoft's handling of information is governed by the
[Microsoft Privacy Statement](https://privacy.microsoft.com/privacystatement)
and the terms applicable to your Microsoft account.

## 5. Analytics, telemetry, and logs

The Software does not currently send analytics, advertising identifiers,
crash reports, or telemetry to the project maintainers.

The Software may write operational messages to your terminal or local system
logs. Your operating system, package distributor, network administrator, or
Microsoft may independently retain logs under their own policies.

## 6. Sharing and sale

The maintainers do not sell your personal information. The Software does not
send your information to the maintainers.

Information is disclosed to Microsoft only as needed to use Microsoft
identity, Graph, and OneDrive services. Information may also be accessible to
people or services you authorize on your device, such as system
administrators, backup software, or log collectors.

## 7. Retention and deletion

Locally stored information remains until you remove it, uninstall the
Software, or delete its state directory. Running `onedrive-cpp logout` removes
the locally stored refresh token but does not necessarily delete other local
state or revoke Microsoft-side consent.

To stop future authorized access:

1. run `onedrive-cpp logout`;
2. revoke the application's permissions in your Microsoft account;
3. delete local state that you no longer need.

Microsoft may retain information according to its own policies and your
account settings.

## 8. Security

The project uses measures such as HTTPS endpoints, owner-only refresh-token
permissions, and protection against symbolic-link token files. No security
measure is perfect. Keep the Software and operating system updated, restrict
access to your account, and do not publish configuration files or tokens.

## 9. Children's privacy

The Software is not directed to children. Account eligibility and parental
consent are governed by Microsoft account terms and applicable law.

## 10. Changes to this statement

This statement may be updated when the Software's data practices change. The
effective date at the top identifies the current version. Material additions,
such as telemetry or project-operated services, should be documented before
release.

## 11. Contact

Privacy questions may be submitted through the project's public
[GitHub issue tracker](https://github.com/cloudfyy/onedrivecpp/issues). Do not
post passwords, OAuth tokens, private file names or contents, or other
sensitive information in a public issue.
