@echo off
cd /d "%~dp0"
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /out:Audio_holder_v6_cs.exe /reference:System.Windows.Forms.dll /reference:System.Drawing.dll Audio_holder_v6.cs
if errorlevel 1 (echo BUILD FAILED) else (echo Done: Audio_holder_v6_cs.exe)
pause
