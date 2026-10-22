# Code signing with SignPath Foundation

Signed installers stop Windows from showing "Windows protected your PC" (SmartScreen can still warn for a short time after a brand-new release while it builds reputation) and keep Smart App Control from blocking PSDBhai on Windows 11.

SignPath Foundation signs open-source projects for free. The certificate is issued to "SignPath Foundation", so that is the publisher Windows shows. Their rules: <https://signpath.org/terms>.

## 1. Before applying (one time)

- [ ] **Make the repository public:** GitHub > PSDBhai > Settings > General > Danger Zone > Change repository visibility > Public.
- [ ] **Turn on two-factor authentication** for your GitHub account (Settings > Password and authentication). SignPath requires it, and you'll need it for your SignPath account too.
- [ ] **Publish the first release (v1.0.0, unsigned).** Push the tag `v1.0.0`; the workflow builds, tests and publishes the release. SignPath only signs software that is already released.
- [ ] **Gather some reputation.** The application asks for evidence that the project is used or trusted (download counts, GitHub stars, posts or discussions about it). Share PSDBhai where Photoshop and Windows users talk (for example Reddit, design communities, your own channels) and keep the links.

## 2. Apply at <https://signpath.org/apply>

You fill this in yourself: it has a reCAPTCHA and asks for your personal details. Suggested answers:

| Field | Answer |
| --- | --- |
| Project Name | PSDBhai |
| Repository URL | https://github.com/Muhon33/PSDBhai |
| Homepage URL | https://github.com/Muhon33/PSDBhai |
| Download URL | https://github.com/Muhon33/PSDBhai#download |
| Privacy Policy URL | https://github.com/Muhon33/PSDBhai#privacy-policy |
| Tagline | Shows thumbnails of Photoshop PSD and PSB files in Windows File Explorer. |
| Category | The closest match in the list (for example graphics or utilities) |
| Description | PSDBhai is a small Windows shell extension that shows real previews of Photoshop PSD and PSB files in File Explorer and in every app's Open and Save dialogs, the way macOS Finder does. It reads the preview Photoshop embeds in the file, or decodes the merged image at reduced size, inside Windows' isolated thumbnail process. It has no dependencies, makes no network connections, and is MIT licensed. |
| Reputation | Your download counts (the release page shows them), stars, and links to any posts or discussions. |
| Maintainer Type | Individual |
| Build System | GitHub Actions |
| First/Last Name, Email | Yours |

## 3. After SignPath approves

SignPath sets up an organization for you. In SignPath:

- [ ] **Project:** create (or confirm) a project with slug `psdbhai`, linked to the repository.
- [ ] **Trusted build system:** link the predefined **GitHub.com** trusted build system to the project.
- [ ] **Artifact configurations:** create two, with these slugs, pasting the XML from this repository:
  - `binaries`: [.signpath/artifact-configurations/binaries.xml](../.signpath/artifact-configurations/binaries.xml)
  - `installer`: [.signpath/artifact-configurations/installer.xml](../.signpath/artifact-configurations/installer.xml)
- [ ] **Signing policy:** slug `release-signing`, with yourself as approver.
- [ ] **CI user:** create a user with the Submitter role on that policy and generate an API token for it.

In GitHub (PSDBhai > Settings > Secrets and variables > Actions):

- [ ] Secret `SIGNPATH_API_TOKEN` = the API token.
- [ ] Variable `SIGNPATH_ORGANIZATION_ID` = your SignPath organization ID.
- [ ] Only if SignPath gave you different slugs: variables `SIGNPATH_PROJECT_SLUG` and `SIGNPATH_SIGNING_POLICY_SLUG`.

## 4. Every release

1. Change `VERSION` (for example to `1.0.1`), commit and push.
2. `git tag v1.0.1` then `git push origin v1.0.1`.
3. SignPath sends two signing requests: the program files, then the installer. Approve each one in SignPath. The workflow waits up to 4 hours for each.
4. The workflow publishes the release with the signed `PSDBhai-Setup-1.0.1.exe`.

## If SignPath declines

Buy a code-signing certificate in your own name instead, for example from Certum (around €49 per year; they verify your ID). Then sign locally:

```powershell
.\build.ps1 -Installer -SignCommand 'signtool sign /fd sha256 /tr <timestamp-url> /td sha256 /a {file}'
```
