// settings.h - 設定 (%APPDATA%\OtonumaASIO\settings.json) の読み書き
#pragma once
#include "bs_common.h"
#include "json_mini.h"
#include <string>
#include <vector>

std::string  bs_to_utf8(const std::wstring& w);
std::wstring bs_from_utf8(const std::string& s);

std::wstring bs_settings_path();               // %APPDATA%\OtonumaASIO\settings.json (旧 BareStream Local の設定があれば初回に引き継ぐ)
bool bs_load_document(Json& doc);              // ファイル全体 (current / presets / devices)
bool bs_save_document(const Json& doc);

Json bs_settings_to_json(const BsSettings& s);
void bs_settings_from_json(const Json& j, BsSettings& s);
bool bs_load_settings(BsSettings& s);          // current を読む (無ければ既定値)
bool bs_save_settings(const BsSettings& s);    // current だけ書き換える

// "key=value" 形式で 1 項目変更。範囲外は丸める。不明キーは false。
bool bs_settings_set(BsSettings& s, const std::string& key, const std::string& value);
// 2 つの設定の差から、反映に必要な区分 (最大のもの) を返す
int  bs_settings_diff_class(const BsSettings& a, const BsSettings& b);
void bs_settings_clamp(BsSettings& s);
// 出力方式ごとのデバイス ID
std::wstring bs_settings_device(const BsSettings& s);
