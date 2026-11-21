@echo off
setlocal
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Microsoft C++ Build Tools were not found.
  echo Run this from a Visual Studio Developer Command Prompt, or install Visual Studio Build Tools with the Desktop development with C++ workload.
  pause
  exit /b 1
)
cl.exe /nologo /O1 /Os /GL /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNDEBUG AudioHolder_v6.cpp /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /OUT:AudioHolder_v6.exe winmm.lib shell32.lib comctl32.lib advapi32.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
echo.
echo Build successful: AudioHolder_v6.exe
endlocal
