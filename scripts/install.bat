@echo off
setlocal EnableExtensions
title OtonumaASIO インストール

rem ============================================================
rem  OtonumaASIO インストール
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
set "SRC=%~dp0"
rem 32bit のコマンドプロンプトで動いても 64bit 側の Program Files に入れる
set "PF=%ProgramFiles%"
if defined ProgramW6432 set "PF=%ProgramW6432%"
set "DEST=%PF%\OtonumaASIO"
set "MENU=%ProgramData%\Microsoft\Windows\Start Menu\Programs\OtonumaASIO"
set "RUNKEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Run"

echo.
echo  OtonumaASIO をインストールします
echo  インストール先: %DEST%
echo.

rem --- 必要なファイルがそろっているか ---
for %%F in (OtonumaASIO64.dll OtonumaASIO32.dll OtonumaPanel.exe otonumareg.exe) do (
    if not exist "%SRC%%%F" (
        echo [エラー] %%F が見つかりません。zip を展開したフォルダの中で実行してください。
        pause
        exit /b 1
    )
)

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

rem --- ファイルをコピー ---
if /i "%SRC%"=="%DEST%\" goto :copied
if not exist "%DEST%" mkdir "%DEST%"
set "FAIL="
for %%F in (OtonumaASIO64.dll OtonumaASIO32.dll OtonumaPanel.exe otonumareg.exe asiohosttest.exe asiohosttest32.exe manual.html LICENSE README.md CHANGELOG.md install.bat uninstall.bat) do (
    if exist "%SRC%%%F" (
        copy /y "%SRC%%%F" "%DEST%\" >nul 2>&1
        if errorlevel 1 set "FAIL=1"
    )
)
if defined FAIL (
    echo [エラー] ファイルをコピーできませんでした。
    pause
    exit /b 1
)
echo  ファイルをコピーしました。
:copied

rem --- ASIO ドライバとして登録 ---
"%DEST%\otonumareg.exe" install
set "RC=%errorlevel%"
chcp 932 >nul 2>&1
if not "%RC%"=="0" (
    echo [エラー] ASIO ドライバの登録に失敗しました。
    pause
    exit /b 1
)

rem --- スタートメニューにショートカット ---
if not exist "%MENU%" mkdir "%MENU%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=New-Object -ComObject WScript.Shell; $l=$s.CreateShortcut('%MENU%\OtonumaASIO コントロールパネル.lnk'); $l.TargetPath='%DEST%\OtonumaPanel.exe'; $l.WorkingDirectory='%DEST%'; $l.Save(); $l=$s.CreateShortcut('%MENU%\OtonumaASIO マニュアル.lnk'); $l.TargetPath='%DEST%\manual.html'; $l.Save(); $l=$s.CreateShortcut('%MENU%\OtonumaASIO アンインストール.lnk'); $l.TargetPath='%DEST%\uninstall.bat'; $l.WorkingDirectory='%DEST%'; $l.Save()" >nul 2>&1
if exist "%MENU%\OtonumaASIO コントロールパネル.lnk" (
    echo  スタートメニューにショートカットを作りました。
) else (
    echo  [注意] スタートメニューのショートカットを作れませんでした。
)

rem --- Windows の起動時に常駐 ---
echo.
choice /c YN /m " Windows の起動時にコントロールパネルをタスクトレイに常駐させますか"
if errorlevel 2 (
    reg delete "%RUNKEY%" /v OtonumaASIOPanel /f >nul 2>&1
) else (
    reg add "%RUNKEY%" /v OtonumaASIOPanel /t REG_SZ /d "\"%DEST%\OtonumaPanel.exe\" /tray" /f >nul
    echo  常駐を設定しました。
)

rem --- コントロールパネルを起動（管理者ではない通常の権限で） ---
start "" explorer.exe "%DEST%\OtonumaPanel.exe"

echo.
echo  インストールが完了しました。
echo  再生ソフトの出力先に「OtonumaASIO」を選んでください。
echo  使い方は スタートメニュー → OtonumaASIO マニュアル を見てください。
echo.
pause
exit /b 0
