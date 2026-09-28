#pragma once
#include <string>
#include <vector>

enum class MonitorSettingsMatch { Missing, Unique, Ambiguous };

inline wchar_t MonitorIdUpper(wchar_t ch)
{
	return ch >= L'a' && ch <= L'z' ? ch - L'a' + L'A' : ch;
}

inline bool IsEdidMonitorId(const std::wstring& id)
{
	if (id.size() != 7) return false;
	for (size_t i = 0; i < id.size(); ++i)
	{
		const wchar_t ch = MonitorIdUpper(id[i]);
		if (i < 3 ? (ch < L'A' || ch > L'Z') :
			!((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F')))
			return false;
	}
	return true;
}

inline MonitorSettingsMatch FindMonitorSettingsKey(
	const std::vector<std::wstring>& keys, const std::wstring& monitorId,
	const std::wstring& uidSuffix, std::wstring& matchedKey)
{
	matchedKey.clear();
	if (monitorId.empty()) return MonitorSettingsMatch::Missing;
	const bool edidId = IsEdidMonitorId(monitorId);
	size_t modelMatches = 0, uidMatches = 0;
	std::wstring modelKey, uidKey;
	for (const auto& candidate : keys)
	{
		const size_t separator = candidate.find(L'^');
		if (separator == std::wstring::npos) continue;
		bool modelMatch = separator == monitorId.size() ||
			(edidId && separator > monitorId.size());
		for (size_t i = 0; modelMatch && i < monitorId.size(); ++i)
			modelMatch = MonitorIdUpper(candidate[i]) == MonitorIdUpper(monitorId[i]);
		// Real monitors append EDID identity fields before '^', e.g.
		// CSO161B24576_00_07E6_B7. Require the model at the START so that
		// MSBDD_/SIMULATED_ entries cannot masquerade as the physical monitor.
		if (modelMatch)
		{
			++modelMatches;
			modelKey = candidate;
		}
		else if (!uidSuffix.empty() && separator >= uidSuffix.size() &&
			candidate.compare(separator - uidSuffix.size(), uidSuffix.size(), uidSuffix) == 0)
		{
			++uidMatches;
			uidKey = candidate;
		}
	}
	// Prefer identity matches over the existing synthetic-monitor UID fallback.
	// Never choose arbitrarily among repeated models or stale registry entries.
	const size_t count = modelMatches ? modelMatches : uidMatches;
	if (count == 0) return MonitorSettingsMatch::Missing;
	if (count != 1) return MonitorSettingsMatch::Ambiguous;
	matchedKey = modelMatches ? modelKey : uidKey;
	return MonitorSettingsMatch::Unique;
}
