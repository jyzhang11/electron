// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/web_contents_preferences.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "atom/browser/native_window.h"
#include "atom/browser/web_view_manager.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "atom/common/options_switches.h"
#include "base/command_line.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/base/switches.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/web_preferences.h"
#include "native_mate/dictionary.h"
#include "net/base/filename_util.h"
#include "services/service_manager/sandbox/switches.h"

#if defined(OS_WIN)
#include "ui/gfx/switches.h"
#endif

DEFINE_WEB_CONTENTS_USER_DATA_KEY(atom::WebContentsPreferences);

namespace {

bool GetAsString(const base::Value* val,
                 const base::StringPiece& path,
                 std::string* out) {
  if (val) {
    auto* found = val->FindKeyOfType(path, base::Value::Type::STRING);
    if (found) {
      *out = found->GetString();
      return true;
    }
  }
  return false;
}

bool GetAsString(const base::Value* val,
                 const base::StringPiece& path,
                 base::string16* out) {
  if (val) {
    auto* found = val->FindKeyOfType(path, base::Value::Type::STRING);
    if (found) {
      *out = base::UTF8ToUTF16(found->GetString());
      return true;
    }
  }
  return false;
}

bool GetAsInteger(const base::Value* val,
                  const base::StringPiece& path,
                  int* out) {
  if (val) {
    auto* found = val->FindKey(path);
    if (found && found->is_int()) {
      *out = found->GetInt();
      return true;
    } else if (found && found->is_string()) {
      return base::StringToInt(found->GetString(), out);
    }
  }
  return false;
}

}  // namespace

namespace atom {

// static
std::vector<WebContentsPreferences*> WebContentsPreferences::instances_;

WebContentsPreferences::WebContentsPreferences(
    content::WebContents* web_contents,
    const mate::Dictionary& web_preferences)
    : web_contents_(web_contents) {
  v8::Isolate* isolate = web_preferences.isolate();
  mate::Dictionary copied(isolate, web_preferences.GetHandle()->Clone());
  // Following fields should not be stored.
  copied.Delete("embedder");
  copied.Delete("isGuest");
  copied.Delete("session");

  mate::ConvertFromV8(isolate, copied.GetHandle(), &preference_);
  web_contents->SetUserData(UserDataKey(), base::WrapUnique(this));

  instances_.push_back(this);

  preference_.SetKey(options::kNodeIntegrationWasExplicitlyEnabled,
                     base::Value(IsEnabled(options::kNodeIntegration)));
  preference_.SetKey(options::kContextIsolationWasExplicitlyDisabled,
                     base::Value(!IsEnabled(options::kContextIsolation, true)));
  preference_.SetKey(
      options::kWebviewTagWasExplicitlyEnabled,
      base::Value(IsEnabled(options::kWebviewTag,
                            IsEnabled(options::kNodeIntegration))));

  // Set WebPreferences defaults onto the JS object
  SetDefaultBoolIfUndefined(options::kPlugins, false);
  SetDefaultBoolIfUndefined(options::kExperimentalFeatures, false);
  bool node = SetDefaultBoolIfUndefined(options::kNodeIntegration, true);
  SetDefaultBoolIfUndefined(options::kNodeIntegrationInWorker, false);
  SetDefaultBoolIfUndefined(options::kWebviewTag, node);
  SetDefaultBoolIfUndefined(options::kSandbox, false);
  SetDefaultBoolIfUndefined(options::kNativeWindowOpen, false);
  SetDefaultBoolIfUndefined(options::kContextIsolation, false);
  SetDefaultBoolIfUndefined("javascript", true);
  SetDefaultBoolIfUndefined("images", true);
  SetDefaultBoolIfUndefined("textAreasAreResizable", true);
  SetDefaultBoolIfUndefined("webgl", true);
  bool webSecurity = true;
  SetDefaultBoolIfUndefined(options::kWebSecurity, webSecurity);
  // If webSecurity was explicity set to false, let's inherit that into
  // insecureContent
  if (web_preferences.Get(options::kWebSecurity, &webSecurity) &&
      !webSecurity) {
    SetDefaultBoolIfUndefined(options::kAllowRunningInsecureContent, true);
  } else {
    SetDefaultBoolIfUndefined(options::kAllowRunningInsecureContent, false);
  }
#if defined(OS_MACOSX)
  SetDefaultBoolIfUndefined(options::kScrollBounce, false);
#endif
  SetDefaultBoolIfUndefined(options::kOffscreen, false);

  SetDefaults();

  last_preference_ = preference_.Clone();
}

WebContentsPreferences::~WebContentsPreferences() {
  instances_.erase(std::remove(instances_.begin(), instances_.end(), this),
                   instances_.end());
}

void WebContentsPreferences::SetDefaults() {
  if (IsEnabled(options::kSandbox)) {
    SetBool(options::kNativeWindowOpen, true);
  }
}

bool WebContentsPreferences::SetDefaultBoolIfUndefined(
    const base::StringPiece& key,
    bool val) {
  auto* current_value =
      preference_.FindKeyOfType(key, base::Value::Type::BOOLEAN);
  if (current_value) {
    return current_value->GetBool();
  } else {
    preference_.SetKey(key, base::Value(val));
    return val;
  }
}

void WebContentsPreferences::SetBool(const base::StringPiece& key, bool value) {
  preference_.SetKey(key, base::Value(value));
}

bool WebContentsPreferences::IsEnabled(const base::StringPiece& name,
                                       bool default_value) const {
  auto* current_value =
      preference_.FindKeyOfType(name, base::Value::Type::BOOLEAN);
  if (current_value)
    return current_value->GetBool();
  return default_value;
}

void WebContentsPreferences::Merge(const base::DictionaryValue& extend) {
  if (preference_.is_dict())
    static_cast<base::DictionaryValue*>(&preference_)->MergeDictionary(&extend);

  SetDefaults();
}

void WebContentsPreferences::Clear() {
  if (preference_.is_dict())
    static_cast<base::DictionaryValue*>(&preference_)->Clear();
}

bool WebContentsPreferences::GetPreference(const base::StringPiece& name,
                                           std::string* value) const {
  return GetAsString(&preference_, name, value);
}

bool WebContentsPreferences::IsRemoteModuleEnabled() const {
  return IsEnabled(options::kEnableRemoteModule, true);
}

bool WebContentsPreferences::GetPreloadPath(
    base::FilePath::StringType* path) const {
  DCHECK(path);
  base::FilePath::StringType preload;
  if (GetAsString(&preference_, options::kPreloadScript, &preload)) {
    if (base::FilePath(preload).IsAbsolute()) {
      *path = std::move(preload);
      return true;
    } else {
      LOG(ERROR) << "preload script must have absolute path.";
    }
  } else if (GetAsString(&preference_, options::kPreloadURL, &preload)) {
    // Translate to file path if there is "preload-url" option.
    base::FilePath preload_path;
    if (net::FileURLToFilePath(GURL(preload), &preload_path)) {
      *path = std::move(preload_path.value());
      return true;
    } else {
      LOG(ERROR) << "preload url must be file:// protocol.";
    }
  }
  return false;
}

// static
content::WebContents* WebContentsPreferences::GetWebContentsFromProcessID(
    int process_id) {
  for (WebContentsPreferences* preferences : instances_) {
    content::WebContents* web_contents = preferences->web_contents_;
    if (web_contents->GetMainFrame()->GetProcess()->GetID() == process_id)
      return web_contents;
  }
  return nullptr;
}

// static
WebContentsPreferences* WebContentsPreferences::From(
    content::WebContents* web_contents) {
  if (!web_contents)
    return nullptr;
  return FromWebContents(web_contents);
}

void WebContentsPreferences::AppendCommandLineSwitches(
    base::CommandLine* command_line) {
  // Append UA Override
  command_line->AppendSwitchASCII("user-agent",
                                  content::GetContentClient()->GetUserAgent());
  // Check if plugins are enabled.
  if (IsEnabled(options::kPlugins))
    command_line->AppendSwitch(switches::kEnablePlugins);

  // Experimental flags.
  if (IsEnabled(options::kExperimentalFeatures))
    command_line->AppendSwitch(
        ::switches::kEnableExperimentalWebPlatformFeatures);

  // Check if we have node integration specified.
  bool enable_node_integration = IsEnabled(options::kNodeIntegration, true);
  command_line->AppendSwitchASCII(switches::kNodeIntegration,
                                  enable_node_integration ? "true" : "false");

  // Whether to enable node integration in Worker.
  if (IsEnabled(options::kNodeIntegrationInWorker))
    command_line->AppendSwitch(switches::kNodeIntegrationInWorker);

  // Check if webview tag creation is enabled, default to nodeIntegration value.
  // TODO(kevinsawicki): Default to false in 2.0
  bool webview_tag = IsEnabled(options::kWebviewTag, enable_node_integration);
  command_line->AppendSwitchASCII(switches::kWebviewTag,
                                  webview_tag ? "true" : "false");

  // If the `sandbox` option was passed to the BrowserWindow's webPreferences,
  // pass `--enable-sandbox` to the renderer so it won't have any node.js
  // integration.
  if (IsEnabled(options::kSandbox))
    command_line->AppendSwitch(switches::kEnableSandbox);
  else if (!command_line->HasSwitch(switches::kEnableSandbox))
    command_line->AppendSwitch(service_manager::switches::kNoSandbox);

  // Check if nativeWindowOpen is enabled.
  if (IsEnabled(options::kNativeWindowOpen))
    command_line->AppendSwitch(switches::kNativeWindowOpen);

  // The preload script.
  base::FilePath::StringType preload;
  if (GetPreloadPath(&preload))
    command_line->AppendSwitchNative(switches::kPreloadScript, preload);

  // Custom args for renderer process
  auto* customArgs =
      preference_.FindKeyOfType(options::kCustomArgs, base::Value::Type::LIST);
  if (customArgs) {
    for (const auto& customArg : customArgs->GetList()) {
      if (customArg.is_string())
        command_line->AppendArg(customArg.GetString());
    }
  }

  // Whether to enable the remote module
  if (!IsRemoteModuleEnabled())
    command_line->AppendSwitch(switches::kDisableRemoteModule);

  // Run Electron APIs and preload script in isolated world
  if (IsEnabled(options::kContextIsolation))
    command_line->AppendSwitch(switches::kContextIsolation);

  // --background-color.
  std::string s;
  if (GetAsString(&preference_, options::kBackgroundColor, &s)) {
    command_line->AppendSwitchASCII(switches::kBackgroundColor, s);
  } else if (!IsEnabled(options::kOffscreen)) {
    // For non-OSR WebContents, we expect to have white background, see
    // https://github.com/electron/electron/issues/13764 for more.
    command_line->AppendSwitchASCII(switches::kBackgroundColor, "#fff");
  }

  // --guest-instance-id, which is used to identify guest WebContents.
  int guest_instance_id = 0;
  if (GetAsInteger(&preference_, options::kGuestInstanceID, &guest_instance_id))
    command_line->AppendSwitchASCII(switches::kGuestInstanceID,
                                    base::IntToString(guest_instance_id));

  // Pass the opener's window id.
  int opener_id;
  if (GetAsInteger(&preference_, options::kOpenerID, &opener_id))
    command_line->AppendSwitchASCII(switches::kOpenerID,
                                    base::IntToString(opener_id));

#if defined(OS_MACOSX)
  // Enable scroll bounce.
  if (IsEnabled(options::kScrollBounce))
    command_line->AppendSwitch(switches::kScrollBounce);
#endif

  // Custom command line switches.
  auto* args =
      preference_.FindKeyOfType("commandLineSwitches", base::Value::Type::LIST);
  if (args) {
    for (const auto& arg : args->GetList()) {
      if (arg.is_string()) {
        const auto& arg_val = arg.GetString();
        if (!arg_val.empty())
          command_line->AppendSwitch(arg_val);
      }
    }
  }

  // Enable blink features.
  if (GetAsString(&preference_, options::kEnableBlinkFeatures, &s))
    command_line->AppendSwitchASCII(::switches::kEnableBlinkFeatures, s);

  // Disable blink features.
  if (GetAsString(&preference_, options::kDisableBlinkFeatures, &s))
    command_line->AppendSwitchASCII(::switches::kDisableBlinkFeatures, s);

  if (guest_instance_id) {
    // Webview `document.visibilityState` tracks window visibility so we need
    // to let it know if the window happens to be hidden right now.
    auto* manager = WebViewManager::GetWebViewManager(web_contents_);
    if (manager) {
      auto* embedder = manager->GetEmbedder(guest_instance_id);
      if (embedder) {
        auto* relay = NativeWindowRelay::FromWebContents(embedder);
        if (relay) {
          auto* window = relay->GetNativeWindow();
          if (window) {
            const bool visible = window->IsVisible() && !window->IsMinimized();
            if (!visible) {
              command_line->AppendSwitch(switches::kHiddenPage);
            }
          }
        }
      }
    }
  }

  // We are appending args to a webContents so let's save the current state
  // of our preferences object so that during the lifetime of the WebContents
  // we can fetch the options used to initally configure the WebContents
  last_preference_ = preference_.Clone();
}

void WebContentsPreferences::OverrideWebkitPrefs(
    content::WebPreferences* prefs) {
  prefs->javascript_enabled = IsEnabled("javascript", true /* default_value */);
  prefs->images_enabled = IsEnabled("images", true /* default_value */);
  prefs->text_areas_are_resizable =
      IsEnabled("textAreasAreResizable", true /* default_value */);
  prefs->navigate_on_drag_drop =
      IsEnabled("navigateOnDragDrop", false /* default_value */);

  // Check if webgl should be enabled.
  bool is_webgl_enabled = IsEnabled("webgl", true /* default_value */);
  prefs->webgl1_enabled = is_webgl_enabled;
  prefs->webgl2_enabled = is_webgl_enabled;

  // Check if web security should be enabled.
  bool is_web_security_enabled =
      IsEnabled(options::kWebSecurity, true /* default_value */);
  prefs->web_security_enabled = is_web_security_enabled;
  prefs->allow_running_insecure_content =
      IsEnabled(options::kAllowRunningInsecureContent,
                !is_web_security_enabled /* default_value */);

  auto* fonts_dict = preference_.FindKeyOfType("defaultFontFamily",
                                               base::Value::Type::DICTIONARY);
  if (fonts_dict) {
    base::string16 font;
    if (GetAsString(fonts_dict, "standard", &font))
      prefs->standard_font_family_map[content::kCommonScript] = font;
    if (GetAsString(fonts_dict, "serif", &font))
      prefs->serif_font_family_map[content::kCommonScript] = font;
    if (GetAsString(fonts_dict, "sansSerif", &font))
      prefs->sans_serif_font_family_map[content::kCommonScript] = font;
    if (GetAsString(fonts_dict, "monospace", &font))
      prefs->fixed_font_family_map[content::kCommonScript] = font;
    if (GetAsString(fonts_dict, "cursive", &font))
      prefs->cursive_font_family_map[content::kCommonScript] = font;
    if (GetAsString(fonts_dict, "fantasy", &font))
      prefs->fantasy_font_family_map[content::kCommonScript] = font;
  }

  int size;
  if (GetAsInteger(&preference_, "defaultFontSize", &size))
    prefs->default_font_size = size;
  if (GetAsInteger(&preference_, "defaultMonospaceFontSize", &size))
    prefs->default_fixed_font_size = size;
  if (GetAsInteger(&preference_, "minimumFontSize", &size))
    prefs->minimum_font_size = size;
  std::string encoding;
  if (GetAsString(&preference_, "defaultEncoding", &encoding))
    prefs->default_encoding = encoding;

  prefs->node_integration = IsEnabled(options::kNodeIntegration);
}

}  // namespace atom
