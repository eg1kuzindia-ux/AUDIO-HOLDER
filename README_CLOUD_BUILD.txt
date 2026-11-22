AUDIO HOLDER v6.1 - CLOUD BUILD (NO LOCAL C++ TOOLCHAIN REQUIRED)

This package includes a GitHub Actions workflow that compiles the EXE on a
GitHub-hosted Windows runner. You do not need Visual Studio or C++ Build Tools
installed on your own PC.

HOW TO BUILD
1. Sign in to GitHub and create a new repository (public or private).
2. Upload the contents of this folder to the repository, preserving the
   .github/workflows/build.yml path. GitHub's web upload interface may hide
   dot-folders; if so, create the workflow file through Add file > Create new
   file and enter .github/workflows/build.yml as its name.
3. Open the repository's Actions tab.
4. Select "Build Audio Holder v6.1" in the left sidebar.
5. Click "Run workflow" and confirm.
6. When the run finishes successfully, open it and download the
   "AudioHolder_v6_1-windows-x64" artifact.
7. Extract the artifact ZIP; it contains AudioHolder_v6_1.exe.

The workflow uses GitHub's windows-2022 hosted runner and the MSVC compiler
already available there. No compiler is installed on your local PC.

NOTES
- GitHub-hosted runner availability and usage limits depend on repository
  visibility and your GitHub plan. Check GitHub Actions usage/pricing if using
  a private repository.
- This produces an x64 Windows executable.
- v6.1 includes more defensive WinMM cleanup: it checks reset, unprepare, and
  close results and retains resources/handle when cleanup fails, allowing
  subsequent timer ticks to retry rather than discarding a possibly-live handle.
- This change is intended to address the reported second-activation failure,
  but has not been compiled with MSVC or verified against the target audio driver.

BUILD COMMAND NOTE
The source filename AudioHolder_v6_1.cpp must appear before /link in the MSVC
command. The included workflow and batch file use the corrected argument order.
