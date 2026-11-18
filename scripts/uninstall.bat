@echo off
setlocal EnableExtensions
title OtonumaASIO アンインストール

rem ============================================================
rem  OtonumaASIO アンインストール
rem  ダブルクリックで実行できます（管理者として自動で実行し直します）
rem ============================================================

rem --- 管理者として実行し直す ---
if defined OTO_TEST_MODE goto :admin
fltmc >nul 2>&1
if not errorlevel 1 goto :admin
if /i "%~1"=="/elevated" (
    echo 管理者権限で実行できませんでした。
    echo このファイルを右クリックして「管理者として実行」を選んでください。
    pause
    exit /b 1
)
echo 管理者権限の確認を表示します...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '/elevated' -Verb RunAs"
exit /b

:admin
rem 32bit のコマンドプロンプトで動いても 64bit 側の Program Files に入れる
set "PF=%ProgramFiles%"
if defined ProgramW6432 set "PF=%ProgramW6432%"
set "DEST=%PF%\OtonumaASIO"
set "MENU=%ProgramData%\Microsoft\Windows\Start Menu\Programs\OtonumaASIO"
set "RUNKEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Run"

rem --- インストール先から起動されたときは、一時フォルダにコピーして実行し直す（フォルダごと消せるように） ---
if /i "%~dp0"=="%DEST%\" (
    copy /y "%~f0" "%TEMP%\otonuma_uninstall.bat" >nul
    cd /d "%TEMP%"
    "%TEMP%\otonuma_uninstall.bat" /elevated
)

echo.
echo  OtonumaASIO をアンインストールします
echo  対象: %DEST%
echo.
choice /c YN /m " 続けますか"
if errorlevel 2 exit /b 0

rem --- コントロールパネルを閉じる ---
taskkill /im OtonumaPanel.exe /f >nul 2>&1
ping -n 2 127.0.0.1 >nul

rem --- 再生ソフトがドライバを使用中でないか ---
if defined OTO_TEST_MODE goto :skiplock
for %%F in (OtonumaASIO64.dll OtonumaASIO32.dll) do (
    if exist "%DEST%\%%F" (
        2>nul (type nul >> "%DEST%\%%F") || (
            echo [エラー] %%F を再生ソフトが使用中です。
            echo 再生ソフトをすべて終了してから、もう一度実行してください。
            pause
            exit /b 1
        )
    )
)
:skiplock

rem --- 旧名 BareStream Local の片付け（入っていれば） ---
set "OLDDEST=%PF%\BareStream Local"
set "OLDMENU=%ProgramData%\Microsoft\Windows\Start Menu\Programs\BareStream Local"
set "OLDFOUND="
reg query "HKLM\SOFTWARE\ASIO\BareStream Local" /reg:64 >nul 2>&1 && set "OLDFOUND=1"
reg query "HKLM\SOFTWARE\ASIO\BareStream Local" /reg:32 >nul 2>&1 && set "OLDFOUND=1"
if exist "%OLDDEST%" set "OLDFOUND=1"
if exist "%OLDMENU%" set "OLDFOUND=1"
if defined OLDFOUND (
    taskkill /im BSPanel.exe /f >nul 2>&1
    reg delete "HKLM\SOFTWARE\ASIO\BareStream Local" /f /reg:64 >nul 2>&1
    reg delete "HKLM\SOFTWARE\ASIO\BareStream Local" /f /reg:32 >nul 2>&1
    reg delete "HKLM\SOFTWARE\Classes\CLSID\{6A1F3C9E-4B27-4D85-9E0A-2C7B51D8F304}" /f /reg:64 >nul 2>&1
    reg delete "HKLM\SOFTWARE\Classes\CLSID\{6A1F3C9E-4B27-4D85-9E0A-2C7B51D8F304}" /f /reg:32 >nul 2>&1
    reg delete "%RUNKEY%" /v BareStreamLocalPanel /f >nul 2>&1
    if exist "%OLDMENU%" rd /s /q "%OLDMENU%" >nul 2>&1
    if exist "%OLDDEST%" rd /s /q "%OLDDEST%" >nul 2>&1
    if exist "%OLDDEST%" (
        echo  [注意] 旧版のフォルダ "%OLDDEST%" を削除できませんでした。再生ソフトを終了してから手で削除してください。
    ) else (
        echo  旧名 BareStream Local の登録・常駐・ショートカット・フォルダを片付けました。
    )
)

rem --- ASIO ドライバの登録を解除 ---
set "REG=%DEST%\otonumareg.exe"
if not exist "%REG%" set "REG=%~dp0otonumareg.exe"
if exist "%REG%" (
    "%REG%" uninstall
    chcp 932 >nul 2>&1
) else (
    echo  [注意] otonumareg.exe が見つからないため、登録の解除を省略しました。
)

rem --- 常駐とスタートメニューを削除 ---
reg delete "%RUNKEY%" /v OtonumaASIOPanel /f >nul 2>&1
if exist "%MENU%" rd /s /q "%MENU%"
echo  常駐の設定とスタートメニューのショートカットを削除しました。

rem --- 設定ファイル ---
echo.
choice /c YN /m " 設定とプリセット（%%APPDATA%%\OtonumaASIO\settings.json）も削除しますか"
if not errorlevel 2 (
    del /q "%APPDATA%\OtonumaASIO\settings.json" >nul 2>&1
    del /q "%APPDATA%\OtonumaASIO\settings.json.tmp" >nul 2>&1
    rd "%APPDATA%\OtonumaASIO" >nul 2>&1
    echo  設定を削除しました。
)

echo.
echo  アンインストールが完了しました。
echo.
pause
cd /d "%TEMP%"
rd /s /q "%DEST%" >nul 2>&1
if exist "%DEST%" (
    echo  [注意] インストール先のフォルダを削除できませんでした。手で削除してください:
    echo         %DEST%
    pause
)
exit /b 0
