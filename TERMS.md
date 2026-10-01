# onedrive-cpp Terms of Service

**Effective date:** October 1, 2026

These Terms of Service ("Terms") govern your use of onedrive-cpp (the
"Software"), an independent, open-source client intended to interact with
Microsoft OneDrive through Microsoft Graph.

These Terms are a project-maintainer draft and are not legal advice. Adapt
them to your organization, jurisdiction, and release model before offering a
hosted or commercial service.

## 1. Acceptance

By downloading, installing, configuring, or using the Software, you agree to
these Terms. If you do not agree, do not use the Software.

## 2. Open-source license

The source code is licensed under the GNU General Public License version 3 or,
at your option, any later version (GPL-3.0-or-later). The GPL governs your
rights to copy, modify, and distribute the Software. These Terms govern your
use of the Software and do not restrict rights granted by the GPL.

## 3. Microsoft account and third-party services

The Software relies on Microsoft identity services, Microsoft Graph, and
OneDrive. You must have a valid Microsoft account and comply with Microsoft's
applicable terms and policies.

Microsoft services are provided by Microsoft, not by the onedrive-cpp
maintainers. Microsoft may change, restrict, suspend, or discontinue its APIs
or services at any time. The maintainers do not control and are not
responsible for Microsoft services.

Microsoft, OneDrive, Microsoft Graph, and related marks are trademarks of
Microsoft Corporation. onedrive-cpp is an independent project and is not
affiliated with, endorsed by, or sponsored by Microsoft.

## 4. Permissions and authorization

The Software requests only the Microsoft account permissions shown during the
Microsoft consent process. You are responsible for reviewing those
permissions before accepting them.

The Software may store an OAuth refresh token locally so that it can maintain
authorized access without requiring you to sign in for every operation. You
may remove the local token with the Software's `logout` command and may revoke
the application's access from your Microsoft account settings.

## 5. Acceptable use

You agree not to use the Software:

- in violation of applicable law or another party's rights;
- to gain unauthorized access to accounts, files, systems, or services;
- to distribute malware or harmful content;
- to interfere with Microsoft services or bypass their security controls;
- in a way that violates Microsoft service terms or API usage policies.

## 6. Your data and responsibilities

You retain ownership of your files and other content. You are responsible for:

- choosing appropriate synchronization settings and permissions;
- maintaining independent backups of important data;
- protecting your device, configuration, and locally stored credentials;
- reviewing operations before using experimental or pre-release features;
- complying with laws and obligations that apply to your data.

File synchronization software can create, modify, move, or delete files. Do
not rely on the Software as your only copy or backup.

## 7. Updates and compatibility

The Software may change without notice. Updates may alter features,
configuration, storage formats, or compatibility. Microsoft API changes may
also require updates. You are responsible for reviewing release notes and
testing updates before using them with important data.

## 8. No warranty

THE SOFTWARE IS PROVIDED "AS IS" AND "AS AVAILABLE", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING WARRANTIES OF MERCHANTABILITY, FITNESS FOR
A PARTICULAR PURPOSE, TITLE, AND NON-INFRINGEMENT. THERE IS NO GUARANTEE THAT
THE SOFTWARE WILL BE SECURE, ERROR-FREE, UNINTERRUPTED, OR COMPATIBLE WITH
EVERY ACCOUNT OR SYSTEM.

## 9. Limitation of liability

TO THE MAXIMUM EXTENT PERMITTED BY LAW, THE MAINTAINERS AND CONTRIBUTORS WILL
NOT BE LIABLE FOR ANY INDIRECT, INCIDENTAL, SPECIAL, CONSEQUENTIAL, EXEMPLARY,
OR PUNITIVE DAMAGES, OR FOR LOSS OF DATA, PROFITS, REVENUE, BUSINESS, OR
GOODWILL, ARISING FROM OR RELATED TO THE SOFTWARE OR THESE TERMS.

Nothing in these Terms excludes liability that cannot legally be excluded.

## 10. Suspension and termination

You may stop using the Software at any time. You should use `logout`, remove
local application data if desired, and revoke the application's access in
your Microsoft account.

Your authorization may stop working if you revoke consent, Microsoft expires
or invalidates credentials, an administrator changes policy, or the
application registration is disabled.

## 11. Changes to these Terms

These Terms may be updated as the project changes. The effective date at the
top identifies the current version. Continued use after an update constitutes
acceptance of the revised Terms to the extent permitted by law.

## 12. Contact

Questions about these Terms may be submitted through the project's public
[GitHub issue tracker](https://github.com/cloudfyy/onedrivecpp/issues). Do not
include passwords, OAuth tokens, private file contents, or other sensitive
information in an issue.
