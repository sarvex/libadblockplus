/*
 * This file is part of Adblock Plus <https://adblockplus.org/>,
 * Copyright (C) 2006-present eyeo GmbH
 *
 * Adblock Plus is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Adblock Plus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Adblock Plus.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>
#include <functional>
#include <string>

#include <AdblockPlus/FilterEngineFactory.h>

#include "DefaultFilterEngine.h"
#include "JsContext.h"
#include "Thread.h"
#include "Utils.h"

using namespace AdblockPlus;

// static
std::string FilterEngineFactory::PrefNameToString(BooleanPrefName prefName)
{
  switch (prefName)
  {
  case BooleanPrefName::SynchronizationEnabled:
    return "synchronization_enabled";

  case BooleanPrefName::FirstRunSubscriptionAutoselect:
    return "first_run_subscription_auto_select";

  default:
    assert(false && "Missing case");
    return {};
  }
}

// static
std::string FilterEngineFactory::PrefNameToString(StringPrefName prefName)
{
  switch (prefName)
  {
  case StringPrefName::AllowedConnectionType:
    return "allowed_connection_type";

  default:
    assert(false && "Missing case");
    return {};
  }
}

// static
bool FilterEngineFactory::StringToPrefName(const std::string& prefNameStr,
                                           BooleanPrefName& prefName)
{ 
  if (prefNameStr == "synchronization_enabled")
  {
    prefName = BooleanPrefName::SynchronizationEnabled;
    return true;
  }

  if (prefNameStr == "first_run_subscription_auto_select")
  {
    prefName = BooleanPrefName::FirstRunSubscriptionAutoselect;
    return true;
  }

  return false;
}

// static
bool FilterEngineFactory::StringToPrefName(const std::string& prefNameStr, StringPrefName& prefName)
{
  if (prefNameStr == "allowed_connection_type")
  {
    prefName = StringPrefName::AllowedConnectionType;
    return true;
  }

  return false;
}

void FilterEngineFactory::CreateAsync(JsEngine& jsEngine,
                                      const EvaluateCallback& evaluateCallback,
                                      const OnCreatedCallback& onCreated,
                                      const CreationParameters& params)
{
  // Why wrap a unique_ptr in a shared_ptr? Because we cannot pass a
  // unique_ptr to an std::function - this would make it move-only and
  // STL doesn't like that. This is just a workaround, the function in
  // question retrieves the unique_ptr from within and keeps using that
  // or the reminder of the stack.
  auto wrappedFilterEngine =
      std::make_shared<std::unique_ptr<DefaultFilterEngine>>(new DefaultFilterEngine(jsEngine));
  auto* bareFilterEngine = wrappedFilterEngine->get();
  {
    auto isSubscriptionDownloadAllowedCallback = params.isSubscriptionDownloadAllowedCallback;
    jsEngine.SetEventCallback(
        "_isSubscriptionDownloadAllowed",
        [isSubscriptionDownloadAllowedCallback, &jsEngine](JsValueList&& params) {
          // param[0] - nullable string Prefs.allowed_connection_type
          // param[1] - function(Boolean)
          bool areArgumentsValid = params.size() == 2 &&
                                   (params[0].IsNull() || params[0].IsString()) &&
                                   params[1].IsFunction();
          assert(
              areArgumentsValid &&
              "Invalid argument: there should be two args and the second one should be a function");
          if (!areArgumentsValid)
            return;
          if (!isSubscriptionDownloadAllowedCallback)
          {
            params[1].Call(jsEngine.NewValue(true));
            return;
          }
          JsEngine::ScopedWeakValues jsFunctionWeakValue(&jsEngine, {params[1]});
          auto callJsCallback = [&jsEngine, jsFunctionWeakValue](bool isAllowed) {
            jsFunctionWeakValue.Values()[0].Call(jsEngine.NewValue(isAllowed));
          };
          std::string allowedConnectionType =
              params[0].IsString() ? params[0].AsString() : std::string();
          isSubscriptionDownloadAllowedCallback(
              params[0].IsString() ? &allowedConnectionType : nullptr, callJsCallback);
        });
  }

  jsEngine.SetEventCallback("_init",
                            [&jsEngine, wrappedFilterEngine, onCreated](JsValueList&& params) {
                              auto uniqueFilterEngine = std::move(*wrappedFilterEngine);
                              onCreated(std::move(uniqueFilterEngine));
                              jsEngine.RemoveEventCallback("_init");
                            });

  bareFilterEngine->StartObservingEvents();

  // Lock the JS engine while we are loading scripts, no timeouts should fire
  // until we are done.
  const JsContext context(jsEngine.GetIsolate(), *jsEngine.GetContext());
  // Set the preconfigured prefs
  auto preconfiguredPrefsObject = jsEngine.NewObject();
  for (const auto& pref : params.preconfiguredPrefs.booleanPrefs)
  {
    preconfiguredPrefsObject.SetProperty(PrefNameToString(pref.first), pref.second);
  }

  for (const auto& pref : params.preconfiguredPrefs.stringPrefs)
  {
    preconfiguredPrefsObject.SetProperty(PrefNameToString(pref.first), pref.second);
  }
  jsEngine.SetGlobalProperty("_preconfiguredPrefs", preconfiguredPrefsObject);

  const auto& jsFiles = Utils::SplitString(ABP_SCRIPT_FILES, ' ');
  // Load adblockplus scripts
  for (const auto& filterEngineJsFile : jsFiles)
  {
    auto filepathComponents = Utils::SplitString(filterEngineJsFile, '/');
    evaluateCallback(filepathComponents.back());
  }
}
