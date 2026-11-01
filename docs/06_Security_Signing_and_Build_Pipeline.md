# 06 — Security (Running Unsigned Builds Safely) & Standardized Build Pipeline

## 1. Running Unsigned Local Builds Safely on Windows

### 1.1 Why Windows Blocks Downloaded Unsigned Apps vs. Local Builds
Windows uses **SmartScreen**, **Smart App Control**, and **Windows Defender** to block untrusted unsigned executables. Crucially, Windows distinguishes between **downloaded files** and **locally compiled binaries** using an NTFS Alternate Data Stream (ADS) called `Zone.Identifier` (**Mark-of-the-Web / MotW**):
- Any `.exe` or `.dll` downloaded via a browser, email client, or chat app is tagged with `ZoneId=3` (Internet Zone). Windows SmartScreen intercepts and blocks unsigned binaries carrying `ZoneId=3`.
- Binaries compiled locally on your machine by CMake/MSVC (`cl.exe` / `link.exe`) **do not have a `Zone.Identifier` stream** and remain in the Local Machine Zone (`ZoneId=0`).

However, to guarantee that **only AudioTool** (and its dynamically loaded node `.dll` plugins in `%LOCALAPPDATA%\Rouvr\AudioTool\plugins`) can run without warnings—while **never** weakening Windows security for standard downloaded applications—we combine **three defense-in-depth mechanisms**.

---

### 1.2 Mechanism 1: Application-Level DLL Search Order Hardening (Built into AudioTool)
Because AudioTool dynamically loads node plugins (`*.dll`) from `%LOCALAPPDATA%\Rouvr\AudioTool\plugins`, a naive call to `LoadLibraryW` could be vulnerable to **DLL Search Order Hijacking** if an attacker drops a malicious DLL into the working directory or `PATH`.

AudioTool hardens DLL loading at startup by:
1. Calling `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` as the very first statement in `main()`. This permanently strips the current working directory (`CWD`) and unconstrained `PATH` directories from the Windows loader's search order.
2. Loading node plugins **exclusively by canonical absolute path** (`\\?\C:\Users\<User>\AppData\Local\Rouvr\AudioTool\cache\plugin_shadow\...`) using `LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)`.
3. Verifying that no plugin DLL in `%LOCALAPPDATA%\Rouvr\AudioTool\plugins` carries an Internet Mark-of-the-Web (`Zone.Identifier` ADS) before loading it (or requiring explicit user approval/unblocking).

---

### 1.3 Mechanism 2: Local-Only Self-Signed Code Signing (Optional, Highest Security)
If you have **Windows Defender Application Control (WDAC)**, **AppLocker**, or strict **Smart App Control** enabled, you should **not** turn off code-signing enforcement globally. Instead, you can create a **Local Developer Code-Signing Certificate** whose private key exists *only* inside your Windows User Certificate Store:

1. **Why this is secure against downloaded apps:**
   - The private key is generated locally inside your machine's Cryptography Next Generation (CNG) Key Storage Provider and is marked **non-exportable**.
   - Standard downloaded applications from the internet cannot sign themselves with your local private key, so Windows continues blocking unsigned downloads normally.
2. **One-Time PowerShell Setup (Run in Developer PowerShell):**
   ```powershell
   # 1. Create a local-only Code Signing Certificate valid for 5 years
   $cert = New-SelfSignedCertificate `
       -Subject "CN=Rouvr AudioTool Local Build" `
       -Type CodeSigningCert `
       -KeyUsage DigitalSignature `
       -KeyExportPolicy NonExportable `
       -CertStoreLocation "Cert:\CurrentUser\My" `
       -NotAfter (Get-Date).AddYears(5)

   # 2. Trust this certificate in CurrentUser Trusted Root & Trusted Publishers
   $rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "CurrentUser")
   $rootStore.Open("ReadWrite")
   $rootStore.Add($cert)
   $rootStore.Close()

   $pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "CurrentUser")
   $pubStore.Open("ReadWrite")
   $pubStore.Add($cert)
   $pubStore.Close()

   Write-Host "Created Local Build Certificate Thumbprint: $($cert.Thumbprint)"
   ```
3. **Automatic Signing in the Build Pipeline:**
   - When `scripts/build.ps1` (or `make build`) runs, if it detects the `"CN=Rouvr AudioTool Local Build"` certificate in `Cert:\CurrentUser\My`, it automatically signs `at_engine_poc.exe` and all built plugin `.dll` files via `Set-AuthenticodeSignature`.
   - If the certificate is not present, the build runs unsigned cleanly without failing.

---

### 1.4 Mechanism 3: Path-Scoped Windows Defender Exclusion (For Real-Time Audio Latency)
Windows Defender's real-time minifilter driver (`WdFilter.sys`) intercepts file opens. When AudioTool shadow-copies plugin DLLs in `%LOCALAPPDATA%\Rouvr\AudioTool\cache` or streams audio files, Defender scans can cause micro-stalls.
To exclude **only** the AudioTool build output and local data directory (leaving `Downloads`, `Desktop`, `Temp`, and browsers fully protected):
```powershell
# Run in Elevated PowerShell (Optional - Scoped strictly to AudioTool directories)
Add-MpPreference -ExclusionPath "D:\Code\AudioTool\build"
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\Rouvr\AudioTool"
```

---

## 2. File Locations & Windows Registry Specification

### 2.1 User-Configurable Files (`%LOCALAPPDATA%\Rouvr\AudioTool`)
All runtime user-configurable files are stored outside the source repository under `%LOCALAPPDATA%\Rouvr\AudioTool`:

| Directory / File | Full Resolved Path Example | Purpose |
| :--- | :--- | :--- |
| **Root Local Data Dir** | `C:\Users\<User>\AppData\Local\Rouvr\AudioTool` | Base directory for all user-configurable state. |
| **Saved Profiles Dir** | `%LOCALAPPDATA%\Rouvr\AudioTool\profiles\` | JSON workspace profiles (`*.atprof`). |
| **Plugin Nodes Dir** | `%LOCALAPPDATA%\Rouvr\AudioTool\plugins\` | Dynamically loaded node DLLs organized in subfolders (`Audio/Sources`, `Audio/Outputs`, `Audio/Processing`, `Control/Triggers`). |
| **Shadow Cache Dir** | `%LOCALAPPDATA%\Rouvr\AudioTool\cache\plugin_shadow\` | Ephemeral shadow-copied `.dll` files loaded at runtime so original DLLs in `plugins/` remain unlocked. |
| **Engine Config** | `%LOCALAPPDATA%\Rouvr\AudioTool\config.json` | Global engine constants (`buffer_size_frames`, `sample_rate`, `max_connections`). |

All project source code, build scripts, CMake files, and compiled build outputs remain inside the project directory (`D:\Code\AudioTool\`). During the build/deploy step (or on first startup), built-in node DLLs are synced into `%LOCALAPPDATA%\Rouvr\AudioTool\plugins\`.

### 2.2 Windows Registry Convention
Following standard Windows software conventions (`HKEY_CURRENT_USER\Software\<Vendor>\<AppName>`):
- **Registry Key Path:** `HKEY_CURRENT_USER\Software\Rouvr\AudioTool`
- **Registry Values Written on Startup / Install:**
  - `InstallPath` (`REG_SZ`): Absolute path to the directory containing the running executable (e.g., `D:\Code\AudioTool\build\bin`).
  - `LocalDataPath` (`REG_SZ`): Absolute path to the user data directory (`C:\Users\<User>\AppData\Local\Rouvr\AudioTool`).
  - `Version` (`REG_SZ`): Manual semantic version string from `VERSION.txt` (e.g., `"0.1.0-poc"`).
  - `BuildId` (`REG_SZ`): Unique build identifier baked into the binary at build time (e.g., `"20261101-011500-a1b2c3d4"`).

---

## 3. Standardized Build Pipeline (`CMake` + `Makefile` / `build.ps1`)

### 3.1 Version & Unique Build ID Architecture
1. **`VERSION.txt` (Root Project Directory):**
   - Contains the manually maintained semantic version string (e.g., `0.1.0`).
   - Developers edit this file manually whenever bumping the version.
2. **Generated Header (`include/at_version.h`):**
   - Automatically generated prior to every build from `cmake/at_version.h.in` or the build pipeline script.
   - Defines:
     - `AT_VERSION_STRING`: Read from `VERSION.txt` (e.g., `"0.1.0"`).
     - `AT_BUILD_ID`: A unique identifier generated for every build combining UTC timestamp + random/git suffix (e.g., `"BLD-20261101-011542-8f3a9c12"`).
     - `AT_GIT_COMMIT_HASH`: Short git SHA of the committed tree.
     - `AT_BUILD_TIMESTAMP_UTC`: ISO-8601 build timestamp.
   - Compiled directly into the engine library, CLI executable, and every plugin DLL so the application can report and verify its exact `BuildId` at runtime and write it to the Registry.

### 3.2 Standardized Build Workflow (Commit $\rightarrow$ Generate Build ID $\rightarrow$ CMake/Ninja Build $\rightarrow$ Deploy Plugins $\rightarrow$ Test)
For familiarity with Linux `cmake` and `make` workflows, the project provides:
1. **`Makefile`** (usable with GNU `make` or `nmake`) and **`build.bat` / `scripts/build.ps1`**:
   - **Step 1:** Reads manual version from `VERSION.txt` and generates a unique `AT_BUILD_ID`.
   - **Step 2:** Updates `include/at_version.h`.
   - **Step 3:** Stages and commits any pending changes to Git (`git add -A && git commit -m "Build <Version> [<BuildId>]"`).
   - **Step 4:** Runs `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` and `cmake --build build` using the Visual Studio 2022 MSVC x64 toolchain.
   - **Step 5:** Deploys compiled node plugin DLLs to `%LOCALAPPDATA%\Rouvr\AudioTool\plugins\...` and runs `ctest --test-dir build --output-on-failure`.
