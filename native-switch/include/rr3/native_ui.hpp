#pragma once

namespace rr3 {

struct DashboardData;

bool nativeUiInit();
void nativeUiExit();
bool nativeUiActive();
void nativeUiRenderLoading(const char* stage, unsigned int percent);
void nativeUiRenderDashboard(const DashboardData& data, int page, const char* notice);

} // namespace rr3
